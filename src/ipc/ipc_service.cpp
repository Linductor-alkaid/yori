#include <yori/ipc/ipc_service.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include <yori/observe/log_sink.hpp>

namespace yori::ipc {
namespace {

constexpr std::uint8_t job_state_wire(job::JobState state) noexcept {
  return static_cast<std::uint8_t>(state);
}

constexpr std::uint8_t gpu_observed_wire(gpu::GpuObservedState state) noexcept {
  return static_cast<std::uint8_t>(state);
}

constexpr std::uint8_t gpu_logical_wire(gpu::GpuLogicalState state) noexcept {
  return static_cast<std::uint8_t>(state);
}

std::uint64_t unix_ns(std::chrono::system_clock::time_point time) {
  const auto duration = time.time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

const store::StoredJob* find_job(const store::StateSnapshot& snapshot, std::uint64_t job_id) {
  const job::JobId id{job_id};
  for (const auto& record : snapshot.jobs) {
    if (record.id == id) {
      return &record;
    }
  }
  return nullptr;
}

// 文件日志尾部读取（基线 8/23：O_NOFOLLOW，非常规文件拒绝）。
class FileLogSnapshotReader final : public LogSnapshotReader {
 public:
  [[nodiscard]] LogTailResult read_tail(const std::string& path, std::uint32_t max_bytes) override {
    if (max_bytes == 0) {
      return LogTailResult{true, false, {}};
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      // 文件尚未创建：该流还没有输出，视为空尾部。
      if (errno == ENOENT || errno == ENOTDIR) {
        return LogTailResult{true, false, {}};
      }
      return LogTailResult{false, false, {}};
    }

    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
      static_cast<void>(::close(fd));
      return LogTailResult{false, false, {}};
    }

    const auto file_size = static_cast<std::uint64_t>(status.st_size);
    const bool truncated = file_size > max_bytes;
    if (truncated && ::lseek(fd, static_cast<off_t>(file_size - max_bytes), SEEK_SET) < 0) {
      static_cast<void>(::close(fd));
      return LogTailResult{false, false, {}};
    }

    LogTailResult result;
    result.ok = true;
    result.truncated = truncated;
    result.tail.resize(max_bytes);
    std::size_t received = 0;
    while (received < result.tail.size()) {
      const ssize_t n = ::read(fd, result.tail.data() + received, result.tail.size() - received);
      if (n > 0) {
        received += static_cast<std::size_t>(n);
        continue;
      }
      if (n == 0) {
        break;
      }
      if (errno != EINTR) {
        static_cast<void>(::close(fd));
        return LogTailResult{false, false, {}};
      }
    }
    result.tail.resize(received);
    static_cast<void>(::close(fd));
    return result;
  }
};

}  // namespace

bool IpcServiceConfig::valid() const noexcept {
  return max_listed_jobs > 0 && max_listed_jobs <= IpcProtocolLimits::kMaxListItems &&
         max_log_tail_bytes > 0 && max_log_tail_bytes <= IpcProtocolLimits::kMaxLogTailBytes;
}

std::unique_ptr<LogSnapshotReader> file_log_snapshot_reader() {
  return std::make_unique<FileLogSnapshotReader>();
}

IpcService::IpcService(IpcServiceConfig config, queue::GlobalJobQueue& queue,
                       store::StateStore& store, GpuStatusSource& gpu_source,
                       LogSnapshotReader& log_reader)
    : config_(config),
      queue_(queue),
      store_(store),
      gpu_source_(gpu_source),
      log_reader_(log_reader) {}

bool IpcService::is_admin(const PeerCredentials& peer) const noexcept {
  for (const std::uint32_t gid : config_.admin_gids) {
    if (peer.gid == gid) {
      return true;
    }
  }
  for (const std::uint32_t uid : config_.admin_uids) {
    if (peer.uid == uid) {
      return true;
    }
  }
  return false;
}

IpcResponse IpcService::error_response(IpcRequestKind kind, IpcError error, std::string detail) {
  IpcResponse response;
  response.kind = kind;
  response.error = error;
  response.detail = std::move(detail);
  return response;
}

IpcResponse IpcService::handle(const PeerCredentials& peer, const IpcRequest& request) {
  switch (request.kind) {
    case IpcRequestKind::kSubmit:
      return handle_submit(peer, request.submit);
    case IpcRequestKind::kPs:
      return handle_ps(peer);
    case IpcRequestKind::kQueue:
      return handle_queue(peer);
    case IpcRequestKind::kGpu:
      return handle_gpu();
    case IpcRequestKind::kCancel:
      return handle_cancel(peer, request.cancel.job_id);
    case IpcRequestKind::kLogs:
      return handle_logs(peer, request.logs);
  }
  return error_response(request.kind, IpcError::kInternal, "unhandled request kind");
}

IpcResponse IpcService::handle_submit(const PeerCredentials& peer,
                                      const IpcSubmitRequest& request) {
  // 身份只来自 SO_PEERCRED：请求结构不存在可消费的客户端身份字段
  //（DEC-010、威胁模型基线 2）。
  job::JobSpec spec;
  spec.owner_uid = peer.uid;
  spec.owner_gid = peer.gid;
  spec.argv = request.argv;
  spec.cwd = request.cwd;
  spec.env = request.env;
  spec.gpu_request = request.gpu_request;
  spec.launch_profile = request.launch_profile;
  spec.tensorboard_logdir = request.tensorboard_logdir;
  spec.submit_time = std::chrono::system_clock::now();

  const job::JobSpecValidationResult validation = job::validate(spec);
  if (!validation.ok()) {
    if (validation.code == job::JobSpecErrorCode::kRootOwnerNotAllowed) {
      return error_response(IpcRequestKind::kSubmit, IpcError::kDenied, "root cannot submit jobs");
    }
    return error_response(IpcRequestKind::kSubmit, IpcError::kInvalidSpec,
                          job::to_string(validation.code));
  }

  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kSubmit, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }

  // JobId 服务器级单调分配：终态 id 不复用（快照最大值 + 1）。
  std::uint64_t max_id = 0;
  for (const auto& record : load.snapshot.jobs) {
    max_id = std::max(max_id, record.id.value());
  }

  store::StoredJob record;
  record.id = job::JobId{max_id + 1};
  record.spec = std::move(spec);
  record.state = job::JobState::kQueued;
  record.revision = 0;

  store::StateMutation mutation;
  mutation.expected_revision = load.snapshot.revision;
  mutation.create_jobs.push_back(record);

  const store::StateStoreWriteResult write = store_.apply(mutation);
  if (!write.ok()) {
    return error_response(IpcRequestKind::kSubmit, IpcError::kStoreFailed,
                          store::to_string(write.code));
  }

  const queue::QueueOperationResult admission = queue_.admit(record);
  if (!admission.ok()) {
    // 容量拒绝显式回滚：持久化 CANCELLED 保留审计事实（revision 递增，
    // QUEUED -> CANCELLED 合法转换；spec 原样）。回滚本身失败时并入 detail，
    // 不静默吞掉不一致。
    store::StoredJob cancelled = record;
    cancelled.state = job::JobState::kCancelled;
    cancelled.revision = 1;
    store::StateMutation rollback;
    rollback.expected_revision = write.revision;
    rollback.update_jobs.push_back(std::move(cancelled));
    const store::StateStoreWriteResult rollback_write = store_.apply(rollback);

    std::string detail = queue::to_string(admission.code);
    if (!rollback_write.ok()) {
      detail += std::string("; rollback failed: ") + store::to_string(rollback_write.code);
    }
    return error_response(IpcRequestKind::kSubmit, IpcError::kQueueRejected, std::move(detail));
  }

  IpcResponse response;
  response.kind = IpcRequestKind::kSubmit;
  response.error = IpcError::kNone;
  response.job_id = record.id.value();
  return response;
}

IpcResponse IpcService::handle_ps(const PeerCredentials& peer) {
  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kPs, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }

  const bool admin = is_admin(peer);
  const bool truncate = load.snapshot.jobs.size() > config_.max_listed_jobs;

  IpcResponse response;
  response.kind = IpcRequestKind::kPs;
  response.error = truncate ? IpcError::kLimit : IpcError::kNone;
  if (truncate) {
    response.detail = "listing truncated to " + std::to_string(config_.max_listed_jobs) + " jobs";
  }

  // 快照按 JobId 有序（StateStore map 序）；截断保留前缀。
  const std::size_t listed =
      std::min(load.snapshot.jobs.size(), static_cast<std::size_t>(config_.max_listed_jobs));
  response.jobs.reserve(listed);
  for (std::size_t i = 0; i < listed; ++i) {
    const store::StoredJob& record = load.snapshot.jobs[i];
    // 脱敏（设计 11.5）：非 owner 非 admin 仅暴露 JobId/状态/revision/owner/
    // 退出状态；argv/cwd/tensorboard_logdir 置空并打 masked 标记。
    const bool masked = record.spec.owner_uid != peer.uid && !admin;
    IpcJobSummary summary;
    summary.job_id = record.id.value();
    summary.state = job_state_wire(record.state);
    summary.owner_uid = record.spec.owner_uid;
    summary.revision = record.revision;
    summary.masked = masked;
    if (!masked) {
      summary.argv = record.spec.argv;
      summary.cwd = record.spec.cwd;
      summary.tensorboard_logdir = record.spec.tensorboard_logdir;
    }
    if (record.execution.exit) {
      summary.exit = IpcExitStatus{record.execution.exit->exited(),
                                   record.execution.exit->exited()
                                       ? record.execution.exit->exit_code
                                       : record.execution.exit->signal_number};
    }
    response.jobs.push_back(std::move(summary));
  }
  return response;
}

IpcResponse IpcService::handle_queue(const PeerCredentials& peer) {
  static_cast<void>(peer);
  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kQueue, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }

  const auto& entries = queue_.entries();
  const bool truncate = entries.size() > config_.max_listed_jobs;

  IpcResponse response;
  response.kind = IpcRequestKind::kQueue;
  response.error = truncate ? IpcError::kLimit : IpcError::kNone;
  if (truncate) {
    response.detail = "listing truncated to " + std::to_string(config_.max_listed_jobs) + " jobs";
  }

  const std::size_t listed =
      std::min(entries.size(), static_cast<std::size_t>(config_.max_listed_jobs));
  response.queue.reserve(listed);
  for (std::size_t i = 0; i < listed; ++i) {
    const store::StoredJob* record = find_job(load.snapshot, entries[i].job_id.value());
    IpcQueueEntry entry;
    entry.job_id = entries[i].job_id.value();
    entry.owner_uid = record != nullptr ? record->spec.owner_uid : 0;
    entry.submit_time_unix_ns = unix_ns(entries[i].submit_time);
    entry.state = job_state_wire(record != nullptr ? record->state : job::JobState::kQueued);
    response.queue.push_back(entry);
  }
  return response;
}

IpcResponse IpcService::handle_gpu() {
  gpu::GpuObservationSnapshot snapshot;
  if (!gpu_source_.try_get_snapshot(snapshot)) {
    return error_response(IpcRequestKind::kGpu, IpcError::kNotAvailable,
                          "no gpu observation available");
  }

  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kGpu, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }

  IpcResponse response;
  response.kind = IpcRequestKind::kGpu;
  response.error = IpcError::kNone;
  response.gpu_revision = snapshot.revision;
  response.devices.reserve(snapshot.devices.size());
  for (const gpu::GpuObservation& observation : snapshot.devices) {
    // lease 是调度事实，优先于观测（RULE-05）。
    std::optional<std::uint64_t> leased_by;
    for (const gpu::GpuLease& lease : load.snapshot.leases) {
      if (lease.gpu_uuid == observation.uuid) {
        leased_by = lease.job_id.value();
        break;
      }
    }
    IpcGpuDevice device;
    device.uuid = observation.uuid.value();
    device.index = observation.index;
    device.observed_state = gpu_observed_wire(observation.state);
    device.logical_state = gpu_logical_wire(
        gpu::derive_logical_state(observation.state, leased_by.has_value()));
    device.utilization_percent = observation.telemetry.utilization_percent;
    device.memory_used_bytes = observation.telemetry.memory_used_bytes;
    device.memory_total_bytes = observation.telemetry.memory_total_bytes;
    device.leased_by_job = leased_by;
    response.devices.push_back(std::move(device));
  }
  return response;
}

IpcResponse IpcService::handle_cancel(const PeerCredentials& peer, std::uint64_t job_id) {
  const auto context_state = [](IpcResponse& response, job::JobState state) {
    response.state = job_state_wire(state);
  };

  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kCancel, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }
  const store::StoredJob* record = find_job(load.snapshot, job_id);
  if (record == nullptr) {
    return error_response(IpcRequestKind::kCancel, IpcError::kNotFound, "job not found");
  }
  const job::JobState current = record->state;

  // owner/admin 授权（RULE-09/设计 11.5），daemon 侧判定。
  if (record->spec.owner_uid != peer.uid && !is_admin(peer)) {
    IpcResponse response =
        error_response(IpcRequestKind::kCancel, IpcError::kDenied, "not job owner or admin");
    context_state(response, current);
    return response;
  }

  // 终态幂等（RULE-04）：已 CANCELLED 重复取消幂等成功；其余终态显式拒绝。
  if (current == job::JobState::kCancelled) {
    IpcResponse response;
    response.kind = IpcRequestKind::kCancel;
    response.state = job_state_wire(job::JobState::kCancelled);
    return response;
  }
  if (job::is_terminal(current)) {
    IpcResponse response = error_response(IpcRequestKind::kCancel, IpcError::kInvalidState,
                                          job::to_string(current));
    context_state(response, current);
    return response;
  }
  if (current != job::JobState::kQueued) {
    // STARTING/RUNNING/STOPPING 的取消需要进程组信号路径（SIGTERM -> grace ->
    // SIGKILL），随守护总装收口接入（M7 前）；当前显式拒绝而非假装支持。
    IpcResponse response = error_response(
        IpcRequestKind::kCancel, IpcError::kUnsupported,
        "cancel of running jobs requires process supervision (not in this build)");
    context_state(response, current);
    return response;
  }

  // 先持久化 CANCELLED（revision + 1），再移除队列条目；队列已无该条目视为
  // 幂等成功。
  store::StoredJob cancelled = *record;
  cancelled.state = job::JobState::kCancelled;
  cancelled.revision = record->revision + 1;

  store::StateMutation mutation;
  mutation.expected_revision = load.snapshot.revision;
  mutation.update_jobs.push_back(std::move(cancelled));

  const store::StateStoreWriteResult write = store_.apply(mutation);
  if (!write.ok()) {
    IpcResponse response = error_response(IpcRequestKind::kCancel, IpcError::kStoreFailed,
                                          store::to_string(write.code));
    context_state(response, current);
    return response;
  }

  const queue::QueueOperationResult removal = queue_.remove(job::JobId{job_id});
  if (!removal.ok() && removal.code != queue::QueueErrorCode::kJobNotFound) {
    // 状态已终态化，队列移除异常仅并入 detail（持久化事实优先）。
    IpcResponse response =
        error_response(IpcRequestKind::kCancel, IpcError::kInternal,
                       std::string("cancelled but queue removal failed: ") +
                           queue::to_string(removal.code));
    context_state(response, job::JobState::kCancelled);
    return response;
  }

  IpcResponse response;
  response.kind = IpcRequestKind::kCancel;
  response.state = job_state_wire(job::JobState::kCancelled);
  return response;
}

IpcResponse IpcService::handle_logs(const PeerCredentials& peer, const IpcLogsRequest& request) {
  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kLogs, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }
  const store::StoredJob* record = find_job(load.snapshot, request.job_id);
  if (record == nullptr) {
    return error_response(IpcRequestKind::kLogs, IpcError::kNotFound, "job not found");
  }
  if (record->spec.owner_uid != peer.uid && !is_admin(peer)) {
    return error_response(IpcRequestKind::kLogs, IpcError::kDenied, "not job owner or admin");
  }
  // log_path 是该 Job 的日志目录（M2 LogSink 约定），仅在调度建立后存在。
  if (!record->execution.log_path) {
    return error_response(IpcRequestKind::kLogs, IpcError::kInvalidState,
                          "job has not started; no logs yet");
  }

  const std::uint32_t max_bytes = std::min(request.max_bytes, config_.max_log_tail_bytes);
  const std::string& directory = *record->execution.log_path;
  const LogTailResult stdout_tail =
      log_reader_.read_tail(directory + "/" + observe::log_file_name(observe::LogStreamKind::kStdout),
                            max_bytes);
  const LogTailResult stderr_tail =
      log_reader_.read_tail(directory + "/" + observe::log_file_name(observe::LogStreamKind::kStderr),
                            max_bytes);
  if (!stdout_tail.ok || !stderr_tail.ok) {
    return error_response(IpcRequestKind::kLogs, IpcError::kNotAvailable,
                          "log read failed: " + directory);
  }

  IpcResponse response;
  response.kind = IpcRequestKind::kLogs;
  response.error = IpcError::kNone;
  response.logs.stdout_truncated = stdout_tail.truncated;
  response.logs.stdout_tail = std::move(stdout_tail.tail);
  response.logs.stderr_truncated = stderr_tail.truncated;
  response.logs.stderr_tail = std::move(stderr_tail.tail);
  return response;
}

}  // namespace yori::ipc
