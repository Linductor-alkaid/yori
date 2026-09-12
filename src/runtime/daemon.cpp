#include "runtime/daemon.hpp"

#include <string>
#include <utility>

namespace yori::runtime {
namespace {

DaemonStartResult start_failure(DaemonStartCode code, std::string message) {
  return DaemonStartResult{code, std::move(message), std::nullopt};
}

}  // namespace

const char* to_string(DaemonStartCode code) noexcept {
  switch (code) {
    case DaemonStartCode::kStarted:
      return "started";
    case DaemonStartCode::kAlreadyStarted:
      return "already started";
    case DaemonStartCode::kInvalidConfig:
      return "invalid config";
    case DaemonStartCode::kRecoveryFailed:
      return "recovery failed";
    case DaemonStartCode::kGpuFailed:
      return "gpu observation failed";
    case DaemonStartCode::kJobManagerFailed:
      return "job manager failed";
    case DaemonStartCode::kIpcFailed:
      return "ipc server failed";
  }
  return "unknown";
}

const char* to_string(DaemonStopCode code) noexcept {
  switch (code) {
    case DaemonStopCode::kStopped:
      return "stopped";
    case DaemonStopCode::kNotRunning:
      return "not running";
    case DaemonStopCode::kAlreadyStopped:
      return "already stopped";
  }
  return "unknown";
}

Daemon::Daemon(executor::Executor& executor, gpu::GpuProvider& gpu_provider,
               std::unique_ptr<store::StateStore> store, DaemonConfig config)
    : executor_(executor),
      gpu_provider_(gpu_provider),
      store_(std::move(store)),
      config_(std::move(config)) {
  launch_adapter_.set_daemon_environment(config_.job_manager.daemon_environment);
}

Daemon::~Daemon() { static_cast<void>(stop()); }

const std::optional<recovery::RecoveryResult>& Daemon::last_recovery() const noexcept {
  return last_recovery_;
}

LogStreamer& Daemon::log_streamer() noexcept { return *log_streamer_; }

LogFollowStatistics Daemon::log_follow_statistics() const {
  return log_follow_ != nullptr ? log_follow_->statistics() : LogFollowStatistics{};
}

JobManagerStats Daemon::job_manager_stats() const {
  return job_manager_ != nullptr ? job_manager_->stats() : JobManagerStats{};
}

DaemonStartResult Daemon::start() {
  if (started_) {
    return start_failure(DaemonStartCode::kAlreadyStarted, "daemon already started");
  }
  if (stop_requested_) {
    return start_failure(DaemonStartCode::kInvalidConfig, "daemon was stopped and cannot restart");
  }
  if (!config_.service.valid()) {
    return start_failure(DaemonStartCode::kInvalidConfig, "invalid ipc service config");
  }
  std::string ipc_validation_error;
  if (!config_.ipc.valid(ipc_validation_error)) {
    return start_failure(DaemonStartCode::kInvalidConfig, ipc_validation_error);
  }
  std::string job_manager_validation_error;
  if (!config_.job_manager.valid(job_manager_validation_error)) {
    return start_failure(DaemonStartCode::kInvalidConfig,
                         "invalid job manager config: " + job_manager_validation_error);
  }
  if (store_ == nullptr) {
    return start_failure(DaemonStartCode::kInvalidConfig, "state store is required");
  }

  // 1) 恢复（gate 阶段 kPhaseRecovery 之前完成，RULE-06）。
  queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone;
  queue_ = queue::GlobalJobQueue::create(config_.queue, queue_error);
  if (queue_ == nullptr) {
    return start_failure(DaemonStartCode::kInvalidConfig,
                         std::string("queue creation failed: ") + queue::to_string(queue_error));
  }
  recovery::JobRecovery recovery(*store_, *queue_);
  recovery::RecoveryResult recovery_result = recovery.recover();
  last_recovery_ = recovery_result;
  if (!recovery_result.ok()) {
    queue_.reset();
    return DaemonStartResult{
        DaemonStartCode::kRecoveryFailed,
        std::string("recovery failed: ") + recovery::to_string(recovery_result.code),
        recovery_result};
  }
  static_cast<void>(startup_gate_.advance_to(kPhaseRecovery));

  // 2) GPU 观察：初始观测同步完成，失败则拒绝启动（`yori gpu` 数据源）。
  gpu_manager_ = std::make_unique<GpuManager>(executor_, gpu_provider_, config_.gpu);
  const GpuManagerStartResult gpu_start = gpu_manager_->start();
  if (!gpu_start.ok()) {
    queue_.reset();
    gpu_manager_.reset();
    return start_failure(DaemonStartCode::kGpuFailed,
                         std::string("gpu manager start failed: ") + to_string(gpu_start.code) +
                             (gpu_start.message.empty() ? "" : ": " + gpu_start.message));
  }
  static_cast<void>(startup_gate_.advance_to(kPhaseGpuObserved));
  gpu_status_ = std::make_unique<ManagerGpuStatusSource>(*gpu_manager_);

  // 3) 观察面（M6）：LogStreamer（Topic/回看窗口/准入）先于会话 worker，
  //     后者先于 IPC（承接 LOGS_FOLLOW 的 fd 移交）。
  std::string streamer_error;
  if (!config_.log_streamer.valid(streamer_error)) {
    gpu_status_.reset();
    static_cast<void>(gpu_manager_->stop());
    gpu_manager_.reset();
    queue_.reset();
    return start_failure(DaemonStartCode::kInvalidConfig,
                         std::string("log streamer config invalid: ") + streamer_error);
  }
  log_streamer_ = std::make_unique<LogStreamer>(config_.log_streamer);

  // 4) 守护承载（M7）：调度开启前启动（gate 阶段 kPhaseSchedulingOpen 的
  //     消费者）；恢复采纳在 worker 启动前完成。
  job_manager_ = std::make_unique<JobManager>(executor_, *store_, *queue_, *gpu_manager_,
                                              *log_streamer_, identity_resolver_, launch_adapter_,
                                              startup_gate_, config_.job_manager);
  static_cast<void>(startup_gate_.advance_to(kPhaseSchedulingOpen));
  const JobManagerStartResult job_manager_start = job_manager_->start(recovery_result);
  if (!job_manager_start.ok()) {
    job_manager_.reset();
    log_streamer_.reset();
    gpu_status_.reset();
    static_cast<void>(gpu_manager_->stop());
    gpu_manager_.reset();
    queue_.reset();
    return start_failure(
        DaemonStartCode::kJobManagerFailed,
        std::string("job manager start failed: ") + to_string(job_manager_start.code) +
            (job_manager_start.message.empty() ? "" : ": " + job_manager_start.message));
  }

  log_reader_ = ipc::file_log_snapshot_reader();
  // ScheduleStatusSource 由 JobManager 直接实现（DEC-012：comm 最新值视图）。
  service_ = std::make_unique<ipc::IpcService>(config_.service, *store_, *gpu_status_, *log_reader_,
                                               *job_manager_, *job_manager_);
  log_follow_ =
      std::make_unique<LogFollowService>(executor_, *service_, *log_streamer_, config_.log_follow);
  // Topic 无 fd 可 poll：发布/终态经变更监听显式唤醒会话 worker（EXEC-03/04）。
  log_streamer_->set_change_listener([this] { log_follow_->notify(); });
  const LogFollowStartResult follow_start = log_follow_->start();
  if (!follow_start.ok()) {
    log_follow_.reset();
    service_.reset();
    log_reader_.reset();
    static_cast<void>(job_manager_->stop());
    job_manager_.reset();
    log_streamer_.reset();
    gpu_status_.reset();
    static_cast<void>(gpu_manager_->stop());
    gpu_manager_.reset();
    queue_.reset();
    return start_failure(DaemonStartCode::kIpcFailed,
                         std::string("log follow service start failed: ") +
                             to_string(follow_start.code) +
                             (follow_start.message.empty() ? "" : ": " + follow_start.message));
  }

  // 5) IPC 服务（EXEC-02）；LOGS_FOLLOW 委派给观察面（EXEC-03）。
  ipc_server_ =
      std::make_unique<UdsIpcServer>(executor_, *service_, config_.ipc, log_follow_.get());
  const ipc::IpcTransportStartResult ipc_start = ipc_server_->start();
  if (!ipc_start.ok()) {
    ipc_server_.reset();
    log_follow_.reset();
    service_.reset();
    log_reader_.reset();
    static_cast<void>(job_manager_->stop());
    job_manager_.reset();
    log_streamer_.reset();
    gpu_status_.reset();
    static_cast<void>(gpu_manager_->stop());
    gpu_manager_.reset();
    queue_.reset();
    return start_failure(DaemonStartCode::kIpcFailed,
                         std::string("ipc server start failed: ") + ipc::to_string(ipc_start.code) +
                             (ipc_start.message.empty() ? "" : ": " + ipc_start.message));
  }

  started_ = true;
  return DaemonStartResult{DaemonStartCode::kStarted, {}, recovery_result};
}

DaemonStopCode Daemon::stop() {
  if (stop_requested_) {
    return DaemonStopCode::kAlreadyStopped;
  }
  stop_requested_ = true;
  if (!started_) {
    return DaemonStopCode::kNotRunning;
  }

  // EXEC-10 完整顺序：① IPC -> ② 跟随会话 -> ③ GPU 周期任务 -> ④ 守护承载
  // （abandon 运行中训练进程，RULE-10；JobManager 内部含 ⑤ 退出监视/日志泵
  // 回收与 ⑦ 终态落盘排空）。
  ipc_server_->stop();
  ipc_server_.reset();
  log_streamer_->set_change_listener(nullptr);  // 先解除回调，再回收 worker。
  log_follow_.reset();
  service_.reset();
  log_reader_.reset();
  static_cast<void>(gpu_manager_->stop());
  static_cast<void>(job_manager_->stop());
  job_manager_.reset();
  gpu_status_.reset();
  gpu_manager_.reset();
  queue_.reset();
  started_ = false;
  return DaemonStopCode::kStopped;
}

}  // namespace yori::runtime
