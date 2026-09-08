#include "runtime/gpu_manager.hpp"

#include <executor/executor.hpp>
#include <map>
#include <thread>
#include <utility>

namespace yori::runtime {
namespace {

using StateMap = std::map<gpu::GpuUuid, gpu::GpuObservedState>;

StateMap build_state_map(const gpu::GpuObservationSnapshot& snapshot) {
  StateMap states;
  for (const auto& device : snapshot.devices) {
    states.emplace(device.uuid, device.state);
  }
  return states;
}

// 观测状态迁移 = 设备出现/消失或状态翻转；遥测-only 波动不属于迁移。
std::vector<gpu::GpuUuid> diff_states(const StateMap& previous, const StateMap& current) {
  std::vector<gpu::GpuUuid> changed;
  for (const auto& [uuid, state] : current) {
    const auto previous_state = previous.find(uuid);
    if (previous_state == previous.end() || previous_state->second != state) {
      changed.push_back(uuid);
    }
  }
  for (const auto& entry : previous) {
    if (current.find(entry.first) == current.end()) {
      changed.push_back(entry.first);
    }
  }
  return changed;
}

// tick 串行化守卫：周期回调可能重叠（执行时长超过周期），重叠 tick 被跳过并
// 计数，保证 baseline 与 DoubleBuffer 的写入单线程化。
class TickGuard final {
 public:
  explicit TickGuard(std::atomic<bool>& lock) : lock_(lock), held_(false) {
    bool expected = false;
    held_ = lock_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }
  ~TickGuard() {
    if (held_) {
      lock_.store(false, std::memory_order_release);
    }
  }
  TickGuard(const TickGuard&) = delete;
  TickGuard& operator=(const TickGuard&) = delete;
  [[nodiscard]] bool held() const noexcept { return held_; }

 private:
  std::atomic<bool>& lock_;
  bool held_;
};

enum class SampleCode {
  kOk,
  kFailed,
  kSkipped,
};

}  // namespace

class GpuManager::Impl final : public std::enable_shared_from_this<Impl> {
 public:
  Impl(executor::Executor& executor_ref, gpu::GpuProvider& provider_ref,
       const GpuManagerConfig& config_value)
      : config(config_value),
        executor(executor_ref),
        provider(provider_ref),
        snapshots("yori-gpu-manager-snapshots"),
        events(executor::comm::ChannelOptions{
            config_value.event_capacity == 0 ? std::size_t{1} : config_value.event_capacity,
            executor::comm::DropPolicy::RejectNewest, true, "yori-gpu-manager-events"}) {}

  GpuManagerStartResult start() {
    if (stopped) {
      return {GpuManagerStartCode::kStopped, {}, "gpu manager was stopped and cannot restart"};
    }
    if (started) {
      return {GpuManagerStartCode::kAlreadyStarted, {}, {}};
    }
    if (config.sample_period < std::chrono::milliseconds{1} ||
        config.sample_period > std::chrono::hours{24} || config.event_capacity == 0 ||
        config.stop_join_timeout < std::chrono::milliseconds{0}) {
      return {GpuManagerStartCode::kInvalidConfig, {}, "invalid gpu manager config"};
    }

    // 首次同步观测：失败即拒绝启动（daemon 启动阶段以此决定是否开启调度），
    // 修正环境后可重试。
    if (sample_once() != SampleCode::kOk) {
      return {GpuManagerStartCode::kInitialObservationFailed, last_error(),
              "initial observation failed"};
    }

    auto self = shared_from_this();
    try {
      timer = executor.submit_periodic_with_handle(
          static_cast<std::int64_t>(config.sample_period.count()),
          [self]() noexcept { self->tick(); });
    } catch (...) {
      return {GpuManagerStartCode::kExecutorRejected, {}, "executor rejected the periodic task"};
    }
    started = true;
    return {GpuManagerStartCode::kStarted, {}, {}};
  }

  GpuManagerStopResult stop() {
    if (stopped) {
      return {GpuManagerStopCode::kAlreadyStopped, {}};
    }
    stopped = true;
    stopping.store(true, std::memory_order_release);
    if (!started) {
      return {GpuManagerStopCode::kNotRunning, {}};
    }
    static_cast<void>(timer.cancel());
    started = false;

    // 有界 join：等待持锁的在途采样结束。Executor 定时器状态的
    // active_callback_count 不含已派发回调（实测阻塞中为 0），因此以采样锁为
    // 事实源：持锁者必然正在 sample_once 内，未持锁的后续 tick 会先观察到
    // stopping 并直接返回。
    const auto deadline = std::chrono::steady_clock::now() + config.stop_join_timeout;
    while (tick_lock.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return {GpuManagerStopCode::kJoinTimedOut,
                "in-flight sample tick did not finish within stop_join_timeout"};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return {GpuManagerStopCode::kStopped, {}};
  }

  // tick 入口：sample_once 内部消化全部异常，异常不得逃出周期回调。
  void tick() noexcept { static_cast<void>(sample_once()); }

  bool try_get_snapshot(gpu::GpuObservationSnapshot& out) {
    executor::comm::Snapshot<gpu::GpuObservationSnapshot> snapshot;
    if (!snapshots.try_load(snapshot) || snapshot.value.revision == 0) {
      return false;
    }
    out = std::move(snapshot.value);
    return true;
  }

  bool try_receive_event(GpuManagerEvent& out) { return events.try_receive(out); }

  bool receive_event_for(GpuManagerEvent& out, std::chrono::milliseconds timeout) {
    return events.receive_for(out, timeout).ok;
  }

  GpuManagerStats stats() const {
    GpuManagerStats result;
    result.ticks = ticks.load(std::memory_order_relaxed);
    result.ticks_ok = ticks_ok.load(std::memory_order_relaxed);
    result.ticks_failed = ticks_failed.load(std::memory_order_relaxed);
    result.tick_exceptions = tick_exceptions.load(std::memory_order_relaxed);
    result.snapshots_published = snapshots_published.load(std::memory_order_relaxed);
    result.publish_rejected = publish_rejected.load(std::memory_order_relaxed);
    result.state_change_events = state_change_events.load(std::memory_order_relaxed);
    result.events_dropped = events_dropped.load(std::memory_order_relaxed);
    result.skipped_overlapping_ticks = skipped_overlapping_ticks.load(std::memory_order_relaxed);
    result.error_streak = error_streak.load(std::memory_order_relaxed);
    result.last_error = last_error_.load(std::memory_order_relaxed);
    result.last_good_revision = last_good_revision.load(std::memory_order_relaxed);
    return result;
  }

  GpuManagerConfig config;
  executor::Executor& executor;
  gpu::GpuProvider& provider;
  executor::comm::DoubleBuffer<gpu::GpuObservationSnapshot> snapshots;
  executor::comm::MpscChannel<GpuManagerEvent> events;
  executor::TimerHandle timer;

 private:
  gpu::GpuProviderErrorCode last_error() const noexcept {
    return last_error_.load(std::memory_order_relaxed);
  }

  // 发送事件；背压失败显式计数并置补投标记。
  bool send_event(GpuManagerEvent&& event) {
    if (dropped_events_pending.load(std::memory_order_acquire)) {
      event.dropped_earlier_events = true;
    }
    if (!events.try_send(std::move(event))) {
      events_dropped.fetch_add(1, std::memory_order_relaxed);
      dropped_events_pending.store(true, std::memory_order_release);
      return false;
    }
    dropped_events_pending.store(false, std::memory_order_release);
    return true;
  }

  void record_failure(gpu::GpuProviderErrorCode error) {
    ticks_failed.fetch_add(1, std::memory_order_relaxed);
    last_error_.store(error, std::memory_order_relaxed);
    const auto streak = error_streak.fetch_add(1, std::memory_order_relaxed) + 1;
    if (streak == 1) {
      GpuManagerEvent event;
      event.kind = GpuManagerEventKind::kErrorStreakStarted;
      event.error = error;
      event.error_streak = streak;
      static_cast<void>(send_event(std::move(event)));
    }
  }

  void record_success(std::uint64_t revision) {
    ticks_ok.fetch_add(1, std::memory_order_relaxed);
    last_error_.store(gpu::GpuProviderErrorCode::kNone, std::memory_order_relaxed);
    last_good_revision.store(revision, std::memory_order_relaxed);
    const auto streak = error_streak.exchange(0, std::memory_order_acq_rel);
    if (streak > 0) {
      GpuManagerEvent event;
      event.kind = GpuManagerEventKind::kErrorStreakEnded;
      event.revision = revision;
      event.error_streak = streak;
      static_cast<void>(send_event(std::move(event)));
    }
  }

  // 单次采样：观测 -> 校验 -> 发布 -> 迁移检测。异常全部在此消化。
  SampleCode sample_once() {
    if (stopping.load(std::memory_order_acquire)) {
      return SampleCode::kSkipped;
    }
    const TickGuard guard(tick_lock);
    if (!guard.held()) {
      skipped_overlapping_ticks.fetch_add(1, std::memory_order_relaxed);
      return SampleCode::kSkipped;
    }
    ticks.fetch_add(1, std::memory_order_relaxed);

    gpu::GpuProviderResult observed;
    try {
      observed = provider.observe();
    } catch (...) {
      tick_exceptions.fetch_add(1, std::memory_order_relaxed);
      record_failure(gpu::GpuProviderErrorCode::kObservationFailed);
      return SampleCode::kFailed;
    }
    if (!observed.ok()) {
      record_failure(observed.code);
      return SampleCode::kFailed;
    }
    const auto validation = gpu::validate(observed.snapshot);
    if (!validation.ok()) {
      // Provider 报成功但快照非法：与 provider 错误同级处理，保持上一份有效
      // 快照不被覆盖。
      record_failure(gpu::GpuProviderErrorCode::kObservationFailed);
      return SampleCode::kFailed;
    }

    const auto revision = observed.snapshot.revision;
    if (!snapshots.try_publish(std::move(observed.snapshot))) {
      // 快照未被消费者取走（槽位被钉住）：保留旧 baseline，下个 tick 重发迁移。
      publish_rejected.fetch_add(1, std::memory_order_relaxed);
      return SampleCode::kOk;
    }
    snapshots_published.fetch_add(1, std::memory_order_relaxed);
    record_success(revision);

    // 发布成功后再读取最新快照做差分（DoubleBuffer 语义：状态快照而非消息）。
    executor::comm::Snapshot<gpu::GpuObservationSnapshot> loaded;
    if (!snapshots.try_load(loaded) || loaded.value.revision != revision) {
      // 读取竞态（极端：槽位轮转）：本 tick 放弃迁移检测，baseline 不动。
      return SampleCode::kOk;
    }

    const StateMap next_baseline = build_state_map(loaded.value);
    if (!baseline_established) {
      // 首次成功采样静默建立基线（start 的初始观测）：不产生迁移事件，
      // 消费方以快照直读获得初始状态。
      baseline = std::move(next_baseline);
      baseline_established = true;
      return SampleCode::kOk;
    }
    auto changed = diff_states(baseline, next_baseline);
    if (!changed.empty()) {
      GpuManagerEvent event;
      event.kind = GpuManagerEventKind::kGpuStateChanged;
      event.revision = revision;
      event.changed = std::move(changed);
      if (!send_event(std::move(event))) {
        // 事件被拒：baseline 不前移，下个成功 tick 重新投递累积迁移。
        return SampleCode::kOk;
      }
      state_change_events.fetch_add(1, std::memory_order_relaxed);
    }
    baseline = std::move(next_baseline);
    return SampleCode::kOk;
  }

  bool started{false};
  bool stopped{false};
  bool baseline_established{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> tick_lock{false};
  StateMap baseline;

  std::atomic<std::uint64_t> ticks{0};
  std::atomic<std::uint64_t> ticks_ok{0};
  std::atomic<std::uint64_t> ticks_failed{0};
  std::atomic<std::uint64_t> tick_exceptions{0};
  std::atomic<std::uint64_t> snapshots_published{0};
  std::atomic<std::uint64_t> publish_rejected{0};
  std::atomic<std::uint64_t> state_change_events{0};
  std::atomic<std::uint64_t> events_dropped{0};
  std::atomic<std::uint64_t> skipped_overlapping_ticks{0};
  std::atomic<std::uint64_t> error_streak{0};
  std::atomic<std::uint64_t> last_good_revision{0};
  std::atomic<gpu::GpuProviderErrorCode> last_error_{gpu::GpuProviderErrorCode::kNone};
  std::atomic<bool> dropped_events_pending{false};
};

GpuManager::GpuManager(executor::Executor& executor, gpu::GpuProvider& provider,
                       GpuManagerConfig config)
    : impl_(std::make_shared<Impl>(executor, provider, config)) {}

GpuManager::~GpuManager() {
  // 析构兜底：正常路径 owner 应先显式 stop()（工程规范关闭顺序）。
  static_cast<void>(impl_->stop());
}

GpuManagerStartResult GpuManager::start() { return impl_->start(); }

GpuManagerStopResult GpuManager::stop() { return impl_->stop(); }

bool GpuManager::try_get_snapshot(gpu::GpuObservationSnapshot& out) {
  return impl_->try_get_snapshot(out);
}

bool GpuManager::try_receive_event(GpuManagerEvent& out) { return impl_->try_receive_event(out); }

bool GpuManager::receive_event_for(GpuManagerEvent& out, std::chrono::milliseconds timeout) {
  return impl_->receive_event_for(out, timeout);
}

GpuManagerStats GpuManager::stats() const { return impl_->stats(); }

const char* to_string(GpuManagerEventKind kind) noexcept {
  switch (kind) {
    case GpuManagerEventKind::kGpuStateChanged:
      return "gpu state changed";
    case GpuManagerEventKind::kErrorStreakStarted:
      return "error streak started";
    case GpuManagerEventKind::kErrorStreakEnded:
      return "error streak ended";
  }
  return "unknown";
}

const char* to_string(GpuManagerStartCode code) noexcept {
  switch (code) {
    case GpuManagerStartCode::kStarted:
      return "started";
    case GpuManagerStartCode::kAlreadyStarted:
      return "already started";
    case GpuManagerStartCode::kStopped:
      return "stopped";
    case GpuManagerStartCode::kInvalidConfig:
      return "invalid config";
    case GpuManagerStartCode::kInitialObservationFailed:
      return "initial observation failed";
    case GpuManagerStartCode::kExecutorRejected:
      return "executor rejected";
  }
  return "unknown";
}

const char* to_string(GpuManagerStopCode code) noexcept {
  switch (code) {
    case GpuManagerStopCode::kStopped:
      return "stopped";
    case GpuManagerStopCode::kNotRunning:
      return "not running";
    case GpuManagerStopCode::kAlreadyStopped:
      return "already stopped";
    case GpuManagerStopCode::kJoinTimedOut:
      return "join timed out";
  }
  return "unknown";
}

}  // namespace yori::runtime
