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
               store::StateStore& store, DaemonConfig config)
    : executor_(executor),
      gpu_provider_(gpu_provider),
      store_(store),
      config_(std::move(config)) {}

Daemon::~Daemon() {
  static_cast<void>(stop());
}

const std::optional<recovery::RecoveryResult>& Daemon::last_recovery() const noexcept {
  return last_recovery_;
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

  // 1) 恢复：RULE-06，绝不无条件重启活动 Job；失败则拒绝启动。
  queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone;
  queue_ = queue::GlobalJobQueue::create(config_.queue, queue_error);
  if (queue_ == nullptr) {
    return start_failure(DaemonStartCode::kInvalidConfig,
                         std::string("queue creation failed: ") + queue::to_string(queue_error));
  }
  recovery::JobRecovery recovery(store_, *queue_);
  recovery::RecoveryResult recovery_result = recovery.recover();
  last_recovery_ = recovery_result;
  if (!recovery_result.ok()) {
    queue_.reset();
    return DaemonStartResult{
        DaemonStartCode::kRecoveryFailed,
        std::string("recovery failed: ") + recovery::to_string(recovery_result.code),
        recovery_result};
  }

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
  gpu_status_ = std::make_unique<ManagerGpuStatusSource>(*gpu_manager_);

  // 3) IPC 服务（EXEC-02）。
  log_reader_ = ipc::file_log_snapshot_reader();
  service_ = std::make_unique<ipc::IpcService>(config_.service, *queue_, store_, *gpu_status_,
                                               *log_reader_);
  ipc_server_ = std::make_unique<UdsIpcServer>(executor_, *service_, config_.ipc);
  const ipc::IpcTransportStartResult ipc_start = ipc_server_->start();
  if (!ipc_start.ok()) {
    ipc_server_.reset();
    service_.reset();
    log_reader_.reset();
    gpu_status_.reset();
    static_cast<void>(gpu_manager_->stop());
    gpu_manager_.reset();
    queue_.reset();
    return start_failure(DaemonStartCode::kIpcFailed,
                         std::string("ipc server start failed: ") +
                             ipc::to_string(ipc_start.code) +
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

  // EXEC-10 适用子集：① 停止 IPC（新连接与请求生产者）-> ③ 停止 GPU 周期
  // 任务。M5 无调度生产者与在飞守护任务；store 写路径在 IPC worker 内同步
  // 完成，IPC join 后无未落盘写入。
  ipc_server_->stop();
  ipc_server_.reset();
  service_.reset();
  log_reader_.reset();
  static_cast<void>(gpu_manager_->stop());
  gpu_status_.reset();
  gpu_manager_.reset();
  queue_.reset();
  started_ = false;
  return DaemonStopCode::kStopped;
}

}  // namespace yori::runtime
