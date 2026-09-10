#include "runtime/log_follow_service.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <executor/blocking_io.hpp>
#include <executor/executor.hpp>
#include <utility>

#include "ipc/uds_io.hpp"

namespace yori::runtime {
namespace {

using ipc::IpcError;
using ipc::IpcRequestKind;
using ipc::IpcStreamFrame;
using ipc::IpcStreamFrameKind;
using observe::LogStreamKind;

// 回放块每轮 poll 的入队上限（公平性：多会话共享一个 worker；回放块本身
// 即泵读块尺寸 64 KiB，无需再切帧）。
constexpr std::size_t kReplayChunksPerCycle = 16;

struct AddCommand final {
  int client_fd{-1};
  LogFollowSession session;
  // 初始 ack 帧 + 订阅期 GAP 帧（完整帧序列，直接进入会话写出缓冲）。
  std::vector<std::uint8_t> outgoing;
  bool job_finished{false};
  std::uint8_t job_state{0};
  std::optional<ipc::IpcExitStatus> exit;
};

struct Command final {
  enum class Kind : std::uint8_t { kAdd } kind{Kind::kAdd};
  AddCommand add;
};

// 跨线程统计（worker 更新，daemon/测试读取）。
struct AtomicStatistics final {
  std::atomic<std::uint64_t> sessions_started{0};
  std::atomic<std::uint64_t> sessions_completed{0};
  std::atomic<std::uint64_t> sessions_disconnected{0};
  std::atomic<std::uint64_t> sessions_backpressure{0};
  std::atomic<std::uint64_t> frames_sent{0};
};

// 单个跟随会话的 worker 侧状态。
struct Session final {
  int fd{-1};
  LogFollowSession handle;
  bool job_finished{false};
  std::uint8_t job_state{0};
  std::optional<ipc::IpcExitStatus> exit;
  bool eof_queued{false};
  bool backpressure{false};
  // 已入队终止帧，等待写出完成后回收。
  bool closing{false};

  struct StreamState final {
    // 回放进度（replay plan chunks 的下标）与已投递到的逻辑 offset。
    std::size_t replay_index{0};
    std::size_t replay_count{0};
    std::uint64_t delivered{0};
  };
  std::array<StreamState, 2> streams{};

  // 待写出的编码帧缓冲（append_stream_frame 追加）。
  std::vector<std::uint8_t> outgoing;
  std::size_t outgoing_sent{0};

  [[nodiscard]] std::size_t pending_bytes() const noexcept {
    return outgoing.size() - outgoing_sent;
  }
};

Session::StreamState& stream_state(Session& session, LogStreamKind kind) noexcept {
  return session.streams[kind == LogStreamKind::kStdout ? 0 : 1];
}

bool all_replay_done(Session& session) {
  for (const LogStreamKind kind : {LogStreamKind::kStdout, LogStreamKind::kStderr}) {
    if (stream_state(session, kind).replay_index < session.handle.replay(kind).chunks.size()) {
      return false;
    }
  }
  return true;
}

// 会话 worker（EXEC-03）：poll 会话 fd + 命令唤醒管道，串行推进全部会话。
// 单会话每轮的排空与回放量有上限（公平性）；socket 写带截止时间。
class FollowWorker final : public executor::IBlockingIoWorker {
 public:
  FollowWorker(int wake_read, executor::comm::MpscChannel<Command>& commands,
               LogFollowServiceConfig config, std::atomic<bool>& stopping,
               AtomicStatistics& statistics)
      : wake_read_(wake_read),
        commands_(commands),
        config_(config),
        stopping_(stopping),
        statistics_(statistics) {}

  void run(executor::StopToken stop_token) override {
    while (!stop_token.stop_requested() && !stopping_.load(std::memory_order_relaxed)) {
      cycle_once();
    }
    shutdown_sessions();
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
  enum class SessionEnd : std::uint8_t {
    kCompleted,
    kDisconnected,
    kBackpressure,
  };

  void cycle_once() {
    // poll 集合：唤醒管道 + 全部会话 fd（断开检测始终开启；有待写数据时
    // 请求 POLLOUT 以便及时写出）。100ms 有界 tick 保证写截止时间在
    // POLLOUT 不再就绪（缓冲塞满）时仍能推进——慢客户端由 write_full 的
    // 截止时间有界化，不依赖 socket 可写事件。
    std::vector<pollfd> waiters;
    waiters.reserve(1 + sessions_.size());
    std::vector<int> session_fds;
    session_fds.reserve(sessions_.size());

    struct pollfd wake {};
    wake.fd = wake_read_;
    wake.events = POLLIN;
    waiters.push_back(wake);

    for (auto& session : sessions_) {
      struct pollfd waiter {};
      waiter.fd = session->fd;
      waiter.events = POLLRDHUP;
      if (session->pending_bytes() > 0) {
        waiter.events |= POLLOUT;
      }
      waiters.push_back(waiter);
      session_fds.push_back(session->fd);
    }

    const int ready = ::poll(waiters.data(), static_cast<nfds_t>(waiters.size()), 100);
    if (ready < 0 && errno != EINTR) {
      struct pollfd backoff {};
      backoff.fd = -1;
      ::poll(&backoff, 1, 100);
      return;
    }
    // ready == 0（tick 到期）与有事件走同一推进路径：会话的排空/回放不依赖
    // fd 事件（订阅队列无 fd），写出由 write_full 自带截止时间有界化。

    if (ready > 0 && (waiters[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      drain_wake_pipe();
      Command command;
      while (commands_.try_receive(command)) {
        apply_add(std::move(command.add));
      }
    }

    // 以 fd 为键收集断开事件，避免回收导致的下标漂移。
    std::vector<int> hungup_fds;
    if (ready > 0) {
      for (std::size_t i = 1; i < waiters.size(); ++i) {
        if ((waiters[i].revents & (POLLRDHUP | POLLHUP | POLLERR)) != 0) {
          hungup_fds.push_back(session_fds[i - 1]);
        }
      }
    }

    // 对端已断开的会话直接回收（不再消耗写出）。
    for (const int fd : hungup_fds) {
      drop_session_by_fd(fd, SessionEnd::kDisconnected);
    }

    // 先推进（回放/排空/编码），后写出（不依赖 POLLOUT 就绪）。
    for (auto& session : sessions_) {
      advance_session(*session);
    }
    for (auto& session : sessions_) {
      if (session->pending_bytes() > 0) {
        flush_session(*session);
      }
    }
    collect_finished_sessions();
  }

  // 终止帧已写出的会话回收（关闭 fd，客户端见连接关闭）。
  void collect_finished_sessions() {
    for (std::size_t i = sessions_.size(); i > 0; --i) {
      Session& session = *sessions_[i - 1];
      if (session.closing && session.pending_bytes() == 0) {
        count_end(session.backpressure ? SessionEnd::kBackpressure : SessionEnd::kCompleted);
        static_cast<void>(::close(session.fd));
        session.fd = -1;
        sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(i - 1));
      }
    }
  }

  void drain_wake_pipe() noexcept {
    char buffer[64];
    while (::read(wake_read_, buffer, sizeof(buffer)) > 0) {
    }
  }

  void apply_add(AddCommand&& add) {
    auto session = std::make_unique<Session>();
    session->fd = add.client_fd;
    session->handle = std::move(add.session);
    session->job_finished = add.job_finished;
    session->job_state = add.job_state;
    session->exit = add.exit;
    session->outgoing = std::move(add.outgoing);
    for (const LogStreamKind kind : {LogStreamKind::kStdout, LogStreamKind::kStderr}) {
      const LogReplayPlan& plan = session->handle.replay(kind);
      Session::StreamState& state = stream_state(*session, kind);
      state.replay_index = 0;
      state.replay_count = plan.chunks.size();
      state.delivered = plan.start_offset;
    }
    statistics_.sessions_started.fetch_add(1, std::memory_order_relaxed);
    sessions_.push_back(std::move(session));
  }

  void advance_session(Session& session) {
    if (session.closing) {
      return;
    }
    advance_replay(session);
    advance_live(session);
  }

  void advance_replay(Session& session) {
    for (const LogStreamKind kind : {LogStreamKind::kStdout, LogStreamKind::kStderr}) {
      Session::StreamState& state = stream_state(session, kind);
      std::size_t appended = 0;
      while (state.replay_index < state.replay_count && appended < kReplayChunksPerCycle &&
             session.pending_bytes() < config_.max_session_buffer_bytes) {
        const LogChunk& chunk = session.handle.replay(kind).chunks[state.replay_index];
        IpcStreamFrame frame;
        frame.kind = IpcStreamFrameKind::kLogData;
        frame.stream = kind == LogStreamKind::kStdout ? 0 : 1;
        frame.begin_offset = chunk.begin_offset;
        frame.end_offset = chunk.end_offset;
        frame.data = chunk.data;
        static_cast<void>(ipc::append_stream_frame(frame, session.outgoing));
        ++appended;
        ++state.replay_index;
      }
    }
  }

  void advance_live(Session& session) {
    if (session.job_finished && all_replay_done(session)) {
      // 订阅时 Job 已终态（Topic 已关闭）：回放完成后以已知终态收尾。
      queue_eof(session);
      return;
    }

    executor::comm::TopicSubscription<LogChunk>& subscription = session.handle.subscription();
    std::uint32_t drained = 0;
    LogChunk chunk;
    while (drained < config_.max_drain_chunks_per_cycle &&
           session.pending_bytes() < config_.max_session_buffer_bytes) {
      if (!subscription.try_receive(chunk)) {
        if (subscription.is_closed() && !session.eof_queued) {
          // Topic 关闭但 EOF chunk 未能送达（订阅队列满时被拒）：以断开
          // 收尾，客户端可凭 offset 重连（缺口有界且显式）。
          session.closing = true;
        }
        return;
      }
      ++drained;
      if (chunk.kind == LogChunk::Kind::kEof) {
        session.job_state = chunk.job_state;
        session.exit = chunk.exit;
        queue_eof(session);
        return;
      }
      Session::StreamState& state = stream_state(session, chunk.stream);
      if (chunk.begin_offset < state.delivered) {
        continue;  // 回放与直播交界的重复块（同锁快照保证整块重复）。
      }
      if (chunk.begin_offset > state.delivered) {
        // 订阅队列溢出（RejectNewest）后的 offset 间断：慢客户端按设计
        // 11.4 断开并回 BACKPRESSURE 帧（含当前 offset）。
        queue_backpressure(session, chunk.stream, chunk.begin_offset);
        return;
      }
      IpcStreamFrame frame;
      frame.kind = IpcStreamFrameKind::kLogData;
      frame.stream = chunk.stream == LogStreamKind::kStdout ? 0 : 1;
      frame.begin_offset = chunk.begin_offset;
      frame.end_offset = chunk.end_offset;
      frame.data = std::move(chunk.data);
      static_cast<void>(ipc::append_stream_frame(frame, session.outgoing));
      state.delivered = chunk.end_offset;
    }
  }

  void queue_eof(Session& session) {
    if (session.eof_queued) {
      return;
    }
    session.eof_queued = true;
    session.closing = true;
    IpcStreamFrame frame;
    frame.kind = IpcStreamFrameKind::kLogEof;
    frame.job_state = session.job_state;
    frame.exit = session.exit;
    static_cast<void>(ipc::append_stream_frame(frame, session.outgoing));
  }

  void queue_backpressure(Session& session, LogStreamKind kind, std::uint64_t offset) {
    session.backpressure = true;
    session.closing = true;
    IpcStreamFrame frame;
    frame.kind = IpcStreamFrameKind::kLogBackpressure;
    frame.stream = kind == LogStreamKind::kStdout ? 0 : 1;
    frame.begin_offset = offset;
    static_cast<void>(ipc::append_stream_frame(frame, session.outgoing));
  }

  void flush_session(Session& session) {
    if (session.pending_bytes() == 0) {
      return;
    }
    const auto deadline = std::chrono::steady_clock::now() + config_.write_deadline;
    const std::size_t size = session.pending_bytes();
    const ipc::uds::FrameIoError error = ipc::uds::write_full(
        session.fd, session.outgoing.data() + session.outgoing_sent, size, deadline);
    if (error != ipc::uds::FrameIoError::kNone) {
      // 写超时/失败：会话有界回收（对端过慢或已消失）。
      drop_session_by_fd(session.fd, SessionEnd::kDisconnected);
      return;
    }
    statistics_.frames_sent.fetch_add(1, std::memory_order_relaxed);
    session.outgoing_sent += size;
    if (session.outgoing_sent == session.outgoing.size()) {
      session.outgoing.clear();
      session.outgoing_sent = 0;
    }
  }

  void drop_session_by_fd(int fd, SessionEnd end) {
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
      if (sessions_[i]->fd == fd) {
        count_end(end);
        static_cast<void>(::close(sessions_[i]->fd));
        sessions_[i]->fd = -1;
        sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  void count_end(SessionEnd end) {
    switch (end) {
      case SessionEnd::kCompleted:
        statistics_.sessions_completed.fetch_add(1, std::memory_order_relaxed);
        break;
      case SessionEnd::kDisconnected:
        statistics_.sessions_disconnected.fetch_add(1, std::memory_order_relaxed);
        break;
      case SessionEnd::kBackpressure:
        statistics_.sessions_backpressure.fetch_add(1, std::memory_order_relaxed);
        break;
    }
  }

  // EXEC-10 ②：断开全部跟随会话（客户端见连接关闭），未处理的入队命令
  // 一并回收 fd，不产生孤儿描述符。
  void shutdown_sessions() {
    Command command;
    while (commands_.try_receive(command)) {
      if (command.add.client_fd >= 0) {
        static_cast<void>(::close(command.add.client_fd));
        command.add.client_fd = -1;
      }
    }
    for (auto& session : sessions_) {
      static_cast<void>(::close(session->fd));
      session->fd = -1;
      // 停止断开的会话计入 disconnected（统计口径：非正常 EOF 收尾）。
      statistics_.sessions_disconnected.fetch_add(1, std::memory_order_relaxed);
    }
    sessions_.clear();
  }

  int wake_read_{-1};
  int wake_write_{-1};
  executor::comm::MpscChannel<Command>& commands_;
  LogFollowServiceConfig config_;
  std::atomic<bool>& stopping_;
  AtomicStatistics& statistics_;
  std::vector<std::unique_ptr<Session>> sessions_;
};

// 组装初始 ack 帧序列：[u32 len][ack payload] + （可选）每流 GAP 帧。
void append_length_prefixed(std::vector<std::uint8_t>& out,
                            const std::vector<std::uint8_t>& payload) {
  const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((size >> shift) & 0xffu));
  }
  out.insert(out.end(), payload.begin(), payload.end());
}

}  // namespace

const char* to_string(LogFollowStartCode code) noexcept {
  switch (code) {
    case LogFollowStartCode::kStarted:
      return "started";
    case LogFollowStartCode::kAlreadyStarted:
      return "already started";
    case LogFollowStartCode::kWorkerRejected:
      return "worker rejected";
  }
  return "unknown";
}

bool LogFollowServiceConfig::valid(std::string& error) const noexcept {
  if (write_deadline <= std::chrono::milliseconds::zero()) {
    error = "write deadline must be positive";
    return false;
  }
  if (max_session_buffer_bytes == 0) {
    error = "session buffer budget must be positive";
    return false;
  }
  if (max_drain_chunks_per_cycle == 0) {
    error = "drain chunks per cycle must be positive";
    return false;
  }
  return true;
}

struct LogFollowService::Impl final {
  Impl(executor::Executor& executor_ref, ipc::IpcService& service_ref, LogStreamer& streamer_ref,
       LogFollowServiceConfig config_ref)
      : commands(executor::comm::ChannelOptions{64, executor::comm::DropPolicy::RejectNewest, true,
                                                "log-follow-commands"}),
        executor(executor_ref),
        service(service_ref),
        streamer(streamer_ref),
        config(config_ref) {}

  executor::comm::MpscChannel<Command> commands;
  executor::Executor& executor;
  ipc::IpcService& service;
  LogStreamer& streamer;
  LogFollowServiceConfig config;
  executor::WorkerHandle handle;
  int wake_read{-1};
  int wake_write{-1};
  std::atomic<bool> worker_stopping{false};
  AtomicStatistics statistics{};
  bool started{false};
  bool stop_requested{false};

  void kick_worker() noexcept {
    if (wake_write >= 0) {
      const char byte = 1;
      ssize_t written = 0;
      do {
        written = ::write(wake_write, &byte, 1);
      } while (written < 0 && errno == EINTR);
    }
  }
};

LogFollowService::LogFollowService(executor::Executor& executor, ipc::IpcService& service,
                                   LogStreamer& streamer, LogFollowServiceConfig config)
    : impl_(std::make_unique<Impl>(executor, service, streamer, config)) {}

LogFollowService::~LogFollowService() { stop(); }

LogFollowStartResult LogFollowService::start() {
  if (impl_->started) {
    return {LogFollowStartCode::kAlreadyStarted, "log follow service already started"};
  }
  if (impl_->stop_requested) {
    return {LogFollowStartCode::kWorkerRejected,
            "log follow service was stopped and cannot restart"};
  }
  std::string validation_error;
  if (!impl_->config.valid(validation_error)) {
    return {LogFollowStartCode::kWorkerRejected, validation_error};
  }

  std::array<int, 2> wake_pipe{};
  if (::pipe2(wake_pipe.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
    return {LogFollowStartCode::kWorkerRejected, "wake pipe creation failed"};
  }

  auto worker = std::make_unique<FollowWorker>(wake_pipe[0], impl_->commands, impl_->config,
                                               impl_->worker_stopping, impl_->statistics);
  worker->set_wake_write(wake_pipe[1]);

  // blocking worker 名字单次注册不可复用（DuplicateName 语义），实例唯一化。
  static std::atomic<std::uint64_t> instance_counter{0};
  const auto instance = instance_counter.fetch_add(1, std::memory_order_relaxed);
  executor::BlockingWorkerSpec spec;
  spec.name = "yori-log-follow-" + std::to_string(instance);
  spec.config.thread_name = "yori-log-follow";
  spec.worker = std::move(worker);
  impl_->handle = impl_->executor.start_worker(std::move(spec));
  if (!impl_->handle.started()) {
    static_cast<void>(::close(wake_pipe[0]));
    static_cast<void>(::close(wake_pipe[1]));
    return {LogFollowStartCode::kWorkerRejected, "executor rejected the blocking worker"};
  }
  impl_->wake_read = wake_pipe[0];
  impl_->wake_write = wake_pipe[1];
  impl_->started = true;
  return {LogFollowStartCode::kStarted, {}};
}

void LogFollowService::stop() {
  if (impl_->stop_requested) {
    return;
  }
  impl_->stop_requested = true;
  impl_->worker_stopping.store(true, std::memory_order_relaxed);
  if (!impl_->started) {
    return;
  }
  // handle.stop() 请求停止、唤醒并 join；worker 退出路径断开全部会话并回收
  // 未处理的入队命令 fd。
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

LogFollowStatistics LogFollowService::statistics() const {
  LogFollowStatistics statistics;
  statistics.sessions_started = impl_->statistics.sessions_started.load(std::memory_order_relaxed);
  statistics.sessions_completed =
      impl_->statistics.sessions_completed.load(std::memory_order_relaxed);
  statistics.sessions_disconnected =
      impl_->statistics.sessions_disconnected.load(std::memory_order_relaxed);
  statistics.sessions_backpressure =
      impl_->statistics.sessions_backpressure.load(std::memory_order_relaxed);
  statistics.frames_sent = impl_->statistics.frames_sent.load(std::memory_order_relaxed);
  statistics.active_sessions = statistics.sessions_started - statistics.sessions_completed -
                               statistics.sessions_disconnected - statistics.sessions_backpressure;
  return statistics;
}

void LogFollowService::notify() noexcept { impl_->kick_worker(); }

UdsIpcStreamDelegate::Outcome LogFollowService::begin_stream(const ipc::PeerCredentials& peer,
                                                             const ipc::IpcRequest& request,
                                                             int client_fd) {
  Outcome outcome;
  outcome.taken_over = false;
  outcome.response.kind = IpcRequestKind::kLogsFollow;

  if (request.kind != IpcRequestKind::kLogsFollow) {
    outcome.response.error = IpcError::kProtocol;
    outcome.response.detail = "delegate only accepts logs-follow requests";
    return outcome;
  }
  if (!impl_->started || impl_->stop_requested) {
    outcome.response.error = IpcError::kUnsupported;
    outcome.response.detail = "follow service is not running";
    return outcome;
  }

  // 验证面（授权/存在性/已启动）在 IpcService；拒绝路径由服务器写回。
  outcome.response = impl_->service.validate_logs_follow(peer, request.logs_follow);
  if (outcome.response.error != IpcError::kNone) {
    return outcome;
  }
  const std::uint8_t validated_job_state = outcome.response.logs_follow.job_state;

  LogSubscribeResult subscribed =
      impl_->streamer.subscribe(job::JobId{request.logs_follow.job_id},
                                request.logs_follow.since_stdout, request.logs_follow.since_stderr);
  switch (subscribed.code) {
    case LogSubscribeCode::kSubscribed:
      break;
    case LogSubscribeCode::kJobUnknown:
      outcome.response.error = IpcError::kNotAvailable;
      outcome.response.detail =
          "job has no live log source on this daemon; use 'yori logs' snapshot";
      return outcome;
    case LogSubscribeCode::kSessionLimitJob:
    case LogSubscribeCode::kSessionLimitTotal:
      outcome.response.error = IpcError::kLimit;
      outcome.response.detail = subscribed.message;
      return outcome;
  }

  // 初始 ack：起始 offset 为 GAP 校正后的回放起点；订阅期 GAP 帧随 ack
  // 帧序列先行入队（会话建立即告知跳变区间）。
  const LogReplayPlan& stdout_plan = subscribed.session.replay(LogStreamKind::kStdout);
  const LogReplayPlan& stderr_plan = subscribed.session.replay(LogStreamKind::kStderr);
  ipc::IpcResponse ack;
  ack.kind = IpcRequestKind::kLogsFollow;
  ack.error = IpcError::kNone;
  // Job 状态以验证面的 store 快照为准（streamer 侧仅在终态后有值）。
  ack.logs_follow.job_state = validated_job_state;
  ack.logs_follow.stdout_offset = stdout_plan.start_offset;
  ack.logs_follow.stderr_offset = stderr_plan.start_offset;

  AddCommand add;
  add.client_fd = client_fd;
  add.job_finished = subscribed.finished;
  add.job_state = subscribed.job_state;
  add.exit = subscribed.exit;
  add.session = std::move(subscribed.session);

  std::vector<std::uint8_t> ack_payload;
  if (!ipc::encode_response_payload(ack, ack_payload)) {
    outcome.response.error = IpcError::kInternal;
    outcome.response.detail = "failed to encode follow ack";
    return outcome;  // session 析构归还计数。
  }
  append_length_prefixed(add.outgoing, ack_payload);

  auto append_gap = [&add](LogStreamKind kind, const LogReplayPlan& plan) {
    if (plan.gap_bytes == 0) {
      return;
    }
    IpcStreamFrame frame;
    frame.kind = IpcStreamFrameKind::kLogGap;
    frame.stream = kind == LogStreamKind::kStdout ? 0 : 1;
    frame.begin_offset = plan.start_offset - plan.gap_bytes;
    frame.end_offset = plan.start_offset;
    static_cast<void>(ipc::append_stream_frame(frame, add.outgoing));
  };
  append_gap(LogStreamKind::kStdout, stdout_plan);
  append_gap(LogStreamKind::kStderr, stderr_plan);

  // 入队成功即完成 fd 移交（worker 兜底回收，包括停止路径）。
  Command command;
  command.add = std::move(add);
  if (!impl_->commands.send_for(std::move(command), std::chrono::seconds{1}).ok) {
    outcome.response.error = IpcError::kLimit;
    outcome.response.detail = "follow admission queue is full";
    return outcome;
  }
  impl_->kick_worker();
  outcome.taken_over = true;
  return outcome;
}

}  // namespace yori::runtime
