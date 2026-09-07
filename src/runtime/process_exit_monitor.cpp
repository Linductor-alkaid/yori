#include "runtime/process_exit_monitor.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <executor/executor.hpp>
#include <future>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yori::runtime {
namespace {

// SIGCHLD 处理器只写一个字节到自管道（async-signal-safe）；worker 的 poll 因此唤醒。
std::atomic<int> g_sigchld_wake_fd{-1};

extern "C" void sigchld_write_wakeup(int) {
  const int fd = g_sigchld_wake_fd.load(std::memory_order_relaxed);
  if (fd < 0) {
    return;
  }
  const char byte = 1;
  ssize_t written = 0;
  do {
    written = ::write(fd, &byte, 1);
  } while (written < 0 && errno == EINTR);
}

struct RegisterCommand final {
  process::ProcessIdentity identity;
  std::promise<ExitRegisterResult> completion;
};

struct UnregisterCommand final {
  std::int64_t pid{0};
  std::promise<ExitUnregisterResult> completion;
};

struct Command final {
  enum class Kind : std::uint8_t { kRegister, kUnregister } kind{Kind::kRegister};
  RegisterCommand register_command;
  UnregisterCommand unregister_command;
};

ExitEvent make_exit_event(std::int64_t pid, const process::ProcessIdentity& identity,
                          process::ExitStatus status, bool identity_verified) {
  return ExitEvent{pid, identity, status, identity_verified};
}

class ReaperWorker final : public executor::IBlockingIoWorker {
 public:
  ReaperWorker(int wake_read, executor::comm::MpscChannel<Command>& commands,
               executor::comm::MpscChannel<ExitEvent>& events,
               std::atomic<std::size_t>& registered_count,
               std::atomic<std::uint64_t>& delivery_retries, std::atomic<bool>& stopping)
      : wake_read_(wake_read),
        commands_(commands),
        events_(events),
        registered_count_(registered_count),
        delivery_retries_(delivery_retries),
        stopping_(stopping) {}

  void run(executor::StopToken stop_token) override {
    while (!stop_token.stop_requested()) {
      wait_for_wakeup();
      drain_wake_pipe();
      apply_commands(stop_token);
      if (stop_token.stop_requested()) {
        break;
      }
      scan_registered(stop_token);
    }
  }

  void wakeup() noexcept override {
    const char byte = 1;
    ssize_t written = 0;
    do {
      written = ::write(wake_write_, &byte, 1);
    } while (written < 0 && errno == EINTR);
  }

  void set_wake_write(int fd) noexcept { wake_write_ = fd; }

 private:
  void wait_for_wakeup() noexcept {
    struct pollfd waiter {};
    waiter.fd = wake_read_;
    waiter.events = POLLIN;
    const int ready = ::poll(&waiter, 1, -1);
    if (ready < 0 && errno != EINTR) {
      // 自管道异常（不应发生）：退化为有界超时，避免忙等。
      struct pollfd backoff {};
      backoff.fd = -1;
      backoff.events = 0;
      ::poll(&backoff, 1, 100);
    }
  }

  void drain_wake_pipe() noexcept {
    char buffer[64];
    while (::read(wake_read_, buffer, sizeof(buffer)) > 0) {
    }
  }

  void apply_commands(const executor::StopToken& stop_token) {
    Command command;
    while (commands_.try_receive(command)) {
      if (command.kind == Command::Kind::kRegister) {
        apply_register(std::move(command.register_command));
      } else {
        apply_unregister(std::move(command.unregister_command));
      }
      if (stop_token.stop_requested()) {
        return;
      }
    }
  }

  void apply_register(RegisterCommand&& command) {
    const auto pid = command.identity.pid;
    if (!command.identity.valid()) {
      command.completion.set_value({ExitRegisterCode::kInvalidIdentity, "invalid identity"});
      return;
    }
    if (registry_.find(pid) != registry_.end()) {
      command.completion.set_value({ExitRegisterCode::kDuplicate, "pid already registered"});
      return;
    }
    registry_.emplace(pid, command.identity);
    registered_count_.store(registry_.size(), std::memory_order_relaxed);
    command.completion.set_value({ExitRegisterCode::kRegistered, {}});
    // 注册后立即补扫该 PID，覆盖注册前已退出的竞态。
    scan_one(pid, command.identity);
  }

  void apply_unregister(UnregisterCommand&& command) {
    const auto iter = registry_.find(command.pid);
    if (iter == registry_.end()) {
      command.completion.set_value({ExitUnregisterCode::kNotFound, "pid not registered"});
      return;
    }
    registry_.erase(iter);
    registered_count_.store(registry_.size(), std::memory_order_relaxed);
    command.completion.set_value({ExitUnregisterCode::kUnregistered, {}});
  }

  void scan_registered(const executor::StopToken& stop_token) {
    std::vector<std::int64_t> pids;
    pids.reserve(registry_.size());
    for (const auto& [pid, identity] : registry_) {
      static_cast<void>(identity);
      pids.push_back(pid);
    }
    for (const auto pid : pids) {
      if (stop_token.stop_requested()) {
        return;
      }
      const auto iter = registry_.find(pid);
      if (iter != registry_.end()) {
        scan_one(pid, iter->second);
      }
    }
  }

  void scan_one(std::int64_t pid, const process::ProcessIdentity& identity) {
    const auto current_ticks = process::read_process_start_ticks(pid);
    if (!current_ticks.has_value() || *current_ticks != identity.start_ticks) {
      // /proc 不可读或启动时间不一致：PID 已被回收复用，无法获得真实退出状态。
      deliver(make_exit_event(pid, identity, process::ExitStatus{}, false), pid);
      return;
    }
    int raw_status = 0;
    const pid_t reaped = ::waitpid(static_cast<pid_t>(pid), &raw_status, WNOHANG);
    if (reaped == 0) {
      return;  // 仍在运行（含僵尸前的正常状态）。
    }
    process::ExitStatus status;
    if (reaped == static_cast<pid_t>(pid)) {
      if (WIFEXITED(raw_status)) {
        status.reason = process::ExitReason::kExited;
        status.exit_code = WEXITSTATUS(raw_status);
      } else if (WIFSIGNALED(raw_status)) {
        status.reason = process::ExitReason::kSignaled;
        status.signal_number = WTERMSIG(raw_status);
      }
      deliver(make_exit_event(pid, identity, status, true), pid);
      return;
    }
    if (errno == ECHILD) {
      // 已被他人回收：状态不可知，显式投递 unknown。
      deliver(make_exit_event(pid, identity, process::ExitStatus{}, false), pid);
    }
  }

  // 退出事件不可丢弃：容量满时有界重试并计数；停止请求期间放弃投递（事件留在
  // 通道缓冲中的语义由 stop 时点决定，此路径只发生在 shutdown 尾部）。
  void deliver(ExitEvent event, std::int64_t pid) {
    registry_.erase(pid);
    registered_count_.store(registry_.size(), std::memory_order_relaxed);
    while (!stopping_.load(std::memory_order_relaxed)) {
      const ExitEvent attempt = event;
      const auto result = events_.send_for(attempt, std::chrono::seconds{5});
      if (result.ok) {
        return;
      }
      delivery_retries_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  int wake_read_{-1};
  int wake_write_{-1};
  executor::comm::MpscChannel<Command>& commands_;
  executor::comm::MpscChannel<ExitEvent>& events_;
  std::atomic<std::size_t>& registered_count_;
  std::atomic<std::uint64_t>& delivery_retries_;
  std::atomic<bool>& stopping_;
  std::unordered_map<std::int64_t, process::ProcessIdentity> registry_;
};

}  // namespace

class ProcessExitMonitor::Impl final {
 public:
  Impl(executor::Executor& executor_ref, std::size_t event_capacity, std::size_t command_capacity)
      : commands(executor::comm::ChannelOptions{command_capacity,
                                                executor::comm::DropPolicy::RejectNewest, true,
                                                "exit-monitor-commands"}),
        events(executor::comm::ChannelOptions{
            event_capacity, executor::comm::DropPolicy::RejectNewest, true, "exit-monitor-events"}),
        executor(executor_ref) {}

  executor::comm::MpscChannel<Command> commands;
  executor::comm::MpscChannel<ExitEvent> events;
  executor::Executor& executor;
  std::atomic<std::size_t> registered_count{0};
  std::atomic<std::uint64_t> delivery_retries{0};
  // blocking worker 由 Executor 持有；该指针在 start 后、stop 前有效，仅用于
  // 从调用方线程写入自管道唤醒。
  ReaperWorker* worker{nullptr};
  executor::WorkerHandle handle;
  struct sigaction previous_sigchld {};
  int wake_read{-1};
  int wake_write{-1};
  std::atomic<bool> worker_stopping{false};
  bool started{false};
  bool stop_requested{false};
  bool signal_installed{false};
};

ProcessExitMonitor::ProcessExitMonitor(executor::Executor& executor, std::size_t event_capacity,
                                       std::size_t command_capacity)
    : impl_(std::make_unique<Impl>(executor, event_capacity, command_capacity)) {}

ProcessExitMonitor::~ProcessExitMonitor() { stop(); }

ExitMonitorStartResult ProcessExitMonitor::start() {
  if (impl_->started) {
    return {ExitMonitorStartCode::kAlreadyStarted, "exit monitor already started"};
  }
  if (impl_->stop_requested) {
    return {ExitMonitorStartCode::kWorkerRejected, "exit monitor was stopped and cannot restart"};
  }

  std::array<int, 2> wake_pipe{};
  if (::pipe2(wake_pipe.data(), O_CLOEXEC | O_NONBLOCK) < 0) {
    return {ExitMonitorStartCode::kWorkerRejected, "wake pipe creation failed"};
  }

  g_sigchld_wake_fd.store(wake_pipe[1], std::memory_order_relaxed);
  struct sigaction action {};
  action.sa_handler = sigchld_write_wakeup;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (::sigaction(SIGCHLD, &action, &impl_->previous_sigchld) < 0) {
    g_sigchld_wake_fd.store(-1, std::memory_order_relaxed);
    static_cast<void>(::close(wake_pipe[0]));
    static_cast<void>(::close(wake_pipe[1]));
    return {ExitMonitorStartCode::kSignalInstallFailed, "sigaction(SIGCHLD) failed"};
  }
  impl_->signal_installed = true;

  auto worker_storage = std::make_unique<ReaperWorker>(
      wake_pipe[0], impl_->commands, impl_->events, impl_->registered_count,
      impl_->delivery_retries, impl_->worker_stopping);
  worker_storage->set_wake_write(wake_pipe[1]);

  impl_->worker = worker_storage.get();
  impl_->wake_read = wake_pipe[0];
  impl_->wake_write = wake_pipe[1];

  // Executor 的 blocking worker 名字单次注册不可复用（DuplicateName 语义），
  // 每个实例以计数后缀唯一化；daemon 生命周期内只有一个 ExitMonitor 实例。
  static std::atomic<std::uint64_t> instance_counter{0};
  const auto instance = instance_counter.fetch_add(1, std::memory_order_relaxed);
  executor::BlockingWorkerSpec spec;
  spec.name = "yori-exit-monitor-" + std::to_string(instance);
  spec.config.thread_name = "yori-exit-mon";
  spec.worker = std::move(worker_storage);
  impl_->handle = impl_->executor.start_worker(std::move(spec));
  if (!impl_->handle.started()) {
    impl_->worker = nullptr;
    g_sigchld_wake_fd.store(-1, std::memory_order_relaxed);
    ::sigaction(SIGCHLD, &impl_->previous_sigchld, nullptr);
    impl_->signal_installed = false;
    static_cast<void>(::close(wake_pipe[0]));
    static_cast<void>(::close(wake_pipe[1]));
    return {ExitMonitorStartCode::kWorkerRejected, "executor rejected the blocking worker"};
  }
  impl_->started = true;
  return {ExitMonitorStartCode::kStarted, {}};
}

ExitRegisterResult ProcessExitMonitor::register_process(process::ProcessIdentity identity) {
  if (impl_->stop_requested || !impl_->started) {
    return {ExitRegisterCode::kNotStarted, "exit monitor is not running"};
  }
  RegisterCommand command;
  command.identity = identity;
  std::future<ExitRegisterResult> completion = command.completion.get_future();
  Command envelope;
  envelope.kind = Command::Kind::kRegister;
  envelope.register_command = std::move(command);
  if (!impl_->commands.send_for(std::move(envelope), std::chrono::seconds{1}).ok) {
    return {ExitRegisterCode::kChannelFull, "command channel is full"};
  }
  impl_->worker->wakeup();
  if (completion.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    return {ExitRegisterCode::kAckTimeout, "worker did not acknowledge registration"};
  }
  return completion.get();
}

ExitUnregisterResult ProcessExitMonitor::unregister_process(std::int64_t pid) {
  if (impl_->stop_requested || !impl_->started) {
    return {ExitUnregisterCode::kNotStarted, "exit monitor is not running"};
  }
  UnregisterCommand command;
  command.pid = pid;
  std::future<ExitUnregisterResult> completion = command.completion.get_future();
  Command envelope;
  envelope.kind = Command::Kind::kUnregister;
  envelope.unregister_command = std::move(command);
  if (!impl_->commands.send_for(std::move(envelope), std::chrono::seconds{1}).ok) {
    return {ExitUnregisterCode::kChannelFull, "command channel is full"};
  }
  impl_->worker->wakeup();
  if (completion.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    return {ExitUnregisterCode::kAckTimeout, "worker did not acknowledge unregistration"};
  }
  return completion.get();
}

bool ProcessExitMonitor::try_receive_exit(ExitEvent& out) { return impl_->events.try_receive(out); }

bool ProcessExitMonitor::receive_exit_for(ExitEvent& out, std::chrono::milliseconds timeout) {
  return impl_->events.receive_for(out, timeout).ok;
}

std::size_t ProcessExitMonitor::registered_count() const noexcept {
  return impl_->registered_count.load(std::memory_order_relaxed);
}

std::uint64_t ProcessExitMonitor::delivery_retry_count() const noexcept {
  return impl_->delivery_retries.load(std::memory_order_relaxed);
}

void ProcessExitMonitor::stop() {
  impl_->worker_stopping.store(true, std::memory_order_relaxed);
  if (!impl_->started) {
    impl_->stop_requested = true;
    return;
  }
  impl_->stop_requested = true;
  g_sigchld_wake_fd.store(-1, std::memory_order_relaxed);
  if (impl_->signal_installed) {
    ::sigaction(SIGCHLD, &impl_->previous_sigchld, nullptr);
    impl_->signal_installed = false;
  }
  impl_->handle.stop();
  impl_->started = false;
  impl_->worker = nullptr;
  if (impl_->wake_read >= 0) {
    static_cast<void>(::close(impl_->wake_read));
    impl_->wake_read = -1;
  }
  if (impl_->wake_write >= 0) {
    static_cast<void>(::close(impl_->wake_write));
    impl_->wake_write = -1;
  }
}

}  // namespace yori::runtime
