// GpuManager 单测（M3-02/M3-04，EXEC-05/EXEC-09 GPU 快照部分）：周期采样、
// DoubleBuffer 快照发布、观测状态迁移事件、错误 streak、背压补投与停止路径。
// 六场景映射见 docs/plans/m3-nvml-gpu-integration.md。

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gpu_test_support.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/gpu_manager.hpp"
#include "yori_test.hpp"

namespace {

using yori::runtime::ExecutorRuntime;
using yori::runtime::ExecutorRuntimeConfig;
using yori::runtime::GpuManager;
using yori::runtime::GpuManagerConfig;
using yori::runtime::GpuManagerEvent;
using yori::runtime::GpuManagerEventKind;
using yori::runtime::GpuManagerStartCode;
using yori::runtime::GpuManagerStopCode;
using yori::testing::wait_for;

constexpr std::chrono::milliseconds kPeriod{30};
constexpr std::chrono::milliseconds kWait{5000};

bool initialize_executor(ExecutorRuntime& runtime) {
  ExecutorRuntimeConfig config;
  std::string error;
  return runtime.initialize(config, error);
}

void configure_two_devices(yori::testing::AtomicGpuProvider& provider) {
  provider.set_uuid(0, "GPU-m3-a");
  provider.set_uuid(1, "GPU-m3-b");
  provider.set_present(0, true);
  provider.set_present(1, true);
  provider.set_state(0, yori::gpu::GpuObservedState::kFree);
  provider.set_state(1, yori::gpu::GpuObservedState::kExternalBusy);
  provider.set_utilization(0, 3);
  provider.set_memory(0, 1000, 100);
}

void test_start_publishes_initial_snapshot() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  GpuManager manager(runtime.executor(), provider, config);

  yori::gpu::GpuObservationSnapshot empty;
  YORI_CHECK(!manager.try_get_snapshot(empty));
  YORI_CHECK(manager.start().ok());

  yori::gpu::GpuObservationSnapshot snapshot;
  YORI_CHECK(manager.try_get_snapshot(snapshot));
  YORI_CHECK(snapshot.revision == 1);
  YORI_CHECK(snapshot.devices.size() == 2);
  YORI_CHECK(snapshot.devices[1].state == yori::gpu::GpuObservedState::kExternalBusy);

  // 基线即首次观测：没有迁移，也没有 streak 事件。
  GpuManagerEvent event;
  YORI_CHECK(!manager.try_receive_event(event));

  const auto stats = manager.stats();
  YORI_CHECK(stats.ticks == 1);
  YORI_CHECK(stats.ticks_ok == 1);
  YORI_CHECK(stats.ticks_failed == 0);
  YORI_CHECK(stats.snapshots_published == 1);

  // 重复 start 显式拒绝。
  YORI_CHECK(manager.start().code == GpuManagerStartCode::kAlreadyStarted);

  YORI_CHECK(manager.stop().code == GpuManagerStopCode::kStopped);
  YORI_CHECK(manager.stop().code == GpuManagerStopCode::kAlreadyStopped);
  YORI_CHECK(manager.start().code == GpuManagerStartCode::kStopped);
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_periodic_ticks_and_telemetry_only_change_is_silent() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  GpuManager manager(runtime.executor(), provider, config);
  YORI_CHECK(manager.start().ok());

  // 只改遥测（RULE-05：遥测不是 ownership，不触发调度事件）。
  provider.set_utilization(0, 99);
  provider.set_memory(0, 1000, 900);
  YORI_CHECK(wait_for(
      [&manager] {
        yori::gpu::GpuObservationSnapshot snapshot;
        return manager.try_get_snapshot(snapshot) &&
               snapshot.devices[0].telemetry.utilization_percent == std::uint32_t{99};
      },
      kWait));

  GpuManagerEvent event;
  YORI_CHECK(!manager.try_receive_event(event));
  const auto stats = manager.stats();
  YORI_CHECK(stats.ticks > 1);
  YORI_CHECK(stats.ticks_failed == 0);
  YORI_CHECK(stats.state_change_events == 0);

  YORI_CHECK(manager.stop().code == GpuManagerStopCode::kStopped);
  // stop 后不再产生 tick。
  const auto ticks_at_stop = manager.stats().ticks;
  std::this_thread::sleep_for(kPeriod * 4);
  YORI_CHECK(manager.stats().ticks == ticks_at_stop);
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_state_transition_and_disappearance_events() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  GpuManager manager(runtime.executor(), provider, config);
  YORI_CHECK(manager.start().ok());

  // 状态翻转：FREE -> UNAVAILABLE。
  provider.set_state(0, yori::gpu::GpuObservedState::kUnavailable);
  GpuManagerEvent event;
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.kind == GpuManagerEventKind::kGpuStateChanged);
  YORI_CHECK(!event.dropped_earlier_events);
  YORI_CHECK(event.changed.size() == 1);
  YORI_CHECK(event.changed[0] == yori::gpu::GpuUuid{"GPU-m3-a"});

  // 设备消失：事件携带该 UUID。
  provider.set_present(1, false);
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.kind == GpuManagerEventKind::kGpuStateChanged);
  YORI_CHECK(event.changed.size() == 1);
  YORI_CHECK(event.changed[0] == yori::gpu::GpuUuid{"GPU-m3-b"});

  // 外部占用消失：EXTERNAL_BUSY -> FREE（调度触发主路径）。
  provider.set_present(1, true);
  provider.set_state(1, yori::gpu::GpuObservedState::kFree);
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.changed.size() == 1);
  YORI_CHECK(event.changed[0] == yori::gpu::GpuUuid{"GPU-m3-b"});

  yori::gpu::GpuObservationSnapshot snapshot;
  YORI_CHECK(manager.try_get_snapshot(snapshot));
  YORI_CHECK(snapshot.devices.size() == 2);
  YORI_CHECK(snapshot.devices[1].state == yori::gpu::GpuObservedState::kFree);
  YORI_CHECK(manager.stats().state_change_events == 3);

  YORI_CHECK(manager.stop().code == GpuManagerStopCode::kStopped);
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_provider_error_streak_and_recovery() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  GpuManager manager(runtime.executor(), provider, config);
  YORI_CHECK(manager.start().ok());
  yori::gpu::GpuObservationSnapshot good;
  YORI_CHECK(manager.try_get_snapshot(good));
  const auto good_revision = good.revision;

  // provider 错误：streak 开始事件 + 保持上一份有效快照。
  provider.set_failure(yori::gpu::GpuProviderErrorCode::kBackendUnavailable);
  GpuManagerEvent event;
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.kind == GpuManagerEventKind::kErrorStreakStarted);
  YORI_CHECK(event.error == yori::gpu::GpuProviderErrorCode::kBackendUnavailable);
  YORI_CHECK(wait_for([&manager] { return manager.stats().error_streak >= 2; }, kWait));
  YORI_CHECK(manager.stats().last_error == yori::gpu::GpuProviderErrorCode::kBackendUnavailable);
  yori::gpu::GpuObservationSnapshot still_good;
  YORI_CHECK(manager.try_get_snapshot(still_good));
  YORI_CHECK(still_good.revision == good_revision);

  // observe() 抛异常：任务异常路径，同样进入失败统计。
  provider.set_failure(std::nullopt);
  provider.set_throwing(true);
  YORI_CHECK(wait_for([&manager] { return manager.stats().tick_exceptions >= 1; }, kWait));

  // 恢复：streak 结束事件。
  provider.set_throwing(false);
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.kind == GpuManagerEventKind::kErrorStreakEnded);
  YORI_CHECK(event.error_streak >= 3);
  YORI_CHECK(wait_for([&manager] { return manager.stats().error_streak == 0; }, kWait));
  YORI_CHECK(manager.stats().ticks_failed >= 3);
  YORI_CHECK(manager.stats().last_error == yori::gpu::GpuProviderErrorCode::kNone);

  YORI_CHECK(manager.stop().code == GpuManagerStopCode::kStopped);
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_invalid_snapshot_rejected() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  GpuManager manager(runtime.executor(), provider, config);
  YORI_CHECK(manager.start().ok());

  // utilization > 100 产出非法快照：不发布、不覆盖上一份有效快照、计入失败。
  provider.set_utilization(0, 200);
  GpuManagerEvent event;
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.kind == GpuManagerEventKind::kErrorStreakStarted);
  YORI_CHECK(manager.stats().ticks_failed >= 1);
  yori::gpu::GpuObservationSnapshot snapshot;
  YORI_CHECK(manager.try_get_snapshot(snapshot));
  YORI_CHECK(snapshot.devices[0].telemetry.utilization_percent == std::uint32_t{3});

  YORI_CHECK(manager.stop().code == GpuManagerStopCode::kStopped);
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_event_backpressure_and_catchup() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  config.event_capacity = 1;
  GpuManager manager(runtime.executor(), provider, config);
  YORI_CHECK(manager.start().ok());

  // 第一条迁移事件占据唯一容量。
  provider.set_state(0, yori::gpu::GpuObservedState::kUnavailable);
  YORI_CHECK(wait_for([&manager] { return manager.stats().state_change_events == 1; }, kWait));

  // 第二条迁移被拒（显式计数），baseline 不前移。
  provider.set_state(1, yori::gpu::GpuObservedState::kFree);
  YORI_CHECK(wait_for([&manager] { return manager.stats().events_dropped >= 1; }, kWait));

  // 消费第一条后，累积迁移以补投标记重新投递。
  GpuManagerEvent event;
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.changed.size() == 1);
  YORI_CHECK(event.changed[0] == yori::gpu::GpuUuid{"GPU-m3-a"});
  YORI_CHECK(manager.receive_event_for(event, kWait));
  YORI_CHECK(event.dropped_earlier_events);
  YORI_CHECK(event.changed.size() == 1);
  YORI_CHECK(event.changed[0] == yori::gpu::GpuUuid{"GPU-m3-b"});

  YORI_CHECK(manager.stop().code == GpuManagerStopCode::kStopped);
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_join_timeout_and_inflight_completion() {
  ExecutorRuntime runtime;
  ExecutorRuntimeConfig runtime_config;
  // 两个 worker：tick2 阻塞在采样内时，后续 tick 才有机会进入并被
  // TickGuard 跳过（单 worker 时它们只会在队列中等待）。
  runtime_config.min_threads = 2;
  runtime_config.max_threads = 2;
  std::string error;
  YORI_CHECK(runtime.initialize(runtime_config, error));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  yori::testing::HoldingGpuProvider holding(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  config.stop_join_timeout = std::chrono::milliseconds{80};
  GpuManager manager(runtime.executor(), holding, config);
  YORI_CHECK(manager.start().ok());

  // 阻塞下一次 observe：tick 卡在采样内，后续重叠 tick 被跳过。
  holding.hold_next_observe();
  YORI_CHECK(wait_for([&manager] { return manager.stats().ticks >= 2; }, kWait));
  YORI_CHECK(
      wait_for([&manager] { return manager.stats().skipped_overlapping_ticks >= 1; }, kWait));

  const auto stop_result = manager.stop();
  YORI_CHECK(stop_result.code == GpuManagerStopCode::kJoinTimedOut);
  const auto published_at_timeout = manager.stats().snapshots_published;

  // 释放在途 tick：其生命周期由回调持有的 shared_ptr 延长，完成后正常发布。
  holding.release();
  YORI_CHECK(wait_for(
      [&manager, published_at_timeout] {
        return manager.stats().snapshots_published > published_at_timeout;
      },
      kWait));

  // manager 析构（stop 已完成，不再 join），provider 之后销毁（owner 纪律）。
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_start_rejection_paths() {
  // 非法配置：周期为 0。
  {
    ExecutorRuntime runtime;
    YORI_CHECK(initialize_executor(runtime));
    yori::testing::AtomicGpuProvider provider;
    configure_two_devices(provider);
    GpuManagerConfig config;
    config.sample_period = std::chrono::milliseconds{0};
    GpuManager manager(runtime.executor(), provider, config);
    const auto result = manager.start();
    YORI_CHECK(result.code == GpuManagerStartCode::kInvalidConfig);
    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // 初始观测失败：显式拒绝且可重试。
  {
    ExecutorRuntime runtime;
    YORI_CHECK(initialize_executor(runtime));
    yori::testing::AtomicGpuProvider provider;
    configure_two_devices(provider);
    provider.set_failure(yori::gpu::GpuProviderErrorCode::kPermissionDenied);
    GpuManagerConfig config;
    config.sample_period = kPeriod;
    GpuManager manager(runtime.executor(), provider, config);
    const auto result = manager.start();
    YORI_CHECK(result.code == GpuManagerStartCode::kInitialObservationFailed);
    YORI_CHECK(result.provider_error == yori::gpu::GpuProviderErrorCode::kPermissionDenied);
    provider.set_failure(std::nullopt);
    YORI_CHECK(manager.start().ok());
    YORI_CHECK(manager.stop().code == GpuManagerStopCode::kStopped);
    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // stop 于 start 之前：kNotRunning，此后不可再启动。
  {
    ExecutorRuntime runtime;
    YORI_CHECK(initialize_executor(runtime));
    yori::testing::AtomicGpuProvider provider;
    configure_two_devices(provider);
    GpuManagerConfig config;
    config.sample_period = kPeriod;
    GpuManager manager(runtime.executor(), provider, config);
    YORI_CHECK(manager.stop().code == GpuManagerStopCode::kNotRunning);
    YORI_CHECK(manager.start().code == GpuManagerStartCode::kStopped);
    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }
}

void test_shutdown_then_stop_degraded_path() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  GpuManager manager(runtime.executor(), provider, config);
  YORI_CHECK(manager.start().ok());

  // 先 shutdown（Executor 停止）再 stop/析构：不挂起、不崩溃（降级路径）。
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  const auto stop_result = manager.stop();
  YORI_CHECK(stop_result.ok());
}

void test_shutdown_then_start_rejected() {
  ExecutorRuntime runtime;
  YORI_CHECK(initialize_executor(runtime));
  yori::testing::AtomicGpuProvider provider;
  configure_two_devices(provider);
  GpuManagerConfig config;
  config.sample_period = kPeriod;
  GpuManager manager(runtime.executor(), provider, config);
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  const auto result = manager.start();
  YORI_CHECK(result.code == GpuManagerStartCode::kExecutorRejected);
}

}  // namespace

int main() {
  test_start_publishes_initial_snapshot();
  test_periodic_ticks_and_telemetry_only_change_is_silent();
  test_state_transition_and_disappearance_events();
  test_provider_error_streak_and_recovery();
  test_invalid_snapshot_rejected();
  test_event_backpressure_and_catchup();
  test_join_timeout_and_inflight_completion();
  test_start_rejection_paths();
  test_shutdown_then_stop_degraded_path();
  test_shutdown_then_start_rejected();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "gpu manager test failures: %d\n", yori::testing::failure_count);
  }
  return yori::testing::failure_count == 0 ? 0 : 1;
}
