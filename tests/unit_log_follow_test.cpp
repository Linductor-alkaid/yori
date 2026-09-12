#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_protocol.hpp>
#include <yori/ipc/ipc_service.hpp>

#include "runtime/executor_runtime.hpp"
#include "runtime/log_follow_service.hpp"
#include "runtime/log_streamer.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

namespace {

using namespace yori;
using namespace yori::runtime;
using namespace yori::ipc;
using observe::LogStreamKind;
using yori::job::JobId;
using yori::job::JobState;

constexpr std::uint32_t kAliceUid = 1000;
constexpr std::uint32_t kAliceGid = 1000;

// ---------------------------------------------------------------------------
// 基础设施。
// ---------------------------------------------------------------------------

class FakeGpuStatus final : public GpuStatusSource {
 public:
  bool try_get_snapshot(yori::gpu::GpuObservationSnapshot& out) override {
    static_cast<void>(out);
    return false;
  }
};

class FakeLogReader final : public LogSnapshotReader {
 public:
  LogTailResult read_tail(const std::string& path, std::uint32_t max_bytes) override {
    static_cast<void>(path);
    static_cast<void>(max_bytes);
    return LogTailResult{true, false, {}};
  }
};

// UDS socketpair 一端作为“客户端”，另一端交给会话 worker。
class ClientConnection final {
 public:
  ClientConnection() {
    YORI_CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds_) == 0);
  }
  ~ClientConnection() {
    close_client();
    if (fds_[1] >= 0) {
      static_cast<void>(::close(fds_[1]));
    }
  }
  ClientConnection(const ClientConnection&) = delete;
  ClientConnection& operator=(const ClientConnection&) = delete;

  [[nodiscard]] int client_fd() const noexcept { return fds_[0]; }
  [[nodiscard]] int take_server_fd() noexcept { return std::exchange(fds_[1], -1); }
  void close_client() noexcept {
    if (fds_[0] >= 0) {
      static_cast<void>(::close(fds_[0]));
      fds_[0] = -1;
    }
  }

 private:
  int fds_[2]{-1, -1};
};

// 阻塞读取一个完整帧（u32 前缀 + payload）。
bool read_frame(int fd, std::vector<std::uint8_t>& payload) {
  std::uint8_t header[4] = {};
  std::size_t received = 0;
  while (received < sizeof(header)) {
    const ssize_t n = ::recv(fd, header + received, sizeof(header) - received, 0);
    if (n <= 0) {
      return false;
    }
    received += static_cast<std::size_t>(n);
  }
  const std::uint32_t length =
      static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8) |
      (static_cast<std::uint32_t>(header[2]) << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
  if (length < 2 || length > IpcProtocolLimits::kMaxPayloadBytes) {
    return false;
  }
  payload.resize(length);
  received = 0;
  while (received < length) {
    const ssize_t n = ::recv(fd, payload.data() + received, length - received, 0);
    if (n <= 0) {
      return false;
    }
    received += static_cast<std::size_t>(n);
  }
  return true;
}

bool frame_readable(int fd, int timeout_ms) {
  struct pollfd waiter {};
  waiter.fd = fd;
  waiter.events = POLLIN;
  const int ready = ::poll(&waiter, 1, timeout_ms);
  return ready > 0 && (waiter.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}

// 排空接收缓冲但不消费（探测是否还有未读数据）。
bool connection_closed(int fd, int timeout_ms) {
  struct pollfd waiter {};
  waiter.fd = fd;
  waiter.events = POLLIN;
  const int ready = ::poll(&waiter, 1, timeout_ms);
  if (ready <= 0) {
    return false;
  }
  if ((waiter.revents & POLLHUP) != 0) {
    return true;
  }
  if ((waiter.revents & POLLIN) != 0) {
    char probe = 0;
    const ssize_t n = ::recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0;
  }
  return false;
}

// JobControl 空实现：跟随会话测试不触发状态变更。
class NullJobControl final : public yori::ipc::JobControl {
 public:
  yori::ipc::JobSubmitOutcome submit_job(const yori::job::JobSpec&) override {
    return yori::ipc::JobSubmitOutcome{yori::ipc::JobSubmitOutcome::Code::kUnavailable, 0,
                                       "null job control"};
  }
  yori::ipc::JobCancelOutcome cancel_job(std::uint64_t) override {
    return yori::ipc::JobCancelOutcome{yori::ipc::JobCancelOutcome::Code::kUnavailable, 0,
                                       "null job control"};
  }
};

// ScheduleStatusSource 空实现：跟随会话测试不消费调度评估。
class NullScheduleStatus final : public yori::ipc::ScheduleStatusSource {
 public:
  bool try_get_schedule_evaluation(yori::scheduler::ScheduleEvaluation&) override { return false; }
};
struct FollowFixture final {
  FollowFixture() {
    std::string error;
    YORI_CHECK(runtime.initialize({}, error));

    IpcServiceConfig service_config;
    service_config.admin_gids = {3000};
    // 守护委派的假实现（M7 起 submit/cancel 走 JobControl）：跟随会话测试
    // 不触发状态变更，静态空实现即可。
    control = std::make_unique<NullJobControl>();
    schedule_status = std::make_unique<NullScheduleStatus>();
    service = std::make_unique<IpcService>(service_config, store, gpu_status, log_reader, *control,
                                           *schedule_status);

    streamer_config.subscription_capacity = 4;
    streamer_config.backlog_bytes_per_stream = LogStreamerConfig::kMinBacklogBytes;
    streamer = std::make_unique<LogStreamer>(streamer_config);
    follow_config.write_deadline = std::chrono::milliseconds{500};
    follow_config.max_session_buffer_bytes = 8192;
    follow =
        std::make_unique<LogFollowService>(runtime.executor(), *service, *streamer, follow_config);
    // Topic 无 fd 可 poll：发布经变更监听唤醒会话 worker。
    LogFollowService* follow_ptr = follow.get();
    streamer->set_change_listener([follow_ptr] { follow_ptr->notify(); });
    YORI_CHECK(follow->start().ok());
  }

  ~FollowFixture() {
    if (streamer) {
      streamer->set_change_listener(nullptr);
    }
    follow.reset();
    streamer.reset();
    service.reset();
    static_cast<void>(runtime.shutdown());
  }

  // 在 store 中造一个已启动（STARTING + log_path + lease）的 Job；默认同时注册
  // 日志源（register_logs=false 用于构造“store 有、streamer 无”的拒绝路径）。
  JobId seed_started_job(std::uint64_t id, bool register_logs = true) {
    yori::job::JobSpec spec;
    spec.owner_uid = kAliceUid;
    spec.owner_gid = kAliceGid;
    spec.argv = {"train"};
    spec.cwd = "/srv";
    spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{10}};

    yori::store::StoredJob record;
    record.id = JobId{id};
    record.spec = spec;
    record.state = JobState::kQueued;
    record.revision = 0;
    yori::store::StateMutation create;
    create.expected_revision = store.load().snapshot.revision;
    create.create_jobs.push_back(record);
    YORI_CHECK(store.apply(create).ok());

    yori::store::StoredJob starting = record;
    starting.state = JobState::kStarting;
    starting.revision = 1;
    starting.execution.log_path = "/var/lib/yori/jobs/" + std::to_string(id);
    yori::store::StateMutation update;
    update.expected_revision = store.load().snapshot.revision;
    update.update_jobs.push_back(std::move(starting));
    update.acquire_leases.push_back(yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-x"}, JobId{id}});
    YORI_CHECK(store.apply(update).ok());

    if (register_logs) {
      std::string streamer_error;
      YORI_CHECK(streamer->register_job(JobId{id}, streamer_error) == LogRegisterCode::kRegistered);
    }
    return JobId{id};
  }

  [[nodiscard]] PeerCredentials alice() const { return PeerCredentials{kAliceUid, kAliceGid, 1}; }

  [[nodiscard]] IpcRequest follow_request(
      std::uint64_t job_id, std::optional<std::uint64_t> since_stdout = std::nullopt,
      std::optional<std::uint64_t> since_stderr = std::nullopt) const {
    IpcRequest request;
    request.kind = IpcRequestKind::kLogsFollow;
    request.logs_follow.job_id = job_id;
    request.logs_follow.since_stdout = since_stdout;
    request.logs_follow.since_stderr = since_stderr;
    return request;
  }

  // 经委派接管连接；返回是否接管成功。
  bool begin(ClientConnection& connection, const IpcRequest& request) {
    const UdsIpcStreamDelegate::Outcome outcome =
        follow->begin_stream(alice(), request, connection.take_server_fd());
    if (!outcome.taken_over) {
      last_rejection_ = outcome.response;
      return false;
    }
    return true;
  }

  ExecutorRuntime runtime;
  yori::testing::InMemoryStateStore store;
  FakeGpuStatus gpu_status;
  FakeLogReader log_reader;
  std::unique_ptr<NullScheduleStatus> schedule_status;
  std::unique_ptr<IpcService> service;
  std::unique_ptr<NullJobControl> control;
  LogStreamerConfig streamer_config;
  std::unique_ptr<LogStreamer> streamer;
  LogFollowServiceConfig follow_config;
  std::unique_ptr<LogFollowService> follow;
  IpcResponse last_rejection_{};
};

// ---------------------------------------------------------------------------
// 六场景（RULE-11）+ M6 专项语义。
// ---------------------------------------------------------------------------

// 场景 1：正常完成——数据帧 -> EOF 帧 -> 连接关闭；统计 completed。
void test_normal_completion_with_eof() {
  FollowFixture fixture;
  const JobId job = fixture.seed_started_job(1);

  ClientConnection connection;
  YORI_CHECK(fixture.begin(connection, fixture.follow_request(1)));

  // 初始 ack。
  std::vector<std::uint8_t> payload;
  YORI_CHECK(read_frame(connection.client_fd(), payload));
  const IpcResponseDecodeResult ack = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(ack.ok());
  YORI_CHECK(ack.value.kind == IpcRequestKind::kLogsFollow);
  YORI_CHECK(ack.value.error == IpcError::kNone);
  YORI_CHECK(ack.value.logs_follow.job_state == static_cast<std::uint8_t>(JobState::kStarting));
  YORI_CHECK(ack.value.logs_follow.stdout_offset == 0);

  // 直播数据 + 终态 EOF。
  YORI_CHECK(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, "hello", 0, 5) ==
             LogPublishCode::kPublished);
  const IpcExitStatus exit_status{true, 0};
  YORI_CHECK(fixture.streamer->finish_job(job, static_cast<std::uint8_t>(JobState::kFinished),
                                          exit_status) == LogFinishCode::kFinished);

  YORI_CHECK(read_frame(connection.client_fd(), payload));
  const IpcStreamFrameDecodeResult data =
      decode_stream_frame_payload(payload.data(), payload.size());
  YORI_CHECK(data.ok());
  YORI_CHECK(data.value.kind == IpcStreamFrameKind::kLogData);
  YORI_CHECK(data.value.begin_offset == 0 && data.value.end_offset == 5);

  YORI_CHECK(read_frame(connection.client_fd(), payload));
  const IpcStreamFrameDecodeResult eof =
      decode_stream_frame_payload(payload.data(), payload.size());
  YORI_CHECK(eof.ok());
  YORI_CHECK(eof.value.kind == IpcStreamFrameKind::kLogEof);
  YORI_CHECK(eof.value.job_state == static_cast<std::uint8_t>(JobState::kFinished));
  YORI_CHECK(eof.value.exit && eof.value.exit->exited_normally && eof.value.exit->code == 0);

  YORI_CHECK(connection_closed(connection.client_fd(), 2000));
  for (int i = 0; i < 100; ++i) {
    if (fixture.follow->statistics().sessions_completed >= 1) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  YORI_CHECK(fixture.follow->statistics().sessions_completed == 1);
  YORI_CHECK(fixture.follow->statistics().active_sessions == 0);
}

// 场景 2：对端异常断开——会话回收、无泄漏、落盘主路径不受影响。
void test_peer_disconnect_recovery() {
  FollowFixture fixture;
  const JobId job = fixture.seed_started_job(2);

  ClientConnection connection;
  YORI_CHECK(fixture.begin(connection, fixture.follow_request(2)));
  YORI_CHECK(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, "data", 0, 4) ==
             LogPublishCode::kPublished);
  std::vector<std::uint8_t> payload;
  YORI_CHECK(read_frame(connection.client_fd(), payload));  // ack
  YORI_CHECK(read_frame(connection.client_fd(), payload));  // data
  connection.close_client();                                // 对端消失（RST/FIN）

  // 客户端断开后：流仍可发布（观察不影响被观察者），会话计数归零。
  YORI_CHECK(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, "more", 4, 8) ==
             LogPublishCode::kPublished);
  for (int i = 0; i < 100; ++i) {
    if (fixture.follow->statistics().active_sessions == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  YORI_CHECK(fixture.follow->statistics().active_sessions == 0);
  YORI_CHECK(fixture.follow->statistics().sessions_disconnected >= 1);
}

// 场景 3：提交拒绝——store 无此 Job kNotFound；streamer 未注册 kNotAvailable；
// 授权失败 DENIED（服务器写回普通响应，不接管连接）。
void test_admission_rejection() {
  FollowFixture fixture;
  const JobId job = fixture.seed_started_job(3, /*register_logs=*/false);
  static_cast<void>(job);

  // store 中不存在的 Job。
  ClientConnection missing;
  const UdsIpcStreamDelegate::Outcome not_found = fixture.follow->begin_stream(
      fixture.alice(), fixture.follow_request(99), missing.take_server_fd());
  YORI_CHECK(!not_found.taken_over);
  YORI_CHECK(not_found.response.error == IpcError::kNotFound);
  static_cast<void>(::close(missing.client_fd()));

  // store 有、streamer 无（daemon 重启后无活跃日志源）：kNotAvailable。
  ClientConnection unregistered;
  const UdsIpcStreamDelegate::Outcome not_available = fixture.follow->begin_stream(
      fixture.alice(), fixture.follow_request(3), unregistered.take_server_fd());
  YORI_CHECK(!not_available.taken_over);
  YORI_CHECK(not_available.response.error == IpcError::kNotAvailable);
  static_cast<void>(::close(unregistered.client_fd()));

  // 验证面拒绝（非 owner）走普通响应路径。
  ClientConnection denied;
  IpcRequest request = fixture.follow_request(3);
  const UdsIpcStreamDelegate::Outcome outcome = fixture.follow->begin_stream(
      PeerCredentials{kAliceUid + 1, kAliceGid + 1, 2}, request, denied.take_server_fd());
  YORI_CHECK(!outcome.taken_over);
  YORI_CHECK(outcome.response.error == IpcError::kDenied);
  // 拒绝路径的 fd 由调用方（服务器）负责；此处模拟服务器写回后关闭。
  static_cast<void>(::close(denied.client_fd()));
}

// 场景 3 续：每 Job 会话上限的拒绝路径（独立夹具以配置小上限）。
void test_session_limit_rejection() {
  FollowFixture fixture;
  fixture.follow.reset();
  fixture.streamer.reset();
  LogStreamerConfig streamer_config;
  streamer_config.max_sessions_per_job = 1;
  streamer_config.subscription_capacity = 4;
  fixture.streamer = std::make_unique<LogStreamer>(streamer_config);
  LogFollowServiceConfig follow_config;
  follow_config.write_deadline = std::chrono::milliseconds{500};
  fixture.follow = std::make_unique<LogFollowService>(fixture.runtime.executor(), *fixture.service,
                                                      *fixture.streamer, follow_config);
  LogFollowService* follow_ptr = fixture.follow.get();
  fixture.streamer->set_change_listener([follow_ptr] { follow_ptr->notify(); });
  YORI_CHECK(fixture.follow->start().ok());

  const JobId job = fixture.seed_started_job(4);
  static_cast<void>(job);

  ClientConnection first;
  YORI_CHECK(fixture.begin(first, fixture.follow_request(4)));

  ClientConnection second;
  const UdsIpcStreamDelegate::Outcome rejected = fixture.follow->begin_stream(
      fixture.alice(), fixture.follow_request(4), second.take_server_fd());
  YORI_CHECK(!rejected.taken_over);
  YORI_CHECK(rejected.response.error == IpcError::kLimit);
  static_cast<void>(::close(second.client_fd()));  // 服务器拒绝后关闭。
}

// 场景 4：执行中取消——活跃流中客户端关闭，会话回收且流继续。
void test_cancel_mid_stream() {
  FollowFixture fixture;
  const JobId job = fixture.seed_started_job(5);

  ClientConnection connection;
  YORI_CHECK(fixture.begin(connection, fixture.follow_request(5)));
  std::vector<std::uint8_t> payload;
  YORI_CHECK(read_frame(connection.client_fd(), payload));  // ack

  std::uint64_t offset = 0;
  for (int round = 0; round < 3; ++round) {
    const std::string chunk = "round-" + std::to_string(round);
    YORI_CHECK(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, chunk, offset,
                                               offset + chunk.size()) ==
               LogPublishCode::kPublished);
    offset += chunk.size();
    YORI_CHECK(read_frame(connection.client_fd(), payload));
  }
  connection.close_client();  // 执行中取消（Ctrl-C 语义）。

  YORI_CHECK(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, "after", offset,
                                             offset + 5) == LogPublishCode::kPublished);
  for (int i = 0; i < 100; ++i) {
    if (fixture.follow->statistics().active_sessions == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  YORI_CHECK(fixture.follow->statistics().active_sessions == 0);
}

// 场景 5：写超时——客户端不读、缓冲塞满、发布持续：会话被有界回收。
void test_write_timeout_bounded_recovery() {
  FollowFixture fixture;
  fixture.follow.reset();
  fixture.streamer.reset();
  LogStreamerConfig streamer_config;
  streamer_config.subscription_capacity = 8;
  fixture.streamer = std::make_unique<LogStreamer>(streamer_config);
  LogFollowServiceConfig follow_config;
  follow_config.write_deadline = std::chrono::milliseconds{200};
  follow_config.max_session_buffer_bytes = 4096;
  fixture.follow = std::make_unique<LogFollowService>(fixture.runtime.executor(), *fixture.service,
                                                      *fixture.streamer, follow_config);
  LogFollowService* follow_ptr = fixture.follow.get();
  fixture.streamer->set_change_listener([follow_ptr] { follow_ptr->notify(); });
  YORI_CHECK(fixture.follow->start().ok());
  const JobId job = fixture.seed_started_job(6);

  // 缩小收发缓冲，让“客户端不读”确定性转化为写阻塞（默认缓冲会吸收全部
  // 64 KiB，超时路径不可达）。
  ClientConnection connection;
  const int server_fd = connection.take_server_fd();
  const int small_buffer = 1024;
  static_cast<void>(
      ::setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)));
  static_cast<void>(::setsockopt(connection.client_fd(), SOL_SOCKET, SO_RCVBUF, &small_buffer,
                                 sizeof(small_buffer)));
  const UdsIpcStreamDelegate::Outcome outcome =
      fixture.follow->begin_stream(fixture.alice(), fixture.follow_request(6), server_fd);
  YORI_CHECK(outcome.taken_over);

  // 持续发布但不读：写出缓冲满 -> 写阻塞 -> 截止时间到 -> 会话回收。
  const std::string block(1024, 'x');
  std::uint64_t offset = 0;
  for (int i = 0; i < 64; ++i) {
    static_cast<void>(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, block, offset,
                                                      offset + block.size()));
    offset += block.size();
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  for (int i = 0; i < 200; ++i) {
    if (fixture.follow->statistics().active_sessions == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  YORI_CHECK(fixture.follow->statistics().active_sessions == 0);
  connection.close_client();
}

// 场景 6：shutdown——活跃会话随服务停止全部断开（客户端见连接关闭）。
void test_shutdown_closes_sessions() {
  FollowFixture fixture;
  static_cast<void>(fixture.seed_started_job(7));

  ClientConnection connection;
  YORI_CHECK(fixture.begin(connection, fixture.follow_request(7)));
  std::vector<std::uint8_t> payload;
  YORI_CHECK(read_frame(connection.client_fd(), payload));  // ack

  fixture.follow->stop();
  YORI_CHECK(connection_closed(connection.client_fd(), 2000));
  fixture.follow.reset();  // 夹具析构不再重复 stop。
}

// M6 专项：慢客户端 BACKPRESSURE——订阅队列溢出后以 offset 间断检出，
// 回 BACKPRESSURE 帧并断开（不静默丢弃）。
void test_slow_client_backpressure() {
  FollowFixture fixture;
  const JobId job = fixture.seed_started_job(8);

  // 缩小收发缓冲：客户端不读时写出缓冲保持满、排空停止、订阅队列（4）
  // 确定性溢出——不依赖发布与 worker 的调度时序。
  ClientConnection connection;
  const int server_fd = connection.take_server_fd();
  const int small_buffer = 1024;
  static_cast<void>(
      ::setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)));
  static_cast<void>(::setsockopt(connection.client_fd(), SOL_SOCKET, SO_RCVBUF, &small_buffer,
                                 sizeof(small_buffer)));
  const UdsIpcStreamDelegate::Outcome outcome =
      fixture.follow->begin_stream(fixture.alice(), fixture.follow_request(8), server_fd);
  YORI_CHECK(outcome.taken_over);
  std::vector<std::uint8_t> payload;
  YORI_CHECK(read_frame(connection.client_fd(), payload));  // ack

  // 阶段 1：客户端不读，灌满会话缓冲（8 KiB）与订阅队列（4 chunk）。
  // 发布量必须显著超过“内核 socket 缓冲（~4.6 KiB）+ 会话缓冲（8 KiB）+
  // 订阅队列（4 chunk）”的总吸收能力（~16.6 KiB），否则队列不溢出。
  // 静默期必须短于写截止时间（500ms）：会话需存活到客户端恢复读取，
  // 走 BACKPRESSURE 断开而非写超时回收。
  const std::string block(1024, 'y');
  std::uint64_t offset = 0;
  for (int i = 0; i < 40; ++i) {
    static_cast<void>(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, block, offset,
                                                      offset + block.size()));
    offset += block.size();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{150});

  // 阶段 2：客户端恢复读取并与发布交替进行。间断的检出需要“被拒绝块
  // 之后的新块”进入队列：先排空旧块，再发布的新块暴露 offset 间断 ->
  // BACKPRESSURE 帧 -> 连接断开（安静流的检出延迟是 M6 计划已记录的
  // 有界语义，此处以交替推进规避）。
  bool saw_backpressure = false;
  for (int round = 0; round < 24 && !saw_backpressure; ++round) {
    static_cast<void>(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, block, offset,
                                                      offset + block.size()));
    offset += block.size();
    // 排空当前可读的帧（含可能的 BACKPRESSURE）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
    while (std::chrono::steady_clock::now() < deadline && !saw_backpressure) {
      if (!frame_readable(connection.client_fd(), 50)) {
        continue;
      }
      if (!read_frame(connection.client_fd(), payload)) {
        break;
      }
      const IpcStreamFrameDecodeResult frame =
          decode_stream_frame_payload(payload.data(), payload.size());
      if (!frame.ok()) {
        break;
      }
      if (frame.value.kind == IpcStreamFrameKind::kLogBackpressure) {
        saw_backpressure = true;
        YORI_CHECK(frame.value.stream == 0);
        YORI_CHECK(frame.value.begin_offset > 0);  // 含当前 offset。
      }
    }
  }
  YORI_CHECK(saw_backpressure);
  YORI_CHECK(connection_closed(connection.client_fd(), 2000));
  for (int i = 0; i < 100; ++i) {
    if (fixture.follow->statistics().active_sessions == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  YORI_CHECK(fixture.follow->statistics().sessions_backpressure >= 1);
}

// M6 专项：--since-offset 回放与 GAP——窗口外起点以 GAP 帧跳到窗口起点。
void test_replay_and_gap() {
  FollowFixture fixture;
  const JobId job = fixture.seed_started_job(9);

  // 灌满窗口（64 KiB，块 32 KiB x4：保留最新两块 [64K, 128K)）。
  const std::string block(std::size_t{32} * 1024, 'g');
  std::uint64_t offset = 0;
  for (int i = 0; i < 4; ++i) {
    YORI_CHECK(fixture.streamer->publish_chunk(job, LogStreamKind::kStdout, block, offset,
                                               offset + block.size()) ==
               LogPublishCode::kPublished);
    offset += block.size();
  }

  ClientConnection connection;
  YORI_CHECK(fixture.begin(connection, fixture.follow_request(9, std::uint64_t{1024})));

  std::vector<std::uint8_t> payload;
  YORI_CHECK(read_frame(connection.client_fd(), payload));
  const IpcResponseDecodeResult ack = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(ack.ok());
  YORI_CHECK(ack.value.logs_follow.stdout_offset == std::uint64_t{64} * 1024);  // GAP 校正后起点。

  // 第二帧：GAP [1024, 64K)。
  YORI_CHECK(read_frame(connection.client_fd(), payload));
  const IpcStreamFrameDecodeResult gap =
      decode_stream_frame_payload(payload.data(), payload.size());
  YORI_CHECK(gap.ok());
  YORI_CHECK(gap.value.kind == IpcStreamFrameKind::kLogGap);
  YORI_CHECK(gap.value.stream == 0);
  YORI_CHECK(gap.value.begin_offset == 1024);
  YORI_CHECK(gap.value.end_offset == std::uint64_t{64} * 1024);

  // 随后是窗口内回放数据块。
  YORI_CHECK(read_frame(connection.client_fd(), payload));
  const IpcStreamFrameDecodeResult replay =
      decode_stream_frame_payload(payload.data(), payload.size());
  YORI_CHECK(replay.ok());
  YORI_CHECK(replay.value.kind == IpcStreamFrameKind::kLogData);
  YORI_CHECK(replay.value.begin_offset == std::uint64_t{64} * 1024);

  // 回放中段断开（测试收尾）：客户端关闭即可，会话由 worker 回收。
  connection.close_client();
}

}  // namespace

int main() {
  test_normal_completion_with_eof();
  test_peer_disconnect_recovery();
  test_admission_rejection();
  test_session_limit_rejection();
  test_cancel_mid_stream();
  test_write_timeout_bounded_recovery();
  test_shutdown_closes_sessions();
  test_slow_client_backpressure();
  test_replay_and_gap();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "log follow: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("log follow: all checks passed\n");
  return 0;
}
