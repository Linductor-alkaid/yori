#include "runtime/log_pump.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <executor/executor.hpp>
#include <functional>
#include <future>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yori::runtime {
namespace {

constexpr std::size_t kMaxChunkBytes = std::size_t{64} * 1024;

struct AttachCommand final {
  LogPumpJobInput input;
  std::promise<LogPumpAttachResult> completion;
};

struct DetachCommand final {
  job::JobId job{};
  std::promise<LogPumpDetachResult> completion;
};

struct Command final {
  enum class Kind : std::uint8_t { kAttach, kDetach } kind{Kind::kAttach};
  AttachCommand attach_command;
  DetachCommand detach_command;
};

// 完成监听槽：owner 线程设置、pump worker 线程读取，mutex 保护（与
// ProcessExitMonitor 的 listener 同型）。
struct DoneListenerSlot final {
  std::mutex mutex;
  std::function<void()> listener;
};

class PumpWorker final : public executor::IBlockingIoWorker {
 public:
  PumpWorker(int wake_read, executor::comm::MpscChannel<Command>& commands,
             executor::comm::MpscChannel<LogPumpDone>& done_events, std::atomic<bool>& stopping,
             std::shared_ptr<DoneListenerSlot> done_listener)
      : wake_read_(wake_read),
        commands_(commands),
        done_events_(done_events),
        stopping_(stopping),
        done_listener_(std::move(done_listener)) {}

  void run(executor::StopToken stop_token) override {
    while (!stop_token.stop_requested() && !stopping_.load(std::memory_order_relaxed)) {
      pump_once();
    }
    shutdown_entries();
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
  struct Stream final {
    int fd{-1};
    bool done{false};
  };

  struct Entry final {
    job::JobId job{};
    Stream stdout_stream;
    Stream stderr_stream;
    observe::LogSink sink;
    std::shared_ptr<LogChunkObserver> observer;
    std::string error;
  };

  // 一轮推进：poll 唤醒管道 + 全部流读端；命令先行应用（attach/detach 可能改变
  // 条目集合），就绪流随后排空。read owners 先收集再应用命令，避免悬空。
  void pump_once() {
    std::vector<pollfd> poll_set;
    poll_set.reserve(1 + entries_.size() * 2);
    std::vector<std::pair<std::uint64_t, observe::LogStreamKind>> owners;

    struct pollfd wake {};
    wake.fd = wake_read_;
    wake.events = POLLIN;
    poll_set.push_back(wake);

    for (auto& [job_value, entry] : entries_) {
      if (!entry.stdout_stream.done && entry.stdout_stream.fd >= 0) {
        poll_set.push_back({entry.stdout_stream.fd, POLLIN, 0});
        owners.emplace_back(job_value, observe::LogStreamKind::kStdout);
      }
      if (!entry.stderr_stream.done && entry.stderr_stream.fd >= 0) {
        poll_set.push_back({entry.stderr_stream.fd, POLLIN, 0});
        owners.emplace_back(job_value, observe::LogStreamKind::kStderr);
      }
    }

    const int ready = ::poll(poll_set.data(), static_cast<nfds_t>(poll_set.size()), -1);
    if (ready < 0 && errno != EINTR) {
      struct pollfd backoff {};
      backoff.fd = -1;
      ::poll(&backoff, 1, 100);
      return;
    }
    if (ready == 0) {
      return;
    }

    std::vector<std::pair<std::uint64_t, observe::LogStreamKind>> ready_streams;
    for (std::size_t i = 1; i < poll_set.size(); ++i) {
      if ((poll_set[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        ready_streams.push_back(owners[i - 1]);
      }
    }

    if ((poll_set[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      drain_wake_pipe();
      Command command;
      while (commands_.try_receive(command)) {
        if (command.kind == Command::Kind::kAttach) {
          apply_attach(std::move(command.attach_command));
        } else {
          apply_detach(std::move(command.detach_command));
        }
      }
    }

    for (const auto& [job_value, stream_kind] : ready_streams) {
      pump_stream(job_value, stream_kind);
    }
  }

  void drain_wake_pipe() noexcept {
    char buffer[64];
    while (::read(wake_read_, buffer, sizeof(buffer)) > 0) {
    }
  }

  void apply_attach(AttachCommand&& command) {
    LogPumpJobInput& input = command.input;
    if (!input.job.valid() || (!input.stdout_read.valid() && !input.stderr_read.valid()) ||
        !input.sink.is_open(observe::LogStreamKind::kStdout)) {
      command.completion.set_value({LogPumpAttachCode::kInvalidInput, "invalid pump input"});
      return;
    }
    const auto job_value = input.job.value();
    if (entries_.find(job_value) != entries_.end()) {
      command.completion.set_value({LogPumpAttachCode::kDuplicateJob, "job already attached"});
      return;
    }
    Entry entry;
    entry.job = input.job;
    entry.stdout_stream.fd = input.stdout_read.release();
    entry.stderr_stream.fd = input.stderr_read.release();
    entry.sink = std::move(input.sink);
    entry.observer = std::move(input.observer);
    entries_.emplace(job_value, std::move(entry));
    command.completion.set_value({LogPumpAttachCode::kAttached, {}});
    check_entry_completion(job_value);
  }

  void apply_detach(DetachCommand&& command) {
    const auto job_value = command.job.value();
    const auto iter = entries_.find(job_value);
    if (iter == entries_.end()) {
      command.completion.set_value({LogPumpDetachCode::kNotFound, "job not attached"});
      return;
    }
    finalize_entry(iter, false);
    command.completion.set_value({LogPumpDetachCode::kDetached, {}});
  }

  void pump_stream(std::uint64_t job_value, observe::LogStreamKind stream_kind) {
    const auto iter = entries_.find(job_value);
    if (iter == entries_.end()) {
      return;
    }
    Entry& entry = iter->second;
    Stream& stream =
        stream_kind == observe::LogStreamKind::kStdout ? entry.stdout_stream : entry.stderr_stream;
    if (stream.done || stream.fd < 0) {
      return;
    }
    std::array<char, kMaxChunkBytes> buffer{};
    const ssize_t n = ::read(stream.fd, buffer.data(), buffer.size());
    if (n > 0) {
      // 落盘优先：接受的字节区间随后发布给观察者（直播分发）。观察者的
      // 异常与失败不进入泵的错误路径（观察不得影响被观察者）。
      const std::uint64_t begin_offset = entry.sink.logical_offset(stream_kind);
      const auto result = entry.sink.append(
          stream_kind, std::string_view(buffer.data(), static_cast<std::size_t>(n)));
      if (!result.ok()) {
        entry.error += std::string(entry.error.empty() ? "" : "; ") + result.message;
      }
      if (entry.observer) {
        try {
          if (result.bytes_accepted > 0) {
            entry.observer->on_chunk(entry.job, stream_kind,
                                     std::string_view(buffer.data(), result.bytes_accepted),
                                     begin_offset, begin_offset + result.bytes_accepted);
          }
          if (result.dropped_bytes > 0) {
            entry.observer->on_drop(entry.job, stream_kind, begin_offset + result.bytes_accepted,
                                    result.dropped_bytes);
          }
        } catch (...) {  // NOLINT(bugprone-empty-catch)
          // 观察路径异常不落盘错误、不中断排空（设计 11.1：观察不得影响被
          // 观察者）；分发缺失由观察侧统计呈现。
        }
      }
      return;
    }
    if (n == 0 || errno != EINTR) {
      // EOF 或不可恢复读错误：该流结束。训练进程不受影响（写端此后得 EPIPE，
      // DEC-008 的 SIGPIPE 忽略语义）。
      if (n < 0) {
        entry.error += std::string(entry.error.empty() ? "" : "; ") + "stream read failed: errno " +
                       std::to_string(errno);
      }
      stream.done = true;
      static_cast<void>(::close(stream.fd));
      stream.fd = -1;
      check_entry_completion(job_value);
    }
  }

  void check_entry_completion(std::uint64_t job_value) {
    const auto iter = entries_.find(job_value);
    if (iter == entries_.end()) {
      return;
    }
    const Entry& entry = iter->second;
    const bool stdout_done = entry.stdout_stream.done || entry.stdout_stream.fd < 0;
    const bool stderr_done = entry.stderr_stream.done || entry.stderr_stream.fd < 0;
    if (stdout_done && stderr_done) {
      finalize_entry(iter);
    }
  }

  // 收尾并移除条目；post_done=true 时投递完成事件。
  void finalize_entry(typename std::unordered_map<std::uint64_t, Entry>::iterator iter,
                      bool post_done = true) {
    Entry entry = std::move(iter->second);
    entries_.erase(iter);
    close_entry_streams(entry);
    std::string close_error;
    if (entry.sink.is_open(observe::LogStreamKind::kStdout)) {
      static_cast<void>(entry.sink.close(observe::LogStreamKind::kStdout, close_error));
    }
    if (entry.sink.is_open(observe::LogStreamKind::kStderr)) {
      static_cast<void>(entry.sink.close(observe::LogStreamKind::kStderr, close_error));
    }
    if (!post_done) {
      return;
    }
    LogPumpDone done;
    done.job = entry.job;
    done.completed = true;
    done.error = entry.error;
    done.statistics = entry.sink.statistics();
    while (!stopping_.load(std::memory_order_relaxed)) {
      LogPumpDone attempt = done;
      if (done_events_.send_for(std::move(attempt), std::chrono::seconds{5}).ok) {
        // 投递成功即唤醒消费方：done 通道无 fd 可 poll，缺这一步则 EOF 发布
        // 汇合（退出事件 + 泵完成）会滞留到下一个无关唤醒（M7 遗留缺陷，
        // 跟随会话因此可无限悬挂）。
        std::function<void()> listener;
        {
          const std::lock_guard<std::mutex> lock(done_listener_->mutex);
          listener = done_listener_->listener;
        }
        if (listener) {
          listener();
        }
        return;
      }
    }
  }

  void close_entry_streams(Entry& entry) noexcept {
    if (entry.stdout_stream.fd >= 0) {
      static_cast<void>(::close(entry.stdout_stream.fd));
      entry.stdout_stream.fd = -1;
      entry.stdout_stream.done = true;
    }
    if (entry.stderr_stream.fd >= 0) {
      static_cast<void>(::close(entry.stderr_stream.fd));
      entry.stderr_stream.fd = -1;
      entry.stderr_stream.done = true;
    }
  }

  // 停止路径：关闭全部读端与落盘句柄，不投递事件（shutdown 语义）。
  void shutdown_entries() {
    for (auto& [job_value, entry] : entries_) {
      static_cast<void>(job_value);
      close_entry_streams(entry);
      std::string error;
      if (entry.sink.is_open(observe::LogStreamKind::kStdout)) {
        static_cast<void>(entry.sink.close(observe::LogStreamKind::kStdout, error));
      }
      if (entry.sink.is_open(observe::LogStreamKind::kStderr)) {
        static_cast<void>(entry.sink.close(observe::LogStreamKind::kStderr, error));
      }
    }
    entries_.clear();
  }

  int wake_read_{-1};
  int wake_write_{-1};
  executor::comm::MpscChannel<Command>& commands_;
  executor::comm::MpscChannel<LogPumpDone>& done_events_;
  std::atomic<bool>& stopping_;
  std::unordered_map<std::uint64_t, Entry> entries_;
  std::shared_ptr<DoneListenerSlot> done_listener_;
};

}  // namespace

class LogPump::Impl final {
 public:
  Impl(executor::Executor& executor_ref, std::size_t done_capacity)
      : commands(executor::comm::ChannelOptions{256, executor::comm::DropPolicy::RejectNewest, true,
                                                "log-pump-commands"}),
        done_events(executor::comm::ChannelOptions{
            done_capacity, executor::comm::DropPolicy::RejectNewest, true, "log-pump-done"}),
        executor(executor_ref) {}

  executor::comm::MpscChannel<Command> commands;
  executor::comm::MpscChannel<LogPumpDone> done_events;
  std::shared_ptr<DoneListenerSlot> done_listener = std::make_shared<DoneListenerSlot>();
  executor::Executor& executor;
  executor::WorkerHandle handle;
  int wake_read{-1};
  int wake_write{-1};
  std::atomic<bool> worker_stopping{false};
  bool started{false};
  bool stop_requested{false};
};

LogPump::LogPump(executor::Executor& executor, std::size_t done_capacity)
    : impl_(std::make_unique<Impl>(executor, done_capacity)) {}

LogPump::~LogPump() { stop(); }

LogPumpStartResult LogPump::start() {
  if (impl_->started) {
    return {LogPumpStartCode::kAlreadyStarted, "log pump already started"};
  }
  if (impl_->stop_requested) {
    return {LogPumpStartCode::kWorkerRejected, "log pump was stopped and cannot restart"};
  }

  std::array<int, 2> wake_pipe{};
  if (::pipe2(wake_pipe.data(), O_CLOEXEC | O_NONBLOCK) < 0) {
    return {LogPumpStartCode::kWorkerRejected, "wake pipe creation failed"};
  }

  auto worker_storage =
      std::make_unique<PumpWorker>(wake_pipe[0], impl_->commands, impl_->done_events,
                                   impl_->worker_stopping, impl_->done_listener);
  worker_storage->set_wake_write(wake_pipe[1]);

  // Executor 的 blocking worker 名字单次注册不可复用（DuplicateName 语义），
  // 每个实例以计数后缀唯一化；daemon 生命周期内只有一个 LogPump 实例。
  static std::atomic<std::uint64_t> instance_counter{0};
  const auto instance = instance_counter.fetch_add(1, std::memory_order_relaxed);
  executor::BlockingWorkerSpec spec;
  spec.name = "yori-log-pump-" + std::to_string(instance);
  spec.config.thread_name = "yori-log-pump";
  spec.worker = std::move(worker_storage);
  impl_->handle = impl_->executor.start_worker(std::move(spec));
  if (!impl_->handle.started()) {
    static_cast<void>(::close(wake_pipe[0]));
    static_cast<void>(::close(wake_pipe[1]));
    return {LogPumpStartCode::kWorkerRejected, "executor rejected the blocking worker"};
  }
  impl_->wake_read = wake_pipe[0];
  impl_->wake_write = wake_pipe[1];
  impl_->started = true;
  return {LogPumpStartCode::kStarted, {}};
}

LogPumpAttachResult LogPump::attach(LogPumpJobInput&& input) {
  if (impl_->stop_requested || !impl_->started) {
    return {LogPumpAttachCode::kNotStarted, "log pump is not running"};
  }
  AttachCommand command;
  command.input = std::move(input);
  std::future<LogPumpAttachResult> completion = command.completion.get_future();
  Command envelope;
  envelope.kind = Command::Kind::kAttach;
  envelope.attach_command = std::move(command);
  if (!impl_->commands.send_for(std::move(envelope), std::chrono::seconds{1}).ok) {
    return {LogPumpAttachCode::kChannelFull, "command channel is full"};
  }
  kick_worker();
  if (completion.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    return {LogPumpAttachCode::kAckTimeout, "worker did not acknowledge attach"};
  }
  return completion.get();
}

void LogPump::set_done_listener(std::function<void()> listener) {
  const std::lock_guard<std::mutex> lock(impl_->done_listener->mutex);
  impl_->done_listener->listener = std::move(listener);
}

LogPumpDetachResult LogPump::detach(job::JobId job) {
  if (impl_->stop_requested || !impl_->started) {
    return {LogPumpDetachCode::kNotStarted, "log pump is not running"};
  }
  DetachCommand command;
  command.job = job;
  std::future<LogPumpDetachResult> completion = command.completion.get_future();
  Command envelope;
  envelope.kind = Command::Kind::kDetach;
  envelope.detach_command = std::move(command);
  if (!impl_->commands.send_for(std::move(envelope), std::chrono::seconds{1}).ok) {
    return {LogPumpDetachCode::kChannelFull, "command channel is full"};
  }
  kick_worker();
  if (completion.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
    return {LogPumpDetachCode::kAckTimeout, "worker did not acknowledge detach"};
  }
  return completion.get();
}

bool LogPump::try_receive_done(LogPumpDone& out) { return impl_->done_events.try_receive(out); }

bool LogPump::receive_done_for(LogPumpDone& out, std::chrono::milliseconds timeout) {
  return impl_->done_events.receive_for(out, timeout).ok;
}

void LogPump::stop() {
  impl_->worker_stopping.store(true, std::memory_order_relaxed);
  if (!impl_->started) {
    impl_->stop_requested = true;
    return;
  }
  impl_->stop_requested = true;
  impl_->handle.stop();
  impl_->started = false;
  if (impl_->wake_read >= 0) {
    static_cast<void>(::close(impl_->wake_read));
    impl_->wake_read = -1;
  }
  if (impl_->wake_write >= 0) {
    static_cast<void>(::close(impl_->wake_write));
    impl_->wake_write = -1;
  }
}

void LogPump::kick_worker() noexcept {
  if (impl_->wake_write >= 0) {
    const char byte = 1;
    ssize_t written = 0;
    do {
      written = ::write(impl_->wake_write, &byte, 1);
    } while (written < 0 && errno == EINTR);
  }
}

}  // namespace yori::runtime
