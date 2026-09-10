#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <executor/comm/topic.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <yori/ipc/ipc_protocol.hpp>
#include <yori/job/job.hpp>
#include <yori/observe/log_sink.hpp>

namespace yori::runtime {

// ---------------------------------------------------------------------------
// LogStreamer（EXEC-04，设计 11.2-11.4）：日志块的订阅分发与跟随会话准入。
//
// 每 Job 一个 executor::comm::Topic<LogChunk>（RejectNewest，每订阅者有界
// 队列即设计 11.4 的发送缓冲）；每流一个有界内存回看窗口承载
// --since-offset 重连回放，窗口淘汰的旧区间以 GAP 显式告知（不伪造、不
// 静默丢弃）。数据面（publish_*）由 LogPump worker 单线程调用；会话面
// （subscribe）由 IPC 网关调用；内部以 mutex 保护 Job 注册表，块分发经
// Topic 自身同步——mutex 是生命周期所有权，不是通信通道的替代。
//
// owner 纪律：随 daemon 主生命周期构造与销毁；停止路径 = unregister 全部
// Job（关闭 Topic，订阅者排空后收到 Closed）。
// ---------------------------------------------------------------------------

// Topic 载荷。kData 携带 [begin_offset, end_offset) 的落盘已接受字节；丢弃
// 标记 chunk 为 begin==end 且 data 非空（offset 不前进，与 LogSink 落盘
// 标记同格式文本）；kEof 为终态控制事件，发布后 Topic 关闭。
struct LogChunk final {
  enum class Kind : std::uint8_t { kData, kEof } kind{Kind::kData};
  observe::LogStreamKind stream{observe::LogStreamKind::kStdout};
  std::uint64_t begin_offset{0};
  std::uint64_t end_offset{0};
  std::vector<std::uint8_t> data;
  // 仅 kEof：
  std::uint8_t job_state{0};
  std::optional<ipc::IpcExitStatus> exit;
};

struct LogStreamerConfig final {
  static constexpr std::uint32_t kMaxSessionsPerJobLimit = 1024;
  static constexpr std::uint32_t kMaxTotalSessionsLimit = 4096;
  static constexpr std::size_t kMinBacklogBytes = 64 * 1024;
  static constexpr std::size_t kMaxBacklogBytes = 64ull << 20;
  static constexpr std::size_t kDefaultBacklogBytes = 8ull << 20;
  static constexpr std::size_t kMinSubscriptionCapacity = 4;

  // 设计 11.4：每 Job 跟随会话上限（默认 8）与全局上限（默认 64）。
  std::uint32_t max_sessions_per_job{8};
  std::uint32_t max_total_sessions{64};
  // --since-offset 重连回看窗口（每流字节预算）。
  std::size_t backlog_bytes_per_stream{kDefaultBacklogBytes};
  // 每订阅者 Topic 队列容量（chunk 数）。
  std::size_t subscription_capacity{64};

  [[nodiscard]] bool valid(std::string& error) const noexcept;
};

enum class LogRegisterCode {
  kRegistered,
  kAlreadyRegistered,
  kInvalidJob,
};

enum class LogUnregisterCode {
  kUnregistered,
  kNotFound,
};

enum class LogPublishCode {
  kPublished,
  kNotRegistered,
  kJobFinished,
  kInvalidOffset,
};

enum class LogFinishCode {
  kFinished,
  kNotFound,
  kAlreadyFinished,
};

enum class LogSubscribeCode {
  kSubscribed,
  kJobUnknown,
  kSessionLimitJob,
  kSessionLimitTotal,
};

// 单流的回放计划：GAP 校正后的起点、跳过的字节数与回放块快照。
struct LogReplayPlan final {
  // 实际起点（since 校正后；无 since 时为订阅时刻的流末 offset）。
  std::uint64_t start_offset{0};
  // since 早于回看窗口起点时为 (start_offset - since)，否则 0。
  std::uint64_t gap_bytes{0};
  std::vector<LogChunk> chunks;
};

// 跟随会话句柄：RAII 持有订阅与会话计数；析构关闭订阅并归还计数。
// 会话 worker 独占持有（移动所有权）。
class LogFollowSession final {
 public:
  LogFollowSession() = default;
  LogFollowSession(LogFollowSession&&) noexcept = default;
  LogFollowSession& operator=(LogFollowSession&&) noexcept = default;
  ~LogFollowSession();

  [[nodiscard]] bool valid() const noexcept { return token_ != nullptr; }
  [[nodiscard]] executor::comm::TopicSubscription<LogChunk>& subscription() noexcept;
  [[nodiscard]] const LogReplayPlan& replay(observe::LogStreamKind stream) const noexcept;

 private:
  friend class LogStreamer;
  struct Token;
  std::shared_ptr<Token> token_;
};

struct LogSubscribeResult final {
  LogSubscribeCode code{LogSubscribeCode::kJobUnknown};
  std::string message;
  // code == kSubscribed 时有效。
  LogFollowSession session;
  // 订阅时刻的 Job 状态（终态时 finished=true，会话在回放后应以 EOF 收尾）。
  bool finished{false};
  std::uint8_t job_state{0};
  std::optional<ipc::IpcExitStatus> exit;
};

struct LogStreamerStatistics final {
  std::uint64_t published_chunks{0};
  std::uint64_t rejected_publishes{0};  // Topic 拒绝的订阅者投递累计（慢客户端信号）
  std::uint64_t backlog_trims{0};       // 回看窗口淘汰次数（GAP 来源）
  std::uint64_t active_sessions{0};
  std::uint64_t registered_jobs{0};
};

class LogStreamer final {
 public:
  explicit LogStreamer(LogStreamerConfig config = {});
  ~LogStreamer();

  LogStreamer(const LogStreamer&) = delete;
  LogStreamer& operator=(const LogStreamer&) = delete;
  LogStreamer(LogStreamer&&) = delete;
  LogStreamer& operator=(LogStreamer&&) = delete;

  // 注册/注销一个 Job 的日志通道（守护建立/回收日志源时调用）。
  [[nodiscard]] LogRegisterCode register_job(job::JobId job, std::string& error);
  [[nodiscard]] LogUnregisterCode unregister_job(job::JobId job);

  // 数据面（LogPump worker）：发布一块已落盘接受的字节。begin_offset 必须
  // 等于该流当前末 offset（标记 chunk 例外：begin == end）。
  [[nodiscard]] LogPublishCode publish_chunk(job::JobId job, observe::LogStreamKind stream,
                                             std::string_view data, std::uint64_t begin_offset,
                                             std::uint64_t end_offset);
  // 发布丢弃标记（LogSink 写失败丢弃后；offset 不前进）。
  [[nodiscard]] LogPublishCode publish_drop_marker(job::JobId job, observe::LogStreamKind stream,
                                                   std::uint64_t offset,
                                                   std::uint64_t dropped_bytes);

  // Job 终态：发布 kEof chunk 并关闭 Topic；重复调用幂等失败。
  [[nodiscard]] LogFinishCode finish_job(job::JobId job, std::uint8_t job_state,
                                         std::optional<ipc::IpcExitStatus> exit);

  // 会话面（IPC 网关）：准入检查 + 订阅 + 回放快照（原子于订阅创建）。
  [[nodiscard]] LogSubscribeResult subscribe(job::JobId job,
                                             std::optional<std::uint64_t> since_stdout,
                                             std::optional<std::uint64_t> since_stderr);

  // 变更通知（M6 会话承载）：每次发布/终态后回调，供会话 worker 的唤醒
  // 管道触发（Topic 无 fd 可 poll）。必须在并发使用前设置一次；回调自身
  // 必须非阻塞、不抛出、可从发布线程调用；清空以解除挂钩。
  void set_change_listener(std::function<void()> listener);

  [[nodiscard]] LogStreamerStatistics statistics() const;

 private:
  struct JobEntry;
  struct GlobalCounts;

  // 会话令牌（LogFollowSession::Token）需访问 JobEntry 的会话计数与全局
  // 计数（嵌套类具有与外围类一致的访问权，C++11 [class.access.nest]）。
  friend class LogFollowSession;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
