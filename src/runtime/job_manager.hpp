#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <executor/comm/channel.hpp>
#include <executor/comm/phase_gate.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_service.hpp>
#include <yori/job/job.hpp>
#include <yori/launch/launch_adapter.hpp>
#include <yori/observe/log_sink.hpp>
#include <yori/process/process_supervisor.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/recovery/job_recovery.hpp>
#include <yori/scheduler/scheduler.hpp>
#include <yori/store/state_store.hpp>

#include "gpu_manager.hpp"
#include "grace_escalation.hpp"
#include "log_pump.hpp"
#include "log_streamer.hpp"
#include "process_exit_monitor.hpp"
#include "scheduler_task_runner.hpp"
#include "store_task_runner.hpp"

namespace yori::runtime {

// ---------------------------------------------------------------------------
// JobManager（M7 守护总装收口）：daemon 侧唯一的 Job 生命周期写者。
//
// 承载与边界：
// - 单个 Executor blocking worker（命令通道 + 唤醒管道，沿用 LogFollowService
//   /ProcessExitMonitor 模式）串行处理全部状态变更：submit（创建 + 准入 +
//   触发调度）、cancel（QUEUED 终态化 / 活动态 STOPPING + SIGTERM + 宽限，
//   DEC-007）、调度结果落地（身份解析 -> LaunchPlan -> 日志目录/LogSink/
//   LogStreamer -> spawn -> STARTING->RUNNING 落盘 -> 退出监视注册 -> 日志泵
//   接入）、退出回收（终态 + lease 释放同 mutation -> EOF 发布 -> 再调度）。
// - 调度经 SchedulerTaskRunner（EXEC-06）触发并立即消费结果：queue 的全部
//   变更因此只发生在本 worker 与启动前的恢复路径，无跨线程队列访问。
// - store 写经 StoreTaskRunner（EXEC-08）；对 IPC 读路径的并发由
//   SerialStateStore 的所有权互斥串行化（外层注入同一实例）。
// - 唤醒源：命令入队、ProcessExitMonitor 事件、GpuManager 事件（均经注入
//   回调写唤醒管道；回调非阻塞）。
// - 启动编排（EXEC-09）：恢复采纳（kAdopted* Job 的 supervisor adopt +
//   退出监视注册 + STOPPING 重发取消）在 worker 启动前完成；调度仅在
//   PhaseGate 达到 kSchedulingOpen 阶段后执行。
// - 关闭（EXEC-10 ④⑤⑦）：停止接受新命令 -> 排空命令（显式 kUnavailable
//   应答）-> 运行中进程 abandon（RULE-10：不终止训练）-> 退出监视/日志泵
//   回收；终态与 lease 落盘在 worker 串行路径内完成。
//
// owner 纪律：单 owner 调用 start/stop；stop 后不可重启；析构以 stop 兜底。
// JobControl 的实现（ipc 侧经注入消费）；调用线程同步等待有界 ack。
// ---------------------------------------------------------------------------

struct JobManagerConfig final {
  // 取消宽限（DEC-007：默认 10s，[100ms, 10min]）。
  std::chrono::milliseconds cancel_grace{process::CancelPolicyLimits::kDefaultGracePeriod};
  // Job 日志根目录；每 Job 子目录 <log_root>/<job-id> 由本组件创建（0750，
  // daemon 属主；文件由 LogSink 以 0640 + Job 属主收敛）。
  std::string log_root{"/var/lib/yori/jobs"};
  // 命令通道容量（准入上限，满即显式 kUnavailable）。
  std::size_t command_capacity{256};
  // submit/cancel 的 ack 等待上限（调用侧同步等待）。
  std::chrono::milliseconds ack_timeout{5000};
  // FIFO 有界跳过的调度扫描窗口（DEC-012：默认 32，[1, 4096]）。
  std::uint32_t scheduler_scan_window{scheduler::SchedulerConfig::kDefaultScanWindow};
  // daemon 环境快照（DEC-006 白名单继承源；空则不继承任何 daemon 变量）。
  std::vector<launch::EnvironmentEntry> daemon_environment;
  // LogSink 的文件后端（测试注入写失败等故障；缺省 POSIX 实现）。
  std::shared_ptr<observe::LogIo> log_io;

  [[nodiscard]] bool valid(std::string& error) const noexcept;
};

// EXEC-09 启动阶段（PhaseGate 阶段值）。
constexpr std::uint64_t kPhaseRecovery = 1;
constexpr std::uint64_t kPhaseGpuObserved = 2;
constexpr std::uint64_t kPhaseSchedulingOpen = 3;

enum class JobManagerStartCode : std::uint8_t {
  kStarted,
  kAlreadyStarted,
  kInvalidConfig,
  kLogRootInvalid,
  kWorkersRejected,
};

[[nodiscard]] const char* to_string(JobManagerStartCode code) noexcept;

struct JobManagerStartResult final {
  JobManagerStartCode code{JobManagerStartCode::kInvalidConfig};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == JobManagerStartCode::kStarted; }
};

enum class JobManagerStopCode : std::uint8_t {
  kStopped,
  kNotRunning,
  kAlreadyStopped,
};

[[nodiscard]] const char* to_string(JobManagerStopCode code) noexcept;

struct JobManagerStats final {
  std::uint64_t commands_processed{0};
  std::uint64_t jobs_submitted{0};
  std::uint64_t jobs_launched{0};
  std::uint64_t launch_failures{0};
  std::uint64_t jobs_finished{0};
  std::uint64_t jobs_failed{0};
  std::uint64_t jobs_cancelled{0};
  std::uint64_t jobs_adopted{0};
  std::uint64_t recancels_armed{0};
  std::uint64_t scheduler_runs{0};
  std::uint64_t scheduler_scheduled{0};
  std::uint64_t scheduler_failed{0};
  std::uint64_t store_write_failures{0};
  std::uint64_t abandoned_at_stop{0};
  std::uint64_t active_supervised{0};
};

class JobManager final : public ipc::JobControl, public ipc::ScheduleStatusSource {
 public:
  // identity_resolver/launch_adapter 的生命周期由调用方保证覆盖本组件
  // （生产：Daemon 持有 PosixIdentityResolver + DefaultLaunchAdapter）。
  JobManager(executor::Executor& executor, store::StateStore& store, queue::GlobalJobQueue& queue,
             GpuManager& gpu_manager, LogStreamer& log_streamer,
             launch::IdentityResolver& identity_resolver, launch::LaunchAdapter& launch_adapter,
             executor::comm::PhaseGate& startup_gate, JobManagerConfig config = {});
  ~JobManager();

  JobManager(const JobManager&) = delete;
  JobManager& operator=(const JobManager&) = delete;
  JobManager(JobManager&&) = delete;
  JobManager& operator=(JobManager&&) = delete;

  // 启动退出监视、日志泵与守护 worker；recovery 的采纳动作（kAdopted* Job：
  // supervisor adopt + 退出监视注册 + STOPPING 重发取消）在 worker 启动前
  // 完成。恢复结果来自 Daemon 启动序（JobRecovery 已落盘）；nullopt 表示
  // 无恢复上下文（测试）。
  [[nodiscard]] JobManagerStartResult start(
      const std::optional<recovery::RecoveryResult>& recovery = std::nullopt);

  // EXEC-10 ④⑤⑦（守护子集）：见类注释。幂等。
  [[nodiscard]] JobManagerStopCode stop();

  // ipc::JobControl（IPC worker 线程调用；同步等待有界 ack）。
  [[nodiscard]] ipc::JobSubmitOutcome submit_job(const job::JobSpec& spec) override;
  [[nodiscard]] ipc::JobCancelOutcome cancel_job(std::uint64_t job_id) override;

  // ipc::ScheduleStatusSource（DEC-012）：最近一次调度评估结论（comm 最新值
  // 视图，跨线程读取安全）。尚无评估时返回 false。
  [[nodiscard]] bool try_get_schedule_evaluation(scheduler::ScheduleEvaluation& out) override;

  [[nodiscard]] JobManagerStats stats() const;

  // 测试与总装观察口。
  [[nodiscard]] ProcessExitMonitor& exit_monitor();
  [[nodiscard]] LogPump& log_pump();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
