#include "runtime/log_streamer.hpp"

#include <utility>

namespace yori::runtime {
namespace {

std::size_t data_size(const LogChunk& chunk) { return chunk.data.size(); }

std::vector<std::uint8_t> to_bytes(std::string_view text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

// 回看窗口淘汰：从最旧块开始整块淘汰，直到总字节数回到预算内；至少保留
// 最新一块（保证活跃流始终可回放最新数据）。
void trim_backlog(std::deque<LogChunk>& backlog, std::size_t& backlog_bytes, std::size_t budget,
                  std::uint64_t& trims) {
  while (backlog.size() > 1 && backlog_bytes > budget) {
    backlog_bytes -= data_size(backlog.front());
    backlog.pop_front();
    ++trims;
  }
}

}  // namespace

// JobEntry：Topic 按值持有（不可移动），经 shared_ptr 进入注册表，活跃
// 会话与在飞 publish 持有共享所有权。
struct LogStreamer::JobEntry final {
  explicit JobEntry(job::JobId id) : job(id), topic("yori-logs-" + std::to_string(id.value())) {}

  job::JobId job;
  executor::comm::Topic<LogChunk> topic;
  std::atomic<std::uint32_t> sessions{0};
  bool finished{false};
  std::uint8_t job_state{0};
  std::optional<ipc::IpcExitStatus> exit;
  // 每流回看与末 offset；仅经 streamer mutex 访问（publish/subscribe 侧），
  // sessions 为会话归还路径的原子计数。
  struct StreamState final {
    std::deque<LogChunk> backlog;
    std::size_t backlog_bytes{0};
    std::uint64_t offset{0};
    bool seen{false};
  };
  std::array<StreamState, 2> streams{};
};

struct LogStreamer::GlobalCounts final {
  std::atomic<std::uint32_t> total_sessions{0};
};

// 会话令牌：持有 JobEntry 与全局计数的共享所有权；析构归还两级计数并关闭
// 订阅（订阅本身的 close 由 TopicSubscription 析构完成）。
struct LogFollowSession::Token final {
  std::shared_ptr<LogStreamer::JobEntry> job;
  std::shared_ptr<LogStreamer::GlobalCounts> counts;
  executor::comm::TopicSubscription<LogChunk> subscription;
  LogReplayPlan stdout_plan;
  LogReplayPlan stderr_plan;

  ~Token() {
    if (job) {
      job->sessions.fetch_sub(1, std::memory_order_relaxed);
    }
    if (counts) {
      counts->total_sessions.fetch_sub(1, std::memory_order_relaxed);
    }
  }
};

LogFollowSession::~LogFollowSession() {
  if (token_ != nullptr) {
    token_->subscription.close();
  }
}

executor::comm::TopicSubscription<LogChunk>& LogFollowSession::subscription() noexcept {
  return token_->subscription;
}

const LogReplayPlan& LogFollowSession::replay(observe::LogStreamKind stream) const noexcept {
  return stream == observe::LogStreamKind::kStdout ? token_->stdout_plan : token_->stderr_plan;
}

struct LogStreamer::Impl final {
  explicit Impl(LogStreamerConfig config_ref) : config(config_ref) {}

  LogStreamerConfig config;
  mutable std::mutex mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<JobEntry>> jobs;
  std::shared_ptr<GlobalCounts> counts{std::make_shared<GlobalCounts>()};
  // 变更监听（会话 worker 唤醒）：锁内读取副本、锁外调用。
  std::function<void()> change_listener;

  // 统计（publish 侧在锁内更新；会话计数为原子快照）。
  std::uint64_t published_chunks{0};
  std::uint64_t rejected_publishes{0};
  std::uint64_t backlog_trims{0};

  JobEntry::StreamState& stream(JobEntry& entry, observe::LogStreamKind kind) noexcept {
    return entry.streams[kind == observe::LogStreamKind::kStdout ? 0 : 1];
  }

  // 发布并维护回看窗口；调用方持有 mutex 且已确认 job 未 finish。
  LogPublishCode publish_locked(JobEntry& entry, const LogChunk& chunk) {
    JobEntry::StreamState& state = stream(entry, chunk.stream);
    if (chunk.kind == LogChunk::Kind::kData) {
      // 首块建立基线（daemon 重启后由 LogPump 的 sink offset 提供）；此后
      // 数据块与标记块都必须衔接当前末 offset（标记块 begin==end==offset）。
      if (state.seen && chunk.begin_offset != state.offset) {
        return LogPublishCode::kInvalidOffset;
      }
      state.seen = true;
      state.offset = chunk.end_offset;
      state.backlog_bytes += chunk.data.size();
      state.backlog.push_back(chunk);
      trim_backlog(state.backlog, state.backlog_bytes, config.backlog_bytes_per_stream,
                   backlog_trims);
    }
    // 左值发布（拷贝分发）：右值重载经 std::forward 进入队列节点会触发
    // clang-analyzer 对模板实例化的 moved-from 误报路径。
    const auto result = entry.topic.publish(chunk);
    rejected_publishes += result.rejected_subscribers;
    ++published_chunks;
    return LogPublishCode::kPublished;
  }

  // 回放计划：backlog 快照 + GAP 校正（调用方持有 mutex）。
  LogReplayPlan plan_locked(JobEntry& entry, observe::LogStreamKind kind,
                            const std::optional<std::uint64_t>& since) {
    JobEntry::StreamState& state = stream(entry, kind);
    LogReplayPlan plan;
    plan.start_offset = state.offset;
    if (!since || *since >= state.offset) {
      return plan;  // 无 since 从当前末尾；未来 offset 钳制为当前（不伪造）。
    }
    // 找到第一个 end_offset > since 的回放块。
    std::size_t index = 0;
    while (index < state.backlog.size() && state.backlog[index].end_offset <= *since) {
      ++index;
    }
    if (index >= state.backlog.size()) {
      // 窗口为空：整段 [since, offset) 不可回放。
      plan.gap_bytes = state.offset - *since;
      return plan;
    }
    plan.start_offset = *since;
    if (state.backlog[index].begin_offset > *since) {
      // since 落在已淘汰区间：GAP 到窗口内第一块起点。
      plan.gap_bytes = state.backlog[index].begin_offset - *since;
      plan.start_offset = state.backlog[index].begin_offset;
    }
    // 逐块拷贝（GCC 13 -O3 对 deque 区间 assign 的 -Wnull-dereference 误报）。
    plan.chunks.reserve(state.backlog.size() - index);
    for (std::size_t i = index; i < state.backlog.size(); ++i) {
      plan.chunks.push_back(state.backlog[i]);
    }
    return plan;
  }
};

bool LogStreamerConfig::valid(std::string& error) const noexcept {
  if (max_sessions_per_job == 0 || max_sessions_per_job > kMaxSessionsPerJobLimit) {
    error = "max_sessions_per_job must be in [1, 1024]";
    return false;
  }
  if (max_total_sessions == 0 || max_total_sessions > kMaxTotalSessionsLimit) {
    error = "max_total_sessions must be in [1, 4096]";
    return false;
  }
  if (backlog_bytes_per_stream < kMinBacklogBytes || backlog_bytes_per_stream > kMaxBacklogBytes) {
    error = "backlog_bytes_per_stream must be in [64 KiB, 64 MiB]";
    return false;
  }
  if (subscription_capacity < kMinSubscriptionCapacity) {
    error = "subscription_capacity must be at least 4";
    return false;
  }
  return true;
}

LogStreamer::LogStreamer(LogStreamerConfig config) : impl_(std::make_unique<Impl>(config)) {}

LogStreamer::~LogStreamer() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  for (auto& [job_value, entry] : impl_->jobs) {
    static_cast<void>(job_value);
    entry->topic.close();
  }
  impl_->jobs.clear();
}

LogRegisterCode LogStreamer::register_job(job::JobId job, std::string& error) {
  if (!job.valid()) {
    error = "invalid job id";
    return LogRegisterCode::kInvalidJob;
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->jobs.find(job.value()) != impl_->jobs.end()) {
    error = "job already registered";
    return LogRegisterCode::kAlreadyRegistered;
  }
  impl_->jobs.emplace(job.value(), std::make_shared<JobEntry>(job));
  return LogRegisterCode::kRegistered;
}

LogUnregisterCode LogStreamer::unregister_job(job::JobId job) {
  std::function<void()> listener;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto iter = impl_->jobs.find(job.value());
    if (iter == impl_->jobs.end()) {
      return LogUnregisterCode::kNotFound;
    }
    iter->second->topic.close();
    impl_->jobs.erase(iter);
    listener = impl_->change_listener;
  }
  if (listener) {
    listener();  // 订阅者需要被唤醒以观察到 Closed。
  }
  return LogUnregisterCode::kUnregistered;
}

LogPublishCode LogStreamer::publish_chunk(job::JobId job, observe::LogStreamKind stream_kind,
                                          std::string_view data, std::uint64_t begin_offset,
                                          std::uint64_t end_offset) {
  if (data.empty() || end_offset < begin_offset || begin_offset + data.size() != end_offset) {
    return LogPublishCode::kInvalidOffset;
  }
  LogChunk chunk;
  chunk.kind = LogChunk::Kind::kData;
  chunk.stream = stream_kind;
  chunk.begin_offset = begin_offset;
  chunk.end_offset = end_offset;
  chunk.data = to_bytes(data);

  LogPublishCode code = LogPublishCode::kNotRegistered;
  std::function<void()> listener;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto iter = impl_->jobs.find(job.value());
    if (iter == impl_->jobs.end()) {
      return LogPublishCode::kNotRegistered;
    }
    if (iter->second->finished) {
      return LogPublishCode::kJobFinished;
    }
    code = impl_->publish_locked(*iter->second, chunk);
    listener = impl_->change_listener;
  }
  if (listener) {
    listener();  // Topic 无 fd 可 poll：显式唤醒会话 worker。
  }
  return code;
}

LogPublishCode LogStreamer::publish_drop_marker(job::JobId job, observe::LogStreamKind stream_kind,
                                                std::uint64_t offset, std::uint64_t dropped_bytes) {
  // 与 LogSink 落盘标记同格式（设计 11.2）；offset 不前进。
  LogChunk chunk;
  chunk.kind = LogChunk::Kind::kData;
  chunk.stream = stream_kind;
  chunk.begin_offset = offset;
  chunk.end_offset = offset;
  chunk.data = to_bytes(observe::drop_marker_text(dropped_bytes));

  LogPublishCode code = LogPublishCode::kNotRegistered;
  std::function<void()> listener;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto iter = impl_->jobs.find(job.value());
    if (iter == impl_->jobs.end()) {
      return LogPublishCode::kNotRegistered;
    }
    if (iter->second->finished) {
      return LogPublishCode::kJobFinished;
    }
    code = impl_->publish_locked(*iter->second, chunk);
    listener = impl_->change_listener;
  }
  if (listener) {
    listener();
  }
  return code;
}

LogFinishCode LogStreamer::finish_job(job::JobId job, std::uint8_t job_state,
                                      std::optional<ipc::IpcExitStatus> exit) {
  LogChunk chunk;
  chunk.kind = LogChunk::Kind::kEof;
  chunk.job_state = job_state;
  chunk.exit = exit;

  std::function<void()> listener;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto iter = impl_->jobs.find(job.value());
    if (iter == impl_->jobs.end()) {
      return LogFinishCode::kNotFound;
    }
    JobEntry& entry = *iter->second;
    if (entry.finished) {
      return LogFinishCode::kAlreadyFinished;  // 终态幂等：不重复发布 EOF（RULE-04）。
    }
    entry.finished = true;
    entry.job_state = job_state;
    entry.exit = exit;
    const auto result = entry.topic.publish(chunk);
    impl_->rejected_publishes += result.rejected_subscribers;
    entry.topic.close();
    listener = impl_->change_listener;
  }
  if (listener) {
    listener();
  }
  return LogFinishCode::kFinished;
}

void LogStreamer::set_change_listener(std::function<void()> listener) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->change_listener = std::move(listener);
}

LogSubscribeResult LogStreamer::subscribe(job::JobId job, std::optional<std::uint64_t> since_stdout,
                                          std::optional<std::uint64_t> since_stderr) {
  LogSubscribeResult result;
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto iter = impl_->jobs.find(job.value());
  if (iter == impl_->jobs.end()) {
    result.message = "job has no log source";
    return result;
  }
  JobEntry& entry = *iter->second;

  const std::uint32_t job_sessions = entry.sessions.load(std::memory_order_relaxed);
  if (job_sessions >= impl_->config.max_sessions_per_job) {
    result.code = LogSubscribeCode::kSessionLimitJob;
    result.message = "per-job follow session limit reached";
    return result;
  }
  const std::uint32_t total_sessions =
      impl_->counts->total_sessions.load(std::memory_order_relaxed);
  if (total_sessions >= impl_->config.max_total_sessions) {
    result.code = LogSubscribeCode::kSessionLimitTotal;
    result.message = "global follow session limit reached";
    return result;
  }

  // 先建订阅（开始缓冲后续发布），再取回放快照——两步同锁内完成，回放与
  // 直播的交界由会话侧按 offset 去重。
  executor::comm::TopicSubscriptionOptions options;
  options.capacity = impl_->config.subscription_capacity;
  options.drop_policy = executor::comm::DropPolicy::RejectNewest;
  options.enable_stats = true;
  options.name = "yori-follow-" + std::to_string(job.value());

  auto token = std::make_shared<LogFollowSession::Token>();
  token->job = iter->second;
  token->counts = impl_->counts;
  token->subscription = entry.topic.subscribe(options);
  token->stdout_plan = impl_->plan_locked(entry, observe::LogStreamKind::kStdout, since_stdout);
  token->stderr_plan = impl_->plan_locked(entry, observe::LogStreamKind::kStderr, since_stderr);

  entry.sessions.fetch_add(1, std::memory_order_relaxed);
  impl_->counts->total_sessions.fetch_add(1, std::memory_order_relaxed);

  result.code = LogSubscribeCode::kSubscribed;
  result.finished = entry.finished;
  result.job_state = entry.job_state;
  result.exit = entry.exit;
  result.session.token_ = std::move(token);
  return result;
}

LogStreamerStatistics LogStreamer::statistics() const {
  LogStreamerStatistics statistics;
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  statistics.published_chunks = impl_->published_chunks;
  statistics.rejected_publishes = impl_->rejected_publishes;
  statistics.backlog_trims = impl_->backlog_trims;
  statistics.registered_jobs = impl_->jobs.size();
  statistics.active_sessions = impl_->counts->total_sessions.load(std::memory_order_relaxed);
  return statistics;
}

}  // namespace yori::runtime
