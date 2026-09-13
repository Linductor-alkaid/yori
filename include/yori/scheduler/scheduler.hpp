#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/job/job.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/store/state_store.hpp>

namespace yori::scheduler {

enum class SchedulerTrigger {
  kJobSubmitted,
  kJobExited,
  kJobCancelled,
  kGpuStateChanged,
  kRecoveryCompleted,
  kAdminStateChanged,
};

[[nodiscard]] const char* to_string(SchedulerTrigger trigger) noexcept;

// FIFO 有界跳过的扫描窗口配置（DEC-012 决策 4）。窗口只限制单次 run_once
// 考察的队首连续条数，不影响队列容量与 FIFO 服务顺序。
struct SchedulerConfig final {
  static constexpr std::uint32_t kDefaultScanWindow = 32;
  static constexpr std::uint32_t kMaxScanWindow =
      static_cast<std::uint32_t>(queue::QueueConfig::kMaxCapacity);

  std::uint32_t scan_window{kDefaultScanWindow};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return scan_window >= 1 && scan_window <= kMaxScanWindow;
  }
};

// QUEUED Job 的等待原因（DEC-012 决策 5）：最近一次调度评估对被跳过 Job 的
// 结论，daemon 派生视图，不持久化。
enum class WaitReason : std::uint8_t {
  kNone = 0,
  kNoFreeGpu = 1,             // 全局无空闲 GPU（kAny）
  kAffinityGpuAllocated = 2,  // kRequired 目标被 Yori lease
  kAffinityGpuExternal = 3,   // kRequired 目标被外部进程占用
  kAffinityGpuState = 4,      // kRequired 目标 UNAVAILABLE 或观测缺失
};

[[nodiscard]] const char* to_string(WaitReason reason) noexcept;

// 单个被跳过 Job 的结构化跳过事件：原因 +（kRequired 时）目标设备。
struct ScheduleSkip final {
  job::JobId job{};
  WaitReason reason{WaitReason::kNone};
  std::optional<gpu::GpuUuid> target;
};

// 一次 run_once 的调度评估结论（可观察、不静默）：窗口内被跳过的 Job 与
// 窗口截断标记（队列长于扫描窗口时其后 Job 本轮未被考察）。
struct ScheduleEvaluation final {
  std::vector<ScheduleSkip> skipped;
  bool window_truncated{false};
};

enum class ScheduleResultCode {
  kScheduled,
  kQueueEmpty,
  kNoCandidate,
  kCancelled,
  kInvalidGpuSnapshot,
  kStateLoadFailed,
  kQueueStateDiverged,
  kStateWriteFailed,
  kQueueRollbackFailed,
};

[[nodiscard]] const char* to_string(ScheduleResultCode code) noexcept;

struct SchedulerEvent final {
  SchedulerTrigger trigger{SchedulerTrigger::kJobSubmitted};
  ScheduleResultCode code{ScheduleResultCode::kQueueEmpty};
  std::optional<job::JobId> job_id;
  std::optional<gpu::GpuUuid> gpu_uuid;
  gpu::GpuObservationValidationResult gpu_validation{};
  store::StateStoreErrorCode store_error{store::StateStoreErrorCode::kNone};
  queue::QueueErrorCode queue_error{queue::QueueErrorCode::kNone};
  std::uint64_t store_revision{0};
};

struct ScheduleResult final {
  ScheduleResultCode code{ScheduleResultCode::kQueueEmpty};
  SchedulerEvent event;
  // 本轮评估：被跳过 Job 与窗口截断标记（kScheduled/kNoCandidate 携带）。
  ScheduleEvaluation evaluation;

  [[nodiscard]] constexpr bool scheduled() const noexcept {
    return code == ScheduleResultCode::kScheduled;
  }
  [[nodiscard]] constexpr bool failed() const noexcept {
    return code == ScheduleResultCode::kInvalidGpuSnapshot ||
           code == ScheduleResultCode::kStateLoadFailed ||
           code == ScheduleResultCode::kQueueStateDiverged ||
           code == ScheduleResultCode::kStateWriteFailed ||
           code == ScheduleResultCode::kQueueRollbackFailed;
  }
};

// 单 owner、事件驱动的 FIFO 调度 Core（DEC-005 + DEC-012 有界跳过修订）。
// 一次调用只处理一个触发事件：按 (submit_time, JobId) 顺序扫描队首开始的
// 有界窗口，跳过当前不可调度的 Job（placement 候选集过滤 + FREE/lease 排除），
// 把窗口内第一个可调度 Job 原子推进到 STARTING 并建立一个 GPU lease；被跳过
// Job 保持 QUEUED 与原队列位置，结论以 ScheduleEvaluation 返回。
class FifoScheduler final {
 public:
  FifoScheduler(queue::GlobalJobQueue& queue, store::StateStore& store, SchedulerConfig config = {})
      : queue_(queue), store_(store), config_(config) {}

  [[nodiscard]] ScheduleResult run_once(SchedulerTrigger trigger,
                                        const gpu::GpuObservationSnapshot& gpu_snapshot);

  // Executor adapter 在任务开始前观察到 StopToken 后使用此结果；取消不会触碰
  // Job 状态、队列或 lease。
  [[nodiscard]] static ScheduleResult cancelled(SchedulerTrigger trigger) noexcept;

 private:
  queue::GlobalJobQueue& queue_;
  store::StateStore& store_;
  SchedulerConfig config_;
};

}  // namespace yori::scheduler
