#include "runtime/job_manager.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <executor/blocking_io.hpp>
#include <executor/executor.hpp>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yori::runtime {
namespace {

// ---------------------------------------------------------------------------
// 辅助：目录链校验与创建（威胁模型基线 8 的 M7 项）。
// ---------------------------------------------------------------------------

// 父目录链属主校验：从根到 path 的每一级都必须是真实目录（拒绝符号链接）、
// 属主为 root 或 daemon euid、且无组/其他写位（带 sticky 位的世界可写目录
// 如 /tmp 例外）。任何一级失败即拒绝。
bool validate_directory_chain(const std::string& path, std::string& error) {
  if (path.empty() || path.front() != '/') {
    error = "log root must be an absolute path: " + path;
    return false;
  }
  std::string prefix;
  prefix.reserve(path.size());
  const auto check_prefix = [&](const std::string& current) {
    struct stat status {};
    if (::lstat(current.c_str(), &status) != 0) {
      error = "cannot stat " + current + ": " + std::strerror(errno);
      return false;
    }
    if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode)) {
      error = current + " is not a real directory";
      return false;
    }
    const uid_t self_uid = ::geteuid();
    if (status.st_uid != 0 && status.st_uid != self_uid) {
      error = current + " is owned by uid " + std::to_string(status.st_uid) +
              " (expected root or daemon euid)";
      return false;
    }
    const bool unsafe_writable = (status.st_mode & 0022) != 0 && (status.st_mode & 01000) == 0;
    if (unsafe_writable) {
      error = current + " is group/world writable without the sticky bit";
      return false;
    }
    return true;
  };
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i] != '/') {
      continue;
    }
    if (i == 0) {
      if (!check_prefix("/")) {
        return false;
      }
      continue;
    }
    prefix.assign(path, 0, i);
    if (!prefix.empty() && !check_prefix(prefix)) {
      return false;
    }
  }
  if (path.back() != '/' && !check_prefix(path)) {
    return false;
  }
  return true;
}

// 日志根目录就绪（启动路径）：父链校验通过后，叶子目录不存在则以 0750 创建
// （属主 daemon euid，生产为 root），已存在则同等校验。叶子写位收敛由创建
// 时的 mode 保证；已存在但组/其他可写的目录拒绝（防共享可写位置投放符号
// 链接）。
bool prepare_log_root(const std::string& path, std::string& error) {
  if (path.empty() || path.front() != '/') {
    error = "log root must be an absolute path: " + path;
    return false;
  }
  const auto last_slash = path.find_last_of('/');
  const std::string parent = last_slash == 0 ? "/" : path.substr(0, last_slash);
  if (!validate_directory_chain(parent, error)) {
    return false;
  }
  if (::mkdir(path.c_str(), 0750) != 0 && errno != EEXIST) {
    error = "mkdir " + path + " failed: " + std::strerror(errno);
    return false;
  }
  std::string leaf_error;
  if (!validate_directory_chain(path, leaf_error)) {
    error = std::move(leaf_error);
    return false;
  }
  return true;
}

// 创建（或复用）Job 日志目录 <root>/<job-id>：mkdir 0750；已存在时必须是
// 真实目录且属主合规。返回完整路径；失败给出稳定原因。
bool ensure_job_directory(const std::string& root, job::JobId id, std::string& out,
                          std::string& error) {
  out = root + "/" + std::to_string(id.value());
  if (::mkdir(out.c_str(), 0750) != 0 && errno != EEXIST) {
    error = "mkdir " + out + " failed: " + std::strerror(errno);
    return false;
  }
  struct stat status {};
  if (::lstat(out.c_str(), &status) != 0) {
    error = "cannot stat " + out + ": " + std::strerror(errno);
    return false;
  }
  if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode)) {
    error = out + " is not a real directory";
    return false;
  }
  const uid_t self_uid = ::geteuid();
  if (status.st_uid != 0 && status.st_uid != self_uid) {
    error = out + " is owned by uid " + std::to_string(status.st_uid);
    return false;
  }
  return true;
}

// JobSpec.launch_profile -> LaunchProfile（MVP 约定，设计 8.1）：缺省/空 ->
// CUDA_VISIBLE_DEVICES 模式；非空值 -> physical_argument 模式的参数名。
launch::LaunchProfile profile_from_spec(const job::JobSpec& spec) {
  launch::LaunchProfile profile;
  if (spec.launch_profile && !spec.launch_profile->empty()) {
    profile.mode = launch::GpuMappingMode::kPhysicalArgument;
    profile.physical_argument = *spec.launch_profile;
  }
  return profile;
}

ipc::IpcExitStatus exit_wire(const process::ExitStatus& status) {
  ipc::IpcExitStatus wire;
  wire.exited_normally = status.exited();
  wire.code = status.exited() ? status.exit_code : status.signal_number;
  return wire;
}

std::uint8_t state_wire(job::JobState state) noexcept { return static_cast<std::uint8_t>(state); }

// LogPump -> LogStreamer 桥（EXEC-04 数据面）：落盘接受后发布，丢弃发标记。
// 与 M6 集成测试的桥同型；观察失败不影响落盘主路径（发布结果只计数）。
class StreamerBridge final : public LogChunkObserver {
 public:
  explicit StreamerBridge(LogStreamer& streamer) : streamer_(streamer) {}

  void on_chunk(const job::JobId& job, observe::LogStreamKind stream, std::string_view data,
                std::uint64_t begin_offset, std::uint64_t end_offset) override {
    static_cast<void>(streamer_.publish_chunk(job, stream, data, begin_offset, end_offset));
  }

  void on_drop(const job::JobId& job, observe::LogStreamKind stream, std::uint64_t offset,
               std::uint64_t dropped_bytes) override {
    static_cast<void>(streamer_.publish_drop_marker(job, stream, offset, dropped_bytes));
  }

 private:
  LogStreamer& streamer_;
};

// 跨线程统计（worker 更新，owner/测试读取）。
struct AtomicStats final {
  std::atomic<std::uint64_t> commands_processed{0};
  std::atomic<std::uint64_t> jobs_submitted{0};
  std::atomic<std::uint64_t> jobs_launched{0};
  std::atomic<std::uint64_t> launch_failures{0};
  std::atomic<std::uint64_t> jobs_finished{0};
  std::atomic<std::uint64_t> jobs_failed{0};
  std::atomic<std::uint64_t> jobs_cancelled{0};
  std::atomic<std::uint64_t> jobs_adopted{0};
  std::atomic<std::uint64_t> recancels_armed{0};
  std::atomic<std::uint64_t> scheduler_runs{0};
  std::atomic<std::uint64_t> scheduler_scheduled{0};
  std::atomic<std::uint64_t> scheduler_failed{0};
  std::atomic<std::uint64_t> store_write_failures{0};
  std::atomic<std::uint64_t> abandoned_at_stop{0};
  std::atomic<std::uint64_t> active_supervised{0};
};

// worker 私有的受守护 Job 状态。
struct SupervisedJob final {
  job::JobId id{};
  gpu::GpuUuid leased_gpu{};
  std::unique_ptr<process::ProcessSupervisor> supervisor;
  std::unique_ptr<GraceEscalation> grace;
  bool cancelling{false};
  bool adopted{false};
  // 终态已落盘、等待日志泵排空后发布 EOF（观察面数据完整性：EOF 不得先于
  // 管道尾部数据发布）。采纳 Job（无泵）在退出处理内直接发布。
  bool exited{false};
  bool pump_done{false};
  std::uint8_t terminal_state{0};
  std::optional<ipc::IpcExitStatus> terminal_exit{};
};

struct Command final {
  enum class Kind : std::uint8_t {
    kSubmit,
    kCancel,
    kWake,  // 自投递的调度触发（kJobSubmitted/kJobExited/kJobCancelled 后）
  } kind{Kind::kWake};
  scheduler::SchedulerTrigger trigger{scheduler::SchedulerTrigger::kJobSubmitted};
  job::JobSpec submit_spec{};
  std::promise<ipc::JobSubmitOutcome> submit_ack;
  std::uint64_t cancel_job_id{0};
  std::promise<ipc::JobCancelOutcome> cancel_ack;
};

const store::StoredJob* find_stored(const store::StateSnapshot& snapshot, std::uint64_t job_id) {
  for (const auto& record : snapshot.jobs) {
    if (record.id.value() == job_id) {
      return &record;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// 守护 worker：单线程串行处理全部命令与事件。
// ---------------------------------------------------------------------------
class ManagerWorker final : public executor::IBlockingIoWorker {
 public:
  ManagerWorker(int wake_read, executor::comm::MpscChannel<Command>& commands,
                executor::comm::PhaseGate& startup_gate, store::StateStore& store,
                queue::GlobalJobQueue& queue, GpuManager& gpu_manager, LogStreamer& log_streamer,
                launch::IdentityResolver& identity_resolver, launch::LaunchAdapter& launch_adapter,
                const JobManagerConfig& config, AtomicStats& stats, std::atomic<bool>& stopping)
      : wake_read_(wake_read),
        commands_(commands),
        startup_gate_(startup_gate),
        store_(store),
        queue_(queue),
        gpu_manager_(gpu_manager),
        log_streamer_(log_streamer),
        identity_resolver_(identity_resolver),
        launch_adapter_(launch_adapter),
        config_(config),
        stats_(stats),
        stopping_(stopping),
        scheduler_(queue, store),
        scheduler_runner_(nullptr),
        store_runner_(nullptr),
        exit_monitor_(nullptr),
        log_pump_(nullptr),
        executor_(nullptr) {}

  void bind(executor::Executor& executor, SchedulerTaskRunner& scheduler_runner,
            StoreTaskRunner& store_runner, ProcessExitMonitor& exit_monitor, LogPump& log_pump,
            std::shared_ptr<LogChunkObserver> bridge) {
    executor_ = &executor;
    scheduler_runner_ = &scheduler_runner;
    store_runner_ = &store_runner;
    exit_monitor_ = &exit_monitor;
    log_pump_ = &log_pump;
    bridge_ = std::move(bridge);
  }

  // 恢复采纳（worker 启动前，owner 线程调用）：kAdopted* Job 建 supervisor
  // adopt + 退出监视注册；kAdoptedStoppingNeedsRecancel 重发 SIGTERM 并重建
  // 宽限（设计 6.2 恢复决策表）。
  void arm_recovery(const std::optional<recovery::RecoveryResult>& recovery) {
    if (!recovery.has_value() || !recovery->ok()) {
      return;
    }
    for (const auto& outcome : recovery->outcomes) {
      if (outcome.decision != recovery::RecoveryDecisionCode::kAdoptedRunning &&
          outcome.decision != recovery::RecoveryDecisionCode::kAdoptedPromotedToRunning &&
          outcome.decision != recovery::RecoveryDecisionCode::kAdoptedStoppingNeedsRecancel) {
        continue;
      }
      const store::StateStoreLoadResult load = store_.load();
      if (!load.ok()) {
        continue;
      }
      const store::StoredJob* record = find_stored(load.snapshot, outcome.job_id.value());
      if (record == nullptr || !record->execution.has_identity()) {
        continue;
      }
      SupervisedJob job;
      job.id = outcome.job_id;
      job.adopted = true;
      for (const auto& lease : load.snapshot.leases) {
        if (lease.job_id == outcome.job_id) {
          job.leased_gpu = lease.gpu_uuid;
          break;
        }
      }
      job.supervisor =
          std::make_unique<process::ProcessSupervisor>(process::CancelPolicy{config_.cancel_grace});
      if (!job.supervisor->adopt(record->execution.identity).ok()) {
        continue;  // 进程在恢复后、采纳前消失：退出事件路径不可达，保持现状。
      }
      if (exit_monitor_->register_process(record->execution.identity).code !=
          ExitRegisterCode::kRegistered) {
        continue;  // 监视注册失败：不接管（进程继续运行，退出不可见直到重启）。
      }
      const std::int64_t pid = record->execution.identity.pid;
      const bool needs_recancel =
          outcome.decision == recovery::RecoveryDecisionCode::kAdoptedStoppingNeedsRecancel;
      if (needs_recancel) {
        job.cancelling = true;
        job.grace = std::make_unique<GraceEscalation>(*executor_, *job.supervisor);
      }
      stats_.jobs_adopted.fetch_add(1, std::memory_order_relaxed);
      stats_.active_supervised.fetch_add(1, std::memory_order_relaxed);
      supervised_[outcome.job_id.value()] = std::move(job);
      by_pid_[pid] = outcome.job_id.value();
      if (needs_recancel) {
        begin_cancellation(outcome.job_id.value(), true);
        stats_.recancels_armed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void run(executor::StopToken stop_token) override {
    while (!stop_token.stop_requested() && !stopping_.load(std::memory_order_relaxed)) {
      wait_for_wakeup();
      drain_wake_pipe();
      if (stopping_.load(std::memory_order_relaxed)) {
        break;
      }
      process_commands();
      if (stopping_.load(std::memory_order_relaxed)) {
        break;
      }
      drain_exit_events();
      drain_pump_done();
      if (drain_gpu_events()) {
        schedule_now(scheduler::SchedulerTrigger::kGpuStateChanged);
      }
    }
    // EXEC-10 ④（调度与写生产者停止）：SchedulerTaskRunner/StoreTaskRunner 是
    // 单 owner 契约（trigger/submit 与 stop_accepting 必须同线程），由 worker
    // 在退出路径停止生产（owner 线程只做唤醒与 join）。
    scheduler_runner_->stop_accepting();
    store_runner_->stop_accepting();
    reject_pending_commands();
  }

  void wakeup() noexcept override {
    const char byte = 1;
    ssize_t written = 0;
    do {
      written = ::write(wake_write_, &byte, 1);
    } while (written < 0 && errno == EINTR);
  }

  void set_wake_write(int fd) noexcept { wake_write_ = fd; }

  // EXEC-10 ④：运行中进程 abandon（RULE-10，不终止训练进程）。
  std::size_t abandon_supervised() {
    std::size_t abandoned = 0;
    for (auto& [id, job] : supervised_) {
      static_cast<void>(id);
      if (job.grace) {
        static_cast<void>(job.grace->disarm());
      }
      if (job.supervisor && job.supervisor->abandon() == process::AbandonCode::kAbandoned) {
        ++abandoned;
      }
      job.supervisor.reset();
      job.grace.reset();
    }
    if (abandoned > 0) {
      stats_.active_supervised.fetch_sub(abandoned, std::memory_order_relaxed);
    }
    return abandoned;
  }

  void shutdown_components() {
    // EXEC-10 ⑤：退出监视与日志泵回收（不向进程发信号；泵关闭读端后按
    // DEC-008 语义训练进程写管道得 EPIPE，输出丢失可接受）。
    if (exit_monitor_ != nullptr) {
      exit_monitor_->stop();
    }
    if (log_pump_ != nullptr) {
      log_pump_->stop();
    }
  }

 private:
  void wait_for_wakeup() noexcept {
    struct pollfd waiter {};
    waiter.fd = wake_read_;
    waiter.events = POLLIN;
    const int ready = ::poll(&waiter, 1, -1);
    if (ready < 0 && errno != EINTR) {
      struct pollfd backoff {};
      backoff.fd = -1;
      ::poll(&backoff, 1, 100);
    }
  }

  void drain_wake_pipe() noexcept {
    char buffer[64];
    while (::read(wake_read_, buffer, sizeof(buffer)) > 0) {
    }
  }

  void process_commands() {
    Command command;
    while (commands_.try_receive(command)) {
      stats_.commands_processed.fetch_add(1, std::memory_order_relaxed);
      switch (command.kind) {
        case Command::Kind::kSubmit:
          process_submit(std::move(command.submit_spec), std::move(command.submit_ack));
          post_wake(scheduler::SchedulerTrigger::kJobSubmitted);
          break;
        case Command::Kind::kCancel:
          process_cancel(command.cancel_job_id, std::move(command.cancel_ack));
          post_wake(scheduler::SchedulerTrigger::kJobCancelled);
          break;
        case Command::Kind::kWake:
          schedule_now(command.trigger);
          break;
      }
    }
  }

  // 停止路径：未处理命令显式 kUnavailable 应答，不静默丢弃。
  void reject_pending_commands() {
    Command command;
    while (commands_.try_receive(command)) {
      if (command.kind == Command::Kind::kSubmit) {
        command.submit_ack.set_value(ipc::JobSubmitOutcome{
            ipc::JobSubmitOutcome::Code::kUnavailable, 0, "job manager is stopping"});
      } else if (command.kind == Command::Kind::kCancel) {
        command.cancel_ack.set_value(ipc::JobCancelOutcome{
            ipc::JobCancelOutcome::Code::kUnavailable, 0, "job manager is stopping"});
      }
    }
  }

  bool post(Command&& command) {
    if (!commands_.try_send(std::move(command))) {
      return false;
    }
    return true;
  }

  void post_wake(scheduler::SchedulerTrigger trigger) {
    Command command;
    command.kind = Command::Kind::kWake;
    command.trigger = trigger;
    static_cast<void>(post(std::move(command)));
    // 自投递后立即唤醒（命令通道可能有空闲窗口）。
    wakeup_noop();
  }

  // 命令入队方的唤醒由 enqueue_command 完成；worker 内自投递同样需要唤醒
  // 下一轮 poll。
  void wakeup_noop() noexcept { wakeup(); }

  // ------------------------------------------------------------------
  // store 写路径（EXEC-08：StoreTaskRunner，单在飞，串行等待消费）。
  // ------------------------------------------------------------------

  store::StateStoreWriteResult run_write(const store::StateMutation& mutation) {
    const auto submitted = store_runner_->submit(mutation);
    if (!submitted.accepted()) {
      stats_.store_write_failures.fetch_add(1, std::memory_order_relaxed);
      return store::StateStoreWriteResult{store::StateStoreErrorCode::kBackendUnavailable, 0};
    }
    const auto completion = store_runner_->wait_and_consume();
    if (completion.code != StoreTaskCompletionCode::kCompleted || !completion.write_result) {
      stats_.store_write_failures.fetch_add(1, std::memory_order_relaxed);
      return store::StateStoreWriteResult{store::StateStoreErrorCode::kBackendUnavailable, 0};
    }
    if (!completion.write_result->ok()) {
      return *completion.write_result;
    }
    return *completion.write_result;
  }

  // 模式化重写：load -> 定位 Job -> compose -> 写入；revision 冲突有界重试
  // （单写者下不应发生，防御性保留）。
  template <typename Compose>
  store::StateStoreWriteResult rewrite_job(std::uint64_t job_id, Compose compose) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      const store::StateStoreLoadResult load = store_.load();
      if (!load.ok()) {
        return store::StateStoreWriteResult{load.code, 0};
      }
      const store::StoredJob* record = find_stored(load.snapshot, job_id);
      if (record == nullptr) {
        return store::StateStoreWriteResult{store::StateStoreErrorCode::kJobNotFound, 0};
      }
      store::StateMutation mutation;
      mutation.expected_revision = load.snapshot.revision;
      store::StoredJob updated = *record;
      compose(*record, updated, mutation);
      mutation.update_jobs.push_back(std::move(updated));
      const store::StateStoreWriteResult write = run_write(mutation);
      if (write.ok() || write.code != store::StateStoreErrorCode::kRevisionConflict) {
        return write;
      }
    }
    return store::StateStoreWriteResult{store::StateStoreErrorCode::kRevisionConflict, 0};
  }

  // ------------------------------------------------------------------
  // submit / cancel。
  // ------------------------------------------------------------------

  void process_submit(job::JobSpec spec, std::promise<ipc::JobSubmitOutcome> ack) {
    const auto fail = [&ack](store::StateStoreErrorCode code) {
      ack.set_value(ipc::JobSubmitOutcome{ipc::JobSubmitOutcome::Code::kStoreFailed, 0,
                                          store::to_string(code)});
    };
    const store::StateStoreLoadResult load = store_.load();
    if (!load.ok()) {
      fail(load.code);
      return;
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
    const store::StateStoreWriteResult write = run_write(mutation);
    if (!write.ok()) {
      fail(write.code);
      return;
    }

    const queue::QueueOperationResult admission = queue_.admit(record);
    if (!admission.ok()) {
      // 容量拒绝显式回滚：持久化 CANCELLED 保留审计事实（与 M5 语义一致）。
      store::StoredJob cancelled = record;
      cancelled.state = job::JobState::kCancelled;
      cancelled.revision = 1;
      store::StateMutation rollback;
      rollback.expected_revision = write.revision;
      rollback.update_jobs.push_back(std::move(cancelled));
      const store::StateStoreWriteResult rollback_write = run_write(rollback);

      std::string detail = queue::to_string(admission.code);
      if (!rollback_write.ok()) {
        detail += std::string("; rollback failed: ") + store::to_string(rollback_write.code);
      }
      ack.set_value(ipc::JobSubmitOutcome{ipc::JobSubmitOutcome::Code::kQueueRejected, 0, detail});
      return;
    }

    stats_.jobs_submitted.fetch_add(1, std::memory_order_relaxed);
    ack.set_value(
        ipc::JobSubmitOutcome{ipc::JobSubmitOutcome::Code::kSubmitted, record.id.value(), {}});
  }

  void process_cancel(std::uint64_t job_id, std::promise<ipc::JobCancelOutcome> ack) {
    const store::StateStoreLoadResult load = store_.load();
    if (!load.ok()) {
      ack.set_value(ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kStoreFailed, 0,
                                          store::to_string(load.code)});
      return;
    }
    const store::StoredJob* record = find_stored(load.snapshot, job_id);
    if (record == nullptr) {
      ack.set_value(
          ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kNotFound, 0, "job not found"});
      return;
    }

    if (record->state == job::JobState::kQueued) {
      const auto write = rewrite_job(job_id, [](const store::StoredJob& current,
                                                store::StoredJob& updated, store::StateMutation&) {
        updated.state = job::JobState::kCancelled;
        updated.revision = current.revision + 1;
      });
      if (!write.ok()) {
        ack.set_value(ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kStoreFailed, 0,
                                            store::to_string(write.code)});
        return;
      }
      const queue::QueueOperationResult removal = queue_.remove(job::JobId{job_id});
      std::string detail;
      if (!removal.ok() && removal.code != queue::QueueErrorCode::kJobNotFound) {
        detail =
            std::string("cancelled but queue removal failed: ") + queue::to_string(removal.code);
      }
      stats_.jobs_cancelled.fetch_add(1, std::memory_order_relaxed);
      ack.set_value(ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kCancelled,
                                          state_wire(job::JobState::kCancelled), detail});
      return;
    }

    if (record->state == job::JobState::kStarting || record->state == job::JobState::kRunning) {
      const auto write = rewrite_job(job_id, [](const store::StoredJob& current,
                                                store::StoredJob& updated, store::StateMutation&) {
        updated.state = job::JobState::kStopping;
        updated.revision = current.revision + 1;
      });
      if (!write.ok()) {
        ack.set_value(ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kStoreFailed, 0,
                                            store::to_string(write.code)});
        return;
      }
      begin_cancellation(job_id, false);
      ack.set_value(ipc::JobCancelOutcome{
          ipc::JobCancelOutcome::Code::kStopping, state_wire(job::JobState::kStopping), {}});
      return;
    }

    if (record->state == job::JobState::kStopping) {
      // 已在取消升级路径：重发 SIGTERM（幂等）并保持既有宽限。
      begin_cancellation(job_id, false);
      ack.set_value(ipc::JobCancelOutcome{
          ipc::JobCancelOutcome::Code::kStopping, state_wire(job::JobState::kStopping), {}});
      return;
    }

    if (record->state == job::JobState::kCancelled) {
      ack.set_value(ipc::JobCancelOutcome{
          ipc::JobCancelOutcome::Code::kCancelled, state_wire(job::JobState::kCancelled), {}});
      return;
    }

    ack.set_value(ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kInvalidState,
                                        state_wire(record->state), job::to_string(record->state)});
  }

  // 活动态取消的进程侧动作：SIGTERM（进程组）+ 宽限升级定时（DEC-007）。
  // from_recovery=true 时不写 store（恢复路径 STOPPING 已落盘）。
  void begin_cancellation(std::uint64_t job_id, bool from_recovery) {
    static_cast<void>(from_recovery);
    const auto iter = supervised_.find(job_id);
    if (iter == supervised_.end() || iter->second.supervisor == nullptr) {
      return;  // 进程不在守护之下（采纳失败/退出处理中）；终态由退出或恢复收敛。
    }
    SupervisedJob& job = iter->second;
    const auto cancel = job.supervisor->request_cancel();
    if (cancel.outcome == process::CancelOutcome::kNotRunning ||
        cancel.outcome == process::CancelOutcome::kAlreadyExited) {
      return;  // 进程已退出：退出事件路径负责终态。
    }
    if (!job.grace) {
      job.grace = std::make_unique<GraceEscalation>(*executor_, *job.supervisor);
    }
    if (!job.cancelling) {
      static_cast<void>(job.grace->arm());
      job.cancelling = true;
    } else if (from_recovery) {
      static_cast<void>(job.grace->arm());
    }
  }

  // ------------------------------------------------------------------
  // 调度（EXEC-06）与启动落地。
  // ------------------------------------------------------------------

  void schedule_now(scheduler::SchedulerTrigger trigger) {
    // EXEC-09：调度仅在启动 PhaseGate 开启后执行。
    if (!startup_gate_.has_reached(kPhaseSchedulingOpen)) {
      return;
    }
    gpu::GpuObservationSnapshot snapshot;
    if (!gpu_manager_.try_get_snapshot(snapshot)) {
      stats_.scheduler_failed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto submitted = scheduler_runner_->trigger(trigger, std::move(snapshot));
    if (!submitted.accepted()) {
      stats_.scheduler_failed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto completion = scheduler_runner_->wait_and_consume();
    stats_.scheduler_runs.fetch_add(1, std::memory_order_relaxed);
    if (completion.code != SchedulerTaskCompletionCode::kCompleted || !completion.schedule_result) {
      stats_.scheduler_failed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const scheduler::ScheduleResult& result = *completion.schedule_result;
    if (result.scheduled()) {
      stats_.scheduler_scheduled.fetch_add(1, std::memory_order_relaxed);
      launch_scheduled(result.event.job_id.value().value(), result.event.gpu_uuid.value());
    } else if (result.failed()) {
      stats_.scheduler_failed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void launch_scheduled(std::uint64_t job_id, const gpu::GpuUuid& uuid) {
    const store::StateStoreLoadResult load = store_.load();
    if (!load.ok()) {
      stats_.launch_failures.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const store::StoredJob* record = find_stored(load.snapshot, job_id);
    if (record == nullptr || record->state != job::JobState::kStarting) {
      // 调度结果与 store 分歧（不应发生）：显式计数，不推进。
      stats_.launch_failures.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const job::JobSpec& spec = record->spec;

    // 物理索引来自当前 GPU 观测（lease 保存稳定 UUID）。
    gpu::GpuObservationSnapshot gpu_snapshot;
    std::uint32_t physical_index = 0;
    if (gpu_manager_.try_get_snapshot(gpu_snapshot)) {
      for (const auto& device : gpu_snapshot.devices) {
        if (device.uuid == uuid) {
          physical_index = device.index;
          break;
        }
      }
    }

    const launch::IdentityResolveResult identity = identity_resolver_.resolve(spec.owner_uid);
    if (!identity.ok()) {
      fail_launch(job_id, uuid, "identity resolve failed: " + identity.message);
      return;
    }

    const launch::LaunchProfile profile = profile_from_spec(spec);
    if (const launch::LaunchProfileValidationResult profile_check = launch::validate(profile);
        !profile_check.ok()) {
      fail_launch(job_id, uuid,
                  std::string("invalid launch profile: ") + launch::to_string(profile_check.code));
      return;
    }

    const launch::LaunchPlanResult plan = launch_adapter_.prepare(
        spec, launch::GpuAssignment{uuid, physical_index, 0}, profile, identity.identity);
    if (!plan.ok()) {
      fail_launch(job_id, uuid, "launch plan rejected: " + plan.message);
      return;
    }

    std::string directory;
    std::string directory_error;
    if (!ensure_job_directory(config_.log_root, job::JobId{job_id}, directory, directory_error)) {
      fail_launch(job_id, uuid, directory_error);
      return;
    }

    observe::LogSink sink(config_.log_io ? config_.log_io : observe::default_log_io());
    observe::LogSinkConfig sink_config;
    sink_config.directory = directory;
    observe::LogSinkOpenOptions open_options;
    open_options.owner_uid = identity.identity.uid;
    open_options.owner_gid = identity.identity.gid;
    std::string open_error;
    if (sink.open(sink_config, open_options, open_error) != observe::LogSinkErrorCode::kNone) {
      fail_launch(job_id, uuid, "log sink open failed: " + open_error);
      return;
    }

    std::string register_error;
    if (log_streamer_.register_job(job::JobId{job_id}, register_error) !=
        LogRegisterCode::kRegistered) {
      fail_launch(job_id, uuid, "log streamer register failed: " + register_error);
      return;
    }

    auto supervisor =
        std::make_unique<process::ProcessSupervisor>(process::CancelPolicy{config_.cancel_grace});
    process::SpawnResult spawned = supervisor->spawn(plan.plan);
    if (!spawned.ok()) {
      static_cast<void>(log_streamer_.unregister_job(job::JobId{job_id}));
      fail_launch(job_id, uuid,
                  std::string("spawn failed: ") + process::to_string(spawned.code) +
                      (spawned.message.empty() ? "" : ": " + spawned.message));
      return;
    }

    // STARTING -> RUNNING：身份、起始时间与日志路径在同一 mutation 落盘
    // （崩溃窗口语义见设计 6.2：STARTING 无身份 -> LOST；有身份 -> 提升）。
    const auto write = rewrite_job(
        job_id, [&spawned, &directory](const store::StoredJob& current, store::StoredJob& updated,
                                       store::StateMutation&) {
          updated.state = job::JobState::kRunning;
          updated.revision = current.revision + 1;
          updated.execution.identity = spawned.identity;
          updated.execution.start_time = std::chrono::system_clock::now();
          updated.execution.log_path = directory;
        });
    if (!write.ok()) {
      // 进程已启动但状态未持久化：终止进程（supervisor 析构 SIGKILL + 有界
      // 回收），尽力终态化，失败显式计数（恢复路径兜底 LOST + lease 释放）。
      static_cast<void>(log_streamer_.unregister_job(job::JobId{job_id}));
      supervisor.reset();
      fail_launch(job_id, uuid,
                  std::string("running state persist failed: ") + store::to_string(write.code));
      return;
    }

    if (exit_monitor_->register_process(spawned.identity).code != ExitRegisterCode::kRegistered) {
      const auto unregister = log_streamer_.unregister_job(job::JobId{job_id});
      static_cast<void>(unregister);
      supervisor.reset();
      fail_launch(job_id, uuid, "exit monitor registration failed");
      return;
    }

    LogPumpJobInput pump_input;
    pump_input.job = job::JobId{job_id};
    pump_input.stdout_read = std::move(spawned.stdout_read);
    pump_input.stderr_read = std::move(spawned.stderr_read);
    pump_input.sink = std::move(sink);
    pump_input.observer = bridge_;
    if (log_pump_->attach(std::move(pump_input)).code != LogPumpAttachCode::kAttached) {
      const auto unregister = log_streamer_.unregister_job(job::JobId{job_id});
      static_cast<void>(unregister);
      static_cast<void>(exit_monitor_->unregister_process(spawned.identity.pid));
      supervisor.reset();
      fail_launch(job_id, uuid, "log pump attach failed");
      return;
    }

    SupervisedJob supervised;
    supervised.id = job::JobId{job_id};
    supervised.leased_gpu = uuid;
    supervised.supervisor = std::move(supervisor);
    supervised_[job_id] = std::move(supervised);
    by_pid_[spawned.identity.pid] = job_id;
    stats_.jobs_launched.fetch_add(1, std::memory_order_relaxed);
    stats_.active_supervised.fetch_add(1, std::memory_order_relaxed);
  }

  // 启动失败收敛：FAILED + lease 释放同一 mutation（终态 + 资源回收原子），
  // 观察面以 EOF 收尾；失败计数不吞掉。
  void fail_launch(std::uint64_t job_id, const gpu::GpuUuid& uuid, const std::string& reason) {
    stats_.launch_failures.fetch_add(1, std::memory_order_relaxed);
    const auto write = rewrite_job(
        job_id, [&uuid, &reason](const store::StoredJob& current, store::StoredJob& updated,
                                 store::StateMutation& mutation) {
          if (job::is_terminal(current.state)) {
            updated = current;  // 已终态（迟到失败）：保持幂等，不复活。
            return;
          }
          updated.state = job::JobState::kFailed;
          updated.revision = current.revision + 1;
          updated.execution.failure_reason =
              reason.substr(0, store::JobExecutionLimits::kMaxFailureReasonBytes);
          updated.execution.end_time = std::chrono::system_clock::now();
          mutation.release_leases.push_back(uuid);
        });
    if (!write.ok()) {
      stats_.store_write_failures.fetch_add(1, std::memory_order_relaxed);
    }
    const auto finish = log_streamer_.finish_job(job::JobId{job_id},
                                                 state_wire(job::JobState::kFailed), std::nullopt);
    static_cast<void>(finish);
    stats_.jobs_failed.fetch_add(1, std::memory_order_relaxed);
  }

  // ------------------------------------------------------------------
  // 事件排空。
  // ------------------------------------------------------------------

  void drain_exit_events() {
    ExitEvent event;
    while (exit_monitor_ != nullptr && exit_monitor_->try_receive_exit(event)) {
      process_exit(event);
      if (stopping_.load(std::memory_order_relaxed)) {
        return;
      }
    }
  }

  void process_exit(const ExitEvent& event) {
    const auto pid_iter = by_pid_.find(event.pid);
    if (pid_iter == by_pid_.end()) {
      return;  // 未守护的 PID（不应发生）：丢弃，监视侧已注销。
    }
    const std::uint64_t job_id = pid_iter->second;
    const auto iter = supervised_.find(job_id);
    if (iter == supervised_.end()) {
      by_pid_.erase(pid_iter);
      return;
    }

    // owner 纪律：先解除宽限定时，再触碰 supervisor（GraceEscalation 契约）。
    if (iter->second.grace) {
      static_cast<void>(iter->second.grace->disarm());
    }
    const bool cancelling = iter->second.cancelling;
    const gpu::GpuUuid uuid = iter->second.leased_gpu;

    job::JobState terminal = job::JobState::kFailed;
    std::optional<std::string> reason;
    if (cancelling) {
      terminal = job::JobState::kCancelled;
    } else if (!event.identity_verified) {
      terminal = job::JobState::kFailed;
      reason = "exit status unavailable (identity verification failed or adopted process)";
    } else if (event.status.is_success()) {
      terminal = job::JobState::kFinished;
    } else {
      terminal = job::JobState::kFailed;
      reason = event.status.exited()
                   ? "exited with code " + std::to_string(event.status.exit_code)
                   : "terminated by signal " + std::to_string(event.status.signal_number);
    }

    const auto write =
        rewrite_job(job_id, [terminal, &reason, &event, &uuid](const store::StoredJob& current,
                                                               store::StoredJob& updated,
                                                               store::StateMutation& mutation) {
          if (job::is_terminal(current.state)) {
            updated = current;  // 终态幂等（RULE-04）：迟到退出不复活。
            return;
          }
          updated.state = terminal;
          updated.revision = current.revision + 1;
          if (event.identity_verified) {
            updated.execution.exit = event.status;
          }
          updated.execution.end_time = std::chrono::system_clock::now();
          updated.execution.failure_reason = reason;
          mutation.release_leases.push_back(uuid);
        });
    if (!write.ok()) {
      // 终态未落盘：显式计数；进程已回收，lease 由下次恢复收敛（LOST + 释放）。
      stats_.store_write_failures.fetch_add(1, std::memory_order_relaxed);
    } else {
      switch (terminal) {
        case job::JobState::kFinished:
          stats_.jobs_finished.fetch_add(1, std::memory_order_relaxed);
          break;
        case job::JobState::kCancelled:
          stats_.jobs_cancelled.fetch_add(1, std::memory_order_relaxed);
          break;
        default:
          stats_.jobs_failed.fetch_add(1, std::memory_order_relaxed);
          break;
      }
    }

    // EOF 发布（观察面）：等待日志泵排空尾部数据后发布；泵已完成的迟到
    // done 事件在 drain_pump_done 中补发。采纳 Job 无泵，直接发布。
    const std::optional<ipc::IpcExitStatus> exit_info =
        event.identity_verified ? std::optional{exit_wire(event.status)} : std::nullopt;
    if (iter->second.adopted || iter->second.pump_done) {
      const auto finish =
          log_streamer_.finish_job(job::JobId{job_id}, state_wire(terminal), exit_info);
      static_cast<void>(finish);
      supervised_.erase(iter);
      stats_.active_supervised.fetch_sub(1, std::memory_order_relaxed);
    } else {
      iter->second.exited = true;
      iter->second.terminal_state = state_wire(terminal);
      iter->second.terminal_exit = exit_info;
    }
    by_pid_.erase(pid_iter);
    // Job 退出触发下一轮调度（GPU 已释放）。
    post_wake(scheduler::SchedulerTrigger::kJobExited);
  }

  void drain_pump_done() {
    LogPumpDone done;
    while (log_pump_ != nullptr && log_pump_->try_receive_done(done)) {
      // 排空完成即尾部数据已全部发布：与退出事件任意先后到达，两者都到齐后
      // 发布 EOF（观察面不丢尾部块，也不悬挂等待）。
      const auto iter = supervised_.find(done.job.value());
      if (iter != supervised_.end()) {
        iter->second.pump_done = true;
        if (iter->second.exited) {
          const auto finish = log_streamer_.finish_job(done.job, iter->second.terminal_state,
                                                       iter->second.terminal_exit);
          static_cast<void>(finish);
          supervised_.erase(iter);
          stats_.active_supervised.fetch_sub(1, std::memory_order_relaxed);
        }
      }
    }
  }

  bool drain_gpu_events() {
    bool state_changed = false;
    GpuManagerEvent event;
    while (gpu_manager_.try_receive_event(event)) {
      if (event.kind == GpuManagerEventKind::kGpuStateChanged) {
        state_changed = true;
      }
    }
    return state_changed;
  }

  int wake_read_{-1};
  int wake_write_{-1};
  executor::comm::MpscChannel<Command>& commands_;
  executor::comm::PhaseGate& startup_gate_;
  store::StateStore& store_;
  queue::GlobalJobQueue& queue_;
  GpuManager& gpu_manager_;
  LogStreamer& log_streamer_;
  launch::IdentityResolver& identity_resolver_;
  launch::LaunchAdapter& launch_adapter_;
  JobManagerConfig config_;
  AtomicStats& stats_;
  std::atomic<bool>& stopping_;

  scheduler::FifoScheduler scheduler_;
  SchedulerTaskRunner* scheduler_runner_;
  StoreTaskRunner* store_runner_;
  ProcessExitMonitor* exit_monitor_;
  LogPump* log_pump_;
  executor::Executor* executor_;
  std::shared_ptr<LogChunkObserver> bridge_;

  std::unordered_map<std::uint64_t, SupervisedJob> supervised_;
  std::unordered_map<std::int64_t, std::uint64_t> by_pid_;
};

}  // namespace

bool JobManagerConfig::valid(std::string& error) const noexcept {
  if (log_root.empty() || log_root.front() != '/') {
    error = "log root must be an absolute path";
    return false;
  }
  if (log_root.size() > observe::LogSinkLimits::kMaxDirectoryBytes) {
    error = "log root too long";
    return false;
  }
  if (cancel_grace < process::CancelPolicyLimits::kMinGracePeriod ||
      cancel_grace > process::CancelPolicyLimits::kMaxGracePeriod) {
    error = "cancel grace out of range";
    return false;
  }
  if (command_capacity == 0) {
    error = "command capacity must be positive";
    return false;
  }
  if (ack_timeout <= std::chrono::milliseconds{0}) {
    error = "ack timeout must be positive";
    return false;
  }
  return true;
}

const char* to_string(JobManagerStartCode code) noexcept {
  switch (code) {
    case JobManagerStartCode::kStarted:
      return "started";
    case JobManagerStartCode::kAlreadyStarted:
      return "already started";
    case JobManagerStartCode::kInvalidConfig:
      return "invalid config";
    case JobManagerStartCode::kLogRootInvalid:
      return "log root rejected";
    case JobManagerStartCode::kWorkersRejected:
      return "executor rejected a worker";
  }
  return "unknown";
}

const char* to_string(JobManagerStopCode code) noexcept {
  switch (code) {
    case JobManagerStopCode::kStopped:
      return "stopped";
    case JobManagerStopCode::kNotRunning:
      return "not running";
    case JobManagerStopCode::kAlreadyStopped:
      return "already stopped";
  }
  return "unknown";
}

class JobManager::Impl final {
 public:
  Impl(executor::Executor& executor_ref, store::StateStore& store_ref,
       queue::GlobalJobQueue& queue_ref, GpuManager& gpu_manager_ref, LogStreamer& log_streamer_ref,
       launch::IdentityResolver& identity_resolver_ref, launch::LaunchAdapter& launch_adapter_ref,
       executor::comm::PhaseGate& startup_gate_ref, JobManagerConfig config_value)
      : config(std::move(config_value)),
        commands(executor::comm::ChannelOptions{config_value.command_capacity,
                                                executor::comm::DropPolicy::RejectNewest, true,
                                                "yori-job-manager-commands"}),
        executor(executor_ref),
        store(store_ref),
        queue(queue_ref),
        gpu_manager(gpu_manager_ref),
        log_streamer(log_streamer_ref),
        startup_gate(startup_gate_ref),
        scheduler(queue_ref, store_ref),
        scheduler_runner(executor_ref, scheduler),
        store_runner(executor_ref, store_ref),
        exit_monitor(executor_ref),
        log_pump(executor_ref),
        identity_resolver(identity_resolver_ref),
        launch_adapter(launch_adapter_ref) {}

  JobManagerConfig config;
  executor::comm::MpscChannel<Command> commands;
  executor::Executor& executor;
  store::StateStore& store;
  queue::GlobalJobQueue& queue;
  GpuManager& gpu_manager;
  LogStreamer& log_streamer;
  executor::comm::PhaseGate& startup_gate;
  scheduler::FifoScheduler scheduler;
  SchedulerTaskRunner scheduler_runner;
  StoreTaskRunner store_runner;
  ProcessExitMonitor exit_monitor;
  LogPump log_pump;
  launch::IdentityResolver& identity_resolver;
  launch::LaunchAdapter& launch_adapter;

  // 唤醒源（退出监视/GPU 事件监听回调，非阻塞）：停止后忽略，fd 关闭前先
  // 由 stop() 解除全部挂钩。
  void kick() noexcept {
    if (stopping.load(std::memory_order_relaxed) || wake_write < 0) {
      return;
    }
    const char byte = 1;
    ssize_t written = 0;
    do {
      written = ::write(wake_write, &byte, 1);
    } while (written < 0 && errno == EINTR);
  }

  std::shared_ptr<StreamerBridge> bridge;
  ManagerWorker* worker{nullptr};
  executor::WorkerHandle handle;
  AtomicStats stats;
  std::atomic<bool> stopping{false};
  int wake_read{-1};
  int wake_write{-1};
  bool started{false};
  bool stop_requested{false};
};

JobManager::JobManager(executor::Executor& executor, store::StateStore& store,
                       queue::GlobalJobQueue& queue, GpuManager& gpu_manager,
                       LogStreamer& log_streamer, launch::IdentityResolver& identity_resolver,
                       launch::LaunchAdapter& launch_adapter,
                       executor::comm::PhaseGate& startup_gate, JobManagerConfig config)
    : impl_(std::make_unique<Impl>(executor, store, queue, gpu_manager, log_streamer,
                                   identity_resolver, launch_adapter, startup_gate,
                                   std::move(config))) {}

JobManager::~JobManager() { static_cast<void>(stop()); }

JobManagerStartResult JobManager::start(const std::optional<recovery::RecoveryResult>& recovery) {
  if (impl_->started) {
    return {JobManagerStartCode::kAlreadyStarted, "job manager already started"};
  }
  if (impl_->stop_requested) {
    return {JobManagerStartCode::kInvalidConfig, "job manager was stopped and cannot restart"};
  }
  std::string error;
  if (!impl_->config.valid(error)) {
    return {JobManagerStartCode::kInvalidConfig, error};
  }
  if (!prepare_log_root(impl_->config.log_root, error)) {
    return {JobManagerStartCode::kLogRootInvalid, error};
  }

  const auto monitor_start = impl_->exit_monitor.start();
  if (!monitor_start.ok()) {
    return {JobManagerStartCode::kWorkersRejected,
            "exit monitor start failed: " + monitor_start.message};
  }
  const auto pump_start = impl_->log_pump.start();
  if (!pump_start.ok()) {
    impl_->exit_monitor.stop();
    return {JobManagerStartCode::kWorkersRejected, "log pump start failed: " + pump_start.message};
  }

  std::array<int, 2> wake_pipe{};
  if (::pipe2(wake_pipe.data(), O_CLOEXEC | O_NONBLOCK) < 0) {
    impl_->log_pump.stop();
    impl_->exit_monitor.stop();
    return {JobManagerStartCode::kWorkersRejected, "wake pipe creation failed"};
  }

  impl_->bridge = std::make_shared<StreamerBridge>(impl_->log_streamer);
  auto worker_storage = std::make_unique<ManagerWorker>(
      wake_pipe[0], impl_->commands, impl_->startup_gate, impl_->store, impl_->queue,
      impl_->gpu_manager, impl_->log_streamer, impl_->identity_resolver, impl_->launch_adapter,
      impl_->config, impl_->stats, impl_->stopping);
  worker_storage->set_wake_write(wake_pipe[1]);
  worker_storage->bind(impl_->executor, impl_->scheduler_runner, impl_->store_runner,
                       impl_->exit_monitor, impl_->log_pump, impl_->bridge);
  impl_->worker = worker_storage.get();
  impl_->wake_read = wake_pipe[0];
  impl_->wake_write = wake_pipe[1];

  // 恢复采纳在 worker 启动前完成（事件在通道中排队，worker 首轮排空）。
  worker_storage->arm_recovery(recovery);

  static std::atomic<std::uint64_t> instance_counter{0};
  const auto instance = instance_counter.fetch_add(1, std::memory_order_relaxed);
  executor::BlockingWorkerSpec spec;
  spec.name = "yori-job-manager-" + std::to_string(instance);
  spec.config.thread_name = "yori-job-mgr";
  spec.worker = std::move(worker_storage);
  impl_->handle = impl_->executor.start_worker(std::move(spec));
  if (!impl_->handle.started()) {
    impl_->worker = nullptr;
    impl_->bridge.reset();
    static_cast<void>(::close(wake_pipe[0]));
    static_cast<void>(::close(wake_pipe[1]));
    impl_->log_pump.stop();
    impl_->exit_monitor.stop();
    return {JobManagerStartCode::kWorkersRejected, "executor rejected the blocking worker"};
  }
  impl_->started = true;

  // 唤醒源挂钩（回调非阻塞，只写唤醒字节）：退出监视事件、GPU 状态事件。
  // 事件通道是 MpscChannel（无 fd 可 poll），与 LogStreamer 的变更监听同型。
  impl_->exit_monitor.set_event_listener([impl = impl_.get()]() noexcept { impl->kick(); });
  impl_->gpu_manager.set_event_listener([impl = impl_.get()]() noexcept { impl->kick(); });

  // 恢复完成触发调度（设计 9：恢复完成是调度触发事件）。
  Command command;
  command.kind = Command::Kind::kWake;
  command.trigger = scheduler::SchedulerTrigger::kRecoveryCompleted;
  static_cast<void>(impl_->commands.try_send(std::move(command)));
  impl_->worker->wakeup();
  return {JobManagerStartCode::kStarted, {}};
}

ipc::JobSubmitOutcome JobManager::submit_job(const job::JobSpec& spec) {
  if (impl_->stop_requested || !impl_->started) {
    return ipc::JobSubmitOutcome{ipc::JobSubmitOutcome::Code::kUnavailable, 0,
                                 "job manager is not running"};
  }
  Command command;
  command.kind = Command::Kind::kSubmit;
  command.submit_spec = spec;
  std::future<ipc::JobSubmitOutcome> completion = command.submit_ack.get_future();
  if (!impl_->commands.try_send(std::move(command))) {
    return ipc::JobSubmitOutcome{ipc::JobSubmitOutcome::Code::kUnavailable, 0,
                                 "job manager command channel is full"};
  }
  impl_->worker->wakeup();
  if (completion.wait_for(impl_->config.ack_timeout) != std::future_status::ready) {
    return ipc::JobSubmitOutcome{ipc::JobSubmitOutcome::Code::kUnavailable, 0,
                                 "job manager did not acknowledge submission"};
  }
  return completion.get();
}

ipc::JobCancelOutcome JobManager::cancel_job(std::uint64_t job_id) {
  if (impl_->stop_requested || !impl_->started) {
    return ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kUnavailable, 0,
                                 "job manager is not running"};
  }
  Command command;
  command.kind = Command::Kind::kCancel;
  command.cancel_job_id = job_id;
  std::future<ipc::JobCancelOutcome> completion = command.cancel_ack.get_future();
  if (!impl_->commands.try_send(std::move(command))) {
    return ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kUnavailable, 0,
                                 "job manager command channel is full"};
  }
  impl_->worker->wakeup();
  if (completion.wait_for(impl_->config.ack_timeout) != std::future_status::ready) {
    return ipc::JobCancelOutcome{ipc::JobCancelOutcome::Code::kUnavailable, 0,
                                 "job manager did not acknowledge cancellation"};
  }
  return completion.get();
}

ProcessExitMonitor& JobManager::exit_monitor() { return impl_->exit_monitor; }

LogPump& JobManager::log_pump() { return impl_->log_pump; }

JobManagerStats JobManager::stats() const {
  JobManagerStats result;
  const auto& stats = impl_->stats;
  result.commands_processed = stats.commands_processed.load(std::memory_order_relaxed);
  result.jobs_submitted = stats.jobs_submitted.load(std::memory_order_relaxed);
  result.jobs_launched = stats.jobs_launched.load(std::memory_order_relaxed);
  result.launch_failures = stats.launch_failures.load(std::memory_order_relaxed);
  result.jobs_finished = stats.jobs_finished.load(std::memory_order_relaxed);
  result.jobs_failed = stats.jobs_failed.load(std::memory_order_relaxed);
  result.jobs_cancelled = stats.jobs_cancelled.load(std::memory_order_relaxed);
  result.jobs_adopted = stats.jobs_adopted.load(std::memory_order_relaxed);
  result.recancels_armed = stats.recancels_armed.load(std::memory_order_relaxed);
  result.scheduler_runs = stats.scheduler_runs.load(std::memory_order_relaxed);
  result.scheduler_scheduled = stats.scheduler_scheduled.load(std::memory_order_relaxed);
  result.scheduler_failed = stats.scheduler_failed.load(std::memory_order_relaxed);
  result.store_write_failures = stats.store_write_failures.load(std::memory_order_relaxed);
  result.abandoned_at_stop = stats.abandoned_at_stop.load(std::memory_order_relaxed);
  result.active_supervised = stats.active_supervised.load(std::memory_order_relaxed);
  return result;
}

JobManagerStopCode JobManager::stop() {
  if (impl_->stop_requested) {
    return JobManagerStopCode::kAlreadyStopped;
  }
  impl_->stopping.store(true, std::memory_order_relaxed);
  if (!impl_->started) {
    impl_->stop_requested = true;
    return JobManagerStopCode::kNotRunning;
  }
  impl_->stop_requested = true;
  // 解除唤醒挂钩（先于 fd 关闭；GPU 周期任务已由 daemon 在阶段 ③ 停止）。
  impl_->gpu_manager.set_event_listener(nullptr);
  impl_->exit_monitor.set_event_listener(nullptr);
  if (impl_->worker != nullptr) {
    impl_->worker->wakeup();
  }
  impl_->handle.stop();  // join worker（串行路径内终态已落盘，EXEC-10 ⑦）
  if (impl_->worker != nullptr) {
    const std::size_t abandoned = impl_->worker->abandon_supervised();
    impl_->stats.abandoned_at_stop.fetch_add(abandoned, std::memory_order_relaxed);
    impl_->worker->shutdown_components();
    impl_->worker = nullptr;
  }
  impl_->bridge.reset();
  if (impl_->wake_read >= 0) {
    static_cast<void>(::close(impl_->wake_read));
    impl_->wake_read = -1;
  }
  if (impl_->wake_write >= 0) {
    static_cast<void>(::close(impl_->wake_write));
    impl_->wake_write = -1;
  }
  impl_->started = false;
  return JobManagerStopCode::kStopped;
}

}  // namespace yori::runtime
