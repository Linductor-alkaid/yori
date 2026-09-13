#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <yori/ipc/ipc_service.hpp>
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

std::string to_lower_ascii(const std::string& text) {
  std::string lowered = text;
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lowered;
}

// scheduler::WaitReason -> IpcWaitReason 的 wire 映射（DEC-012：数值对齐）。
static_assert(static_cast<int>(scheduler::WaitReason::kNone) ==
                  static_cast<int>(IpcWaitReason::kNone),
              "wait reason wire values must stay aligned");
static_assert(static_cast<int>(scheduler::WaitReason::kNoFreeGpu) ==
                  static_cast<int>(IpcWaitReason::kNoFreeGpu),
              "wait reason wire values must stay aligned");
static_assert(static_cast<int>(scheduler::WaitReason::kAffinityGpuAllocated) ==
                  static_cast<int>(IpcWaitReason::kAffinityGpuAllocated),
              "wait reason wire values must stay aligned");
static_assert(static_cast<int>(scheduler::WaitReason::kAffinityGpuExternal) ==
                  static_cast<int>(IpcWaitReason::kAffinityGpuExternal),
              "wait reason wire values must stay aligned");
static_assert(static_cast<int>(scheduler::WaitReason::kAffinityGpuState) ==
                  static_cast<int>(IpcWaitReason::kAffinityGpuState),
              "wait reason wire values must stay aligned");

// placement 输入解析（DEC-012 决策 2）：全数字串按 NVML index、其余按 UUID
// 在当前观测快照中解析。索引只是易用输入，解析结果写入 JobSpec 的稳定
// GpuUuid；失败给出稳定原因（拒绝提交，不猜测）。
std::optional<std::string> resolve_gpu_spec(const std::string& gpu_spec,
                                            const gpu::GpuObservationSnapshot& snapshot,
                                            gpu::GpuUuid& out) {
  if (gpu_spec.empty()) {
    return std::string{"GPU_SPEC_EMPTY"};
  }
  bool numeric = true;
  for (const char c : gpu_spec) {
    if (c < '0' || c > '9') {
      numeric = false;
      break;
    }
  }
  if (numeric) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long index = std::strtoull(gpu_spec.c_str(), &end, 10);
    if (errno != 0 || end == gpu_spec.c_str() || *end != '\0' || index > 0xFFFFFFFFULL) {
      return std::string{"GPU_INDEX_NOT_FOUND: "} + gpu_spec;
    }
    for (const gpu::GpuObservation& device : snapshot.devices) {
      if (device.index == static_cast<std::uint32_t>(index)) {
        out = device.uuid;
        return std::nullopt;
      }
    }
    return std::string{"GPU_INDEX_NOT_FOUND: "} + gpu_spec;
  }
  const gpu::GpuUuid uuid{gpu_spec};
  if (!uuid.valid()) {
    return std::string{"GPU_UUID_INVALID"};
  }
  for (const gpu::GpuObservation& device : snapshot.devices) {
    if (device.uuid == uuid) {
      out = uuid;
      return std::nullopt;
    }
  }
  return std::string{"GPU_UUID_NOT_FOUND: "} + gpu_spec;
}

}  // namespace

bool IpcServiceConfig::valid() const noexcept {
  if (max_listed_jobs == 0 || max_listed_jobs > IpcProtocolLimits::kMaxListItems ||
      max_log_tail_bytes == 0 || max_log_tail_bytes > IpcProtocolLimits::kMaxLogTailBytes) {
    return false;
  }
  if (sensitive_env_patterns.size() > 64) {
    return false;
  }
  for (const std::string& pattern : sensitive_env_patterns) {
    if (pattern.empty() || pattern.size() > 64 || pattern.find('\0') != std::string::npos) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<LogSnapshotReader> file_log_snapshot_reader() {
  return std::make_unique<FileLogSnapshotReader>();
}

IpcService::IpcService(IpcServiceConfig config, store::StateStore& store,
                       GpuStatusSource& gpu_source, LogSnapshotReader& log_reader,
                       JobControl& job_control, ScheduleStatusSource& schedule_source)
    : config_(std::move(config)),
      store_(store),
      gpu_source_(gpu_source),
      log_reader_(log_reader),
      job_control_(job_control),
      schedule_source_(schedule_source) {}

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
    case IpcRequestKind::kLogsFollow:
      // 流式请求必须经传输层的流式委派接管（fd 移交）；走到普通 handler
      // 说明该构建/装配未启用流式会话，显式拒绝而非假装支持。
      return error_response(IpcRequestKind::kLogsFollow, IpcError::kUnsupported,
                            "streaming follow is not enabled on this endpoint");
    case IpcRequestKind::kTensorboard:
      return handle_tensorboard(peer, request.tensorboard.job_id);
    case IpcRequestKind::kInspect:
      return handle_inspect(peer, request.inspect.job_id);
  }
  return error_response(request.kind, IpcError::kInternal, "unhandled request kind");
}

IpcResponse IpcService::handle_submit(const PeerCredentials& peer,
                                      const IpcSubmitRequest& request) {
  // 身份只来自 SO_PEERCRED：请求结构不存在可消费的客户端身份字段
  // （DEC-010、威胁模型基线 2）。
  job::JobSpec spec;
  spec.owner_uid = peer.uid;
  spec.owner_gid = peer.gid;
  spec.argv = request.argv;
  spec.cwd = request.cwd;
  spec.env = request.env;
  spec.gpu_request = request.gpu_request;
  spec.launch_profile = request.launch_profile;
  spec.tensorboard_logdir = request.tensorboard_logdir;
  // v2 扩展（DEC-011）：提交时捕获的 executable 与环境元数据；v1 提交
  // 缺省（无捕获语义，daemon 启动时沿用 PATH 搜索）。
  spec.executable = request.executable;
  if (request.env_metadata) {
    job::EnvMetadata metadata;
    metadata.source = static_cast<job::EnvSource>(request.env_metadata->source);
    metadata.python_version = request.env_metadata->python_version;
    spec.env_metadata = std::move(metadata);
  }
  // v3 扩展（DEC-012）：placement 输入由 daemon 以当前观测解析为稳定 UUID
  // 并写入 JobSpec；与 gpu_request 计数语义互斥。解析失败显式拒绝提交。
  if (request.gpu_spec) {
    if (request.gpu_request != 1) {
      return error_response(IpcRequestKind::kSubmit, IpcError::kInvalidSpec,
                            job::to_string(job::JobSpecErrorCode::kPlacementGpuRequestConflict));
    }
    gpu::GpuObservationSnapshot snapshot;
    if (!gpu_source_.try_get_snapshot(snapshot)) {
      return error_response(IpcRequestKind::kSubmit, IpcError::kNotAvailable,
                            "no gpu observation available for placement resolution");
    }
    gpu::GpuUuid target;
    if (const auto failure = resolve_gpu_spec(*request.gpu_spec, snapshot, target)) {
      return error_response(IpcRequestKind::kSubmit, IpcError::kInvalidSpec, *failure);
    }
    spec.gpu_placement.mode = job::GpuPlacementMode::kRequired;
    spec.gpu_placement.devices.push_back(std::move(target));
  }
  spec.submit_time = std::chrono::system_clock::now();

  const job::JobSpecValidationResult validation = job::validate(spec);
  if (!validation.ok()) {
    if (validation.code == job::JobSpecErrorCode::kRootOwnerNotAllowed) {
      return error_response(IpcRequestKind::kSubmit, IpcError::kDenied, "root cannot submit jobs");
    }
    return error_response(IpcRequestKind::kSubmit, IpcError::kInvalidSpec,
                          job::to_string(validation.code));
  }

  // 状态变更（JobId 分配、store 创建、队列准入）委派给唯一写者（M7 守护
  // 总装：JobManager worker 经 JobControl）。
  const JobSubmitOutcome outcome = job_control_.submit_job(spec);
  switch (outcome.code) {
    case JobSubmitOutcome::Code::kSubmitted:
      break;
    case JobSubmitOutcome::Code::kStoreFailed:
      return error_response(IpcRequestKind::kSubmit, IpcError::kStoreFailed, outcome.detail);
    case JobSubmitOutcome::Code::kQueueRejected:
      return error_response(IpcRequestKind::kSubmit, IpcError::kQueueRejected, outcome.detail);
    case JobSubmitOutcome::Code::kUnavailable:
      return error_response(IpcRequestKind::kSubmit, IpcError::kNotAvailable, outcome.detail);
  }

  IpcResponse response;
  response.kind = IpcRequestKind::kSubmit;
  response.error = IpcError::kNone;
  response.job_id = outcome.job_id;
  return response;
}

IpcResponse IpcService::handle_ps(const PeerCredentials& peer) {
  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kPs, IpcError::kStoreFailed, store::to_string(load.code));
  }

  const bool admin = is_admin(peer);
  const bool truncate = load.snapshot.jobs.size() > config_.max_listed_jobs;

  // 最近一次调度评估（DEC-012）：QUEUED Job 的 wait_reason 派生视图。
  scheduler::ScheduleEvaluation evaluation;
  const bool have_evaluation = schedule_source_.try_get_schedule_evaluation(evaluation);

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
      summary.exit =
          IpcExitStatus{record.execution.exit->exited(),
                        record.execution.exit->exited() ? record.execution.exit->exit_code
                                                        : record.execution.exit->signal_number};
    }
    if (record.state == job::JobState::kQueued && have_evaluation) {
      for (const scheduler::ScheduleSkip& skip : evaluation.skipped) {
        if (skip.job == record.id) {
          summary.wait_reason = static_cast<std::uint8_t>(skip.reason);
          // placement 明细（目标 UUID）仅 owner/admin；脱敏视图只有原因本身。
          if (!masked && skip.target) {
            summary.wait_detail = skip.target->value();
          }
          break;
        }
      }
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

  // 队列视图由 store 快照派生（M7）：QUEUED Job 按 (submit_time, JobId) 升序，
  // 与 GlobalJobQueue 的派生索引一致；索引本身是 JobManager 私有。
  std::vector<const store::StoredJob*> queued;
  queued.reserve(load.snapshot.jobs.size());
  for (const auto& record : load.snapshot.jobs) {
    if (record.state == job::JobState::kQueued) {
      queued.push_back(&record);
    }
  }
  std::sort(queued.begin(), queued.end(),
            [](const store::StoredJob* lhs, const store::StoredJob* rhs) {
              if (lhs->spec.submit_time != rhs->spec.submit_time) {
                return lhs->spec.submit_time < rhs->spec.submit_time;
              }
              return lhs->id < rhs->id;
            });

  const bool admin = is_admin(peer);
  const bool truncate = queued.size() > config_.max_listed_jobs;
  scheduler::ScheduleEvaluation evaluation;
  const bool have_evaluation = schedule_source_.try_get_schedule_evaluation(evaluation);

  IpcResponse response;
  response.kind = IpcRequestKind::kQueue;
  response.error = truncate ? IpcError::kLimit : IpcError::kNone;
  if (truncate) {
    response.detail = "listing truncated to " + std::to_string(config_.max_listed_jobs) + " jobs";
  }

  const std::size_t listed =
      std::min(queued.size(), static_cast<std::size_t>(config_.max_listed_jobs));
  response.queue.reserve(listed);
  for (std::size_t i = 0; i < listed; ++i) {
    const store::StoredJob* record = queued[i];
    IpcQueueEntry entry;
    entry.job_id = record->id.value();
    entry.owner_uid = record->spec.owner_uid;
    entry.submit_time_unix_ns = unix_ns(record->spec.submit_time);
    entry.state = job_state_wire(record->state);
    if (have_evaluation) {
      for (const scheduler::ScheduleSkip& skip : evaluation.skipped) {
        if (skip.job == record->id) {
          entry.wait_reason = static_cast<std::uint8_t>(skip.reason);
          if ((admin || record->spec.owner_uid == peer.uid) && skip.target) {
            entry.wait_detail = skip.target->value();
          }
          break;
        }
      }
    }
    response.queue.push_back(std::move(entry));
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
    device.logical_state =
        gpu_logical_wire(gpu::derive_logical_state(observation.state, leased_by.has_value()));
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
    IpcResponse response =
        error_response(IpcRequestKind::kCancel, IpcError::kInvalidState, job::to_string(current));
    context_state(response, current);
    return response;
  }

  // QUEUED 与活动态（STARTING/RUNNING/STOPPING）的取消都委派给唯一写者
  // （M7 守护总装）：QUEUED 终态化并移出队列；活动态进入 SIGTERM -> grace ->
  // SIGKILL 的取消升级路径（DEC-007），结果状态 STOPPING。
  const JobCancelOutcome outcome = job_control_.cancel_job(job_id);
  switch (outcome.code) {
    case JobCancelOutcome::Code::kCancelled:
    case JobCancelOutcome::Code::kStopping: {
      IpcResponse response;
      response.kind = IpcRequestKind::kCancel;
      response.state = outcome.state;
      return response;
    }
    case JobCancelOutcome::Code::kNotFound:
      return error_response(IpcRequestKind::kCancel, IpcError::kNotFound, "job not found");
    case JobCancelOutcome::Code::kInvalidState: {
      IpcResponse response =
          error_response(IpcRequestKind::kCancel, IpcError::kInvalidState, outcome.detail);
      response.state = outcome.state;
      return response;
    }
    case JobCancelOutcome::Code::kStoreFailed:
      return error_response(IpcRequestKind::kCancel, IpcError::kStoreFailed, outcome.detail);
    case JobCancelOutcome::Code::kUnavailable:
      return error_response(IpcRequestKind::kCancel, IpcError::kNotAvailable, outcome.detail);
  }
  return error_response(IpcRequestKind::kCancel, IpcError::kInternal, "unhandled cancel outcome");
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
  LogTailResult stdout_tail = log_reader_.read_tail(
      directory + "/" + observe::log_file_name(observe::LogStreamKind::kStdout), max_bytes);
  LogTailResult stderr_tail = log_reader_.read_tail(
      directory + "/" + observe::log_file_name(observe::LogStreamKind::kStderr), max_bytes);
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

IpcResponse IpcService::validate_logs_follow(const PeerCredentials& peer,
                                             const IpcLogsFollowRequest& request) {
  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kLogsFollow, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }
  const store::StoredJob* record = find_job(load.snapshot, request.job_id);
  if (record == nullptr) {
    return error_response(IpcRequestKind::kLogsFollow, IpcError::kNotFound, "job not found");
  }
  if (record->spec.owner_uid != peer.uid && !is_admin(peer)) {
    return error_response(IpcRequestKind::kLogsFollow, IpcError::kDenied, "not job owner or admin");
  }
  if (!record->execution.log_path) {
    return error_response(IpcRequestKind::kLogsFollow, IpcError::kInvalidState,
                          "job has not started; no logs yet");
  }
  IpcResponse response;
  response.kind = IpcRequestKind::kLogsFollow;
  response.error = IpcError::kNone;
  response.logs_follow.job_state = job_state_wire(record->state);
  return response;
}

IpcResponse IpcService::handle_tensorboard(const PeerCredentials& peer, std::uint64_t job_id) {
  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kTensorboard, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }
  const store::StoredJob* record = find_job(load.snapshot, job_id);
  if (record == nullptr) {
    return error_response(IpcRequestKind::kTensorboard, IpcError::kNotFound, "job not found");
  }
  // cwd/tensorboard_logdir 属敏感字段（设计 11.5）：非 owner 非 admin 直接
  // 拒绝，不提供脱敏视图。
  if (record->spec.owner_uid != peer.uid && !is_admin(peer)) {
    return error_response(IpcRequestKind::kTensorboard, IpcError::kDenied,
                          "not job owner or admin");
  }
  IpcResponse response;
  response.kind = IpcRequestKind::kTensorboard;
  response.error = IpcError::kNone;
  response.tensorboard.logdir = record->spec.tensorboard_logdir;
  response.tensorboard.cwd = record->spec.cwd;
  return response;
}

bool IpcService::is_sensitive_env_name(const std::string& name) const {
  const std::string lowered = to_lower_ascii(name);
  for (const std::string& pattern : config_.sensitive_env_patterns) {
    if (lowered.find(to_lower_ascii(pattern)) != std::string::npos) {
      return true;
    }
  }
  return false;
}

IpcResponse IpcService::handle_inspect(const PeerCredentials& peer, std::uint64_t job_id) {
  const store::StateStoreLoadResult load = store_.load();
  if (!load.ok()) {
    return error_response(IpcRequestKind::kInspect, IpcError::kStoreFailed,
                          store::to_string(load.code));
  }
  const store::StoredJob* record = find_job(load.snapshot, job_id);
  if (record == nullptr) {
    return error_response(IpcRequestKind::kInspect, IpcError::kNotFound, "job not found");
  }
  // 执行上下文属敏感面（DEC-011 决策 7）：非 owner 非 admin 直接拒绝，不提供
  // 脱敏视图（同 TENSORBOARD 模式）。
  if (record->spec.owner_uid != peer.uid && !is_admin(peer)) {
    return error_response(IpcRequestKind::kInspect, IpcError::kDenied, "not job owner or admin");
  }

  IpcResponse response;
  response.kind = IpcRequestKind::kInspect;
  response.error = IpcError::kNone;
  IpcInspectPayload& inspect = response.inspect;
  inspect.job_id = record->id.value();
  inspect.state = job_state_wire(record->state);
  inspect.owner_uid = record->spec.owner_uid;
  inspect.revision = record->revision;
  inspect.cwd = record->spec.cwd;
  inspect.executable = record->spec.executable;
  inspect.argv = record->spec.argv;
  if (record->spec.env_metadata) {
    inspect.env_metadata =
        IpcEnvMetadata{static_cast<std::uint8_t>(record->spec.env_metadata->source),
                       record->spec.env_metadata->python_version};
  }

  // env：变量名全部可见；命中敏感名模式的值在 daemon 侧替换为固定掩码
  // （协议不传输原值；daemon 日志同样不打印 env 值）。
  inspect.env.reserve(record->spec.env.size());
  for (const auto& [name, value] : record->spec.env) {
    const bool masked = is_sensitive_env_name(name);
    inspect.env.push_back(IpcEnvEntry{name, masked, masked ? std::string{"***"} : value});
  }

  // 分配结果：lease 是调度事实（RULE-05）；物理索引来自当前 GPU 观测。
  for (const gpu::GpuLease& lease : load.snapshot.leases) {
    if (lease.job_id == record->id) {
      inspect.gpu_uuid = lease.gpu_uuid.value();
      gpu::GpuObservationSnapshot gpu_snapshot;
      if (gpu_source_.try_get_snapshot(gpu_snapshot)) {
        for (const gpu::GpuObservation& device : gpu_snapshot.devices) {
          if (device.uuid == lease.gpu_uuid) {
            inspect.gpu_index = device.index;
            break;
          }
        }
      }
      break;
    }
  }

  // placement 输入（DEC-012）：INSPECT 仅 owner/admin 可达，无脱敏分支。
  inspect.gpu_placement_mode = static_cast<std::uint8_t>(record->spec.gpu_placement.mode);
  if (record->spec.gpu_placement.mode == job::GpuPlacementMode::kRequired) {
    inspect.gpu_placement_device = record->spec.gpu_placement.devices.front().value();
  }

  inspect.submit_time_unix_ns = unix_ns(record->spec.submit_time);
  if (record->execution.exit) {
    inspect.exit =
        IpcExitStatus{record->execution.exit->exited(),
                      record->execution.exit->exited() ? record->execution.exit->exit_code
                                                       : record->execution.exit->signal_number};
  }
  inspect.log_path = record->execution.log_path;
  return response;
}

}  // namespace yori::ipc
