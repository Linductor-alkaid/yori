#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <executor/comm/channel.hpp>
#include <executor/comm/double_buffer.hpp>
#include <memory>
#include <string>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>

namespace executor {
class Executor;
}

namespace yori::runtime {

enum class GpuManagerEventKind : std::uint8_t {
  // 观测状态迁移（设备出现/消失/状态翻转）；遥测-only 波动不产生事件。
  kGpuStateChanged,
  // 连续采样失败的开始（第一条失败）与结束（恢复成功）。
  kErrorStreakStarted,
  kErrorStreakEnded,
};

[[nodiscard]] const char* to_string(GpuManagerEventKind kind) noexcept;

struct GpuManagerEvent final {
  GpuManagerEventKind kind{GpuManagerEventKind::kGpuStateChanged};
  std::uint64_t revision{0};
  // kErrorStreakStarted 时为本次失败的 provider 错误码。
  gpu::GpuProviderErrorCode error{gpu::GpuProviderErrorCode::kNone};
  // 错误事件携带的当前连续失败长度。
  std::uint64_t error_streak{0};
  // kGpuStateChanged 时发生迁移的设备 UUID（含出现/消失）。
  std::vector<gpu::GpuUuid> changed;
  // 该事件之前存在被背压丢弃的事件（补投标记，消费方须重新拉取快照）。
  bool dropped_earlier_events{false};
};

struct GpuManagerStats final {
  std::uint64_t ticks{0};
  std::uint64_t ticks_ok{0};
  // provider 错误 + 非法快照 + tick 内异常之和。
  std::uint64_t ticks_failed{0};
  std::uint64_t tick_exceptions{0};
  std::uint64_t snapshots_published{0};
  std::uint64_t publish_rejected{0};
  std::uint64_t state_change_events{0};
  std::uint64_t events_dropped{0};
  std::uint64_t skipped_overlapping_ticks{0};
  std::uint64_t error_streak{0};
  gpu::GpuProviderErrorCode last_error{gpu::GpuProviderErrorCode::kNone};
  std::uint64_t last_good_revision{0};
};

struct GpuManagerConfig final {
  // 采样周期（允许抖动的后台周期任务，EXEC-05）。
  std::chrono::milliseconds sample_period{5000};
  // 状态变化/错误事件通道容量（有界，满即拒绝并计数 + 补投）。
  std::size_t event_capacity{64};
  // stop() 等待在途 tick 归零的上限。
  std::chrono::milliseconds stop_join_timeout{10000};
};

enum class GpuManagerStartCode {
  kStarted,
  kAlreadyStarted,
  kStopped,
  kInvalidConfig,
  kInitialObservationFailed,
  kExecutorRejected,
};

[[nodiscard]] const char* to_string(GpuManagerStartCode code) noexcept;

struct GpuManagerStartResult final {
  GpuManagerStartCode code{GpuManagerStartCode::kExecutorRejected};
  gpu::GpuProviderErrorCode provider_error{gpu::GpuProviderErrorCode::kNone};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == GpuManagerStartCode::kStarted; }
};

enum class GpuManagerStopCode {
  kStopped,
  kNotRunning,
  kAlreadyStopped,
  kJoinTimedOut,
};

[[nodiscard]] const char* to_string(GpuManagerStopCode code) noexcept;

struct GpuManagerStopResult final {
  GpuManagerStopCode code{GpuManagerStopCode::kNotRunning};
  std::string message;

  [[nodiscard]] bool ok() const noexcept {
    return code == GpuManagerStopCode::kStopped || code == GpuManagerStopCode::kNotRunning ||
           code == GpuManagerStopCode::kAlreadyStopped;
  }
};

// GPU 遥测采样与外部占用扫描的 Executor 承载（总计划 EXEC-05）：start 先在
// owner 线程同步完成首次观测并建立基线，再经 submit_periodic_with_handle 提交
// 周期采样；每次 tick 校验快照、经 DoubleBuffer 发布（EXEC-09 的 GPU 快照部分）
// 并在观测状态迁移时向有界事件通道投递调度触发事件。遥测-only 波动不触发事件
// （RULE-05/RULE-07：lease 是调度事实，观测不驱动高频调度）。
//
// owner 纪律：单 owner 调用 start/stop/事件消费；provider 生命周期由 owner
// 保证覆盖 manager 与任何在途 tick（stop() 正常 join 后无在途 tick；join 超时
// 返回 kJoinTimedOut，此时不得销毁 provider）。stop 后不可重启。
class GpuManager final {
 public:
  GpuManager(executor::Executor& executor, gpu::GpuProvider& provider,
             GpuManagerConfig config = {});
  ~GpuManager();

  GpuManager(const GpuManager&) = delete;
  GpuManager& operator=(const GpuManager&) = delete;
  GpuManager(GpuManager&&) = delete;
  GpuManager& operator=(GpuManager&&) = delete;

  // 首次同步观测成功后提交周期任务。初始观测失败返回
  // kInitialObservationFailed（可修正后重试 start）；Executor 拒绝提交返回
  // kExecutorRejected（同样可重试）。
  [[nodiscard]] GpuManagerStartResult start();

  // 取消周期任务（关闭阶段 ③）并有界等待在途 tick 结束；以 Executor 定时器
  // 状态的 active_callback_count 为事实源。幂等。
  [[nodiscard]] GpuManagerStopResult stop();

  // 读取最新有效快照（EXEC-09 DoubleBuffer）。尚无观测时返回 false。
  [[nodiscard]] bool try_get_snapshot(gpu::GpuObservationSnapshot& out);

  // 事件消费（单消费者）。stop 之后缓冲事件仍可读取，析构前应排空。
  [[nodiscard]] bool try_receive_event(GpuManagerEvent& out);
  [[nodiscard]] bool receive_event_for(GpuManagerEvent& out, std::chrono::milliseconds timeout);

  [[nodiscard]] GpuManagerStats stats() const;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace yori::runtime
