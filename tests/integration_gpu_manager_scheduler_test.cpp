// 进程内集成闭环（M3-04）：GpuManager 状态变化事件 -> 以 DoubleBuffer 快照驱动
// FifoScheduler -> lease 建立。外部占用（EXTERNAL_BUSY）阻塞队首；外部进程退出
// 后观测迁移触发调度（EXEC-05/EXEC-06/EXEC-09 的 GPU 侧联动）。Core 的 lease
// 事实与 Provider 观测分列不被破坏（RULE-05）。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "gpu_test_support.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/gpu_manager.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori/scheduler/scheduler.hpp"
#include "yori_test.hpp"

namespace {

using yori::runtime::ExecutorRuntime;
using yori::runtime::ExecutorRuntimeConfig;
using yori::runtime::GpuManager;
using yori::runtime::GpuManagerConfig;
using yori::runtime::GpuManagerEventKind;

yori::store::StoredJob queued_job(std::uint64_t id) {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{id}};
  return {yori::job::JobId{id}, std::move(spec), yori::job::JobState::kQueued, 0};
}

// 单次事件驱动调度：消费一条 GpuManager 事件并以最新快照运行调度器。
yori::scheduler::ScheduleResult schedule_on_event(GpuManager& manager,
                                                  yori::scheduler::FifoScheduler& scheduler) {
  yori::gpu::GpuObservationSnapshot snapshot;
  if (!manager.try_get_snapshot(snapshot)) {
    std::fprintf(stderr, "expected a published gpu snapshot\n");
    std::exit(1);
  }
  return scheduler.run_once(yori::scheduler::SchedulerTrigger::kGpuStateChanged, snapshot);
}

void test_external_busy_unblocks_on_observation_transition() {
  ExecutorRuntime runtime;
  ExecutorRuntimeConfig runtime_config;
  std::string error;
  if (!runtime.initialize(runtime_config, error)) {
    std::fprintf(stderr, "executor initialize failed: %s\n", error.c_str());
    std::exit(1);
  }

  // GPU-0 被外部进程占用：Job 排队且队首阻塞。
  yori::testing::AtomicGpuProvider provider;
  provider.set_uuid(0, "GPU-int-a");
  provider.set_present(0, true);
  provider.set_state(0, yori::gpu::GpuObservedState::kExternalBusy);

  yori::queue::QueueErrorCode queue_error{};
  auto queue = yori::queue::GlobalJobQueue::create({16}, queue_error);
  if (!queue || queue_error != yori::queue::QueueErrorCode::kNone) {
    std::fprintf(stderr, "failed to create queue\n");
    std::exit(1);
  }
  yori::testing::InMemoryStateStore store;
  yori::scheduler::FifoScheduler scheduler(*queue, store);

  yori::store::StateMutation create;
  create.create_jobs.push_back(queued_job(7));
  if (!store.apply(create)) {
    std::fprintf(stderr, "failed to seed store\n");
    std::exit(1);
  }
  const auto state = store.load();
  if (!state || !queue->restore(state.snapshot)) {
    std::fprintf(stderr, "failed to restore queue\n");
    std::exit(1);
  }

  GpuManagerConfig config;
  config.sample_period = std::chrono::milliseconds{30};
  GpuManager manager(runtime.executor(), provider, config);
  const auto start_result = manager.start();
  if (!start_result.ok()) {
    std::fprintf(stderr, "gpu manager start failed: %s\n", start_result.message.c_str());
    std::exit(1);
  }

  // 初始观测即 EXTERNAL_BUSY：手动触发一次调度验证队首阻塞（迁移事件未发生，
  // 该手动触发模拟其他调度触发源）。
  auto blocked = schedule_on_event(manager, scheduler);
  YORI_CHECK(blocked.code == yori::scheduler::ScheduleResultCode::kNoCandidate);

  // 外部进程退出：观测迁移 FREE -> 调度事件 -> lease 建立。
  provider.set_state(0, yori::gpu::GpuObservedState::kFree);
  yori::runtime::GpuManagerEvent event;
  const auto received = manager.receive_event_for(event, std::chrono::milliseconds{5000});
  YORI_CHECK(received);
  YORI_CHECK(event.kind == GpuManagerEventKind::kGpuStateChanged);
  YORI_CHECK(event.changed.size() == 1);
  YORI_CHECK(event.changed[0] == yori::gpu::GpuUuid{"GPU-int-a"});

  const auto scheduled = schedule_on_event(manager, scheduler);
  YORI_CHECK(scheduled.scheduled());
  YORI_CHECK(scheduled.event.job_id == yori::job::JobId{7});
  YORI_CHECK(scheduled.event.gpu_uuid == yori::gpu::GpuUuid{"GPU-int-a"});

  // lease 是 Core 事实：store 中出现 lease，且观测仍为 FREE（未被伪装）。
  const auto after = store.load();
  YORI_CHECK(after.ok());
  YORI_CHECK(after.snapshot.leases.size() == 1);
  YORI_CHECK(after.snapshot.leases[0].gpu_uuid == yori::gpu::GpuUuid{"GPU-int-a"});
  YORI_CHECK(after.snapshot.leases[0].job_id == yori::job::JobId{7});
  YORI_CHECK(after.snapshot.jobs[0].state == yori::job::JobState::kStarting);
  yori::gpu::GpuObservationSnapshot snapshot;
  YORI_CHECK(manager.try_get_snapshot(snapshot));
  YORI_CHECK(snapshot.devices[0].state == yori::gpu::GpuObservedState::kFree);

  // lease 已建立：即使观测仍 FREE，后续调度不再重复分配同一 GPU。
  const auto repeat = schedule_on_event(manager, scheduler);
  YORI_CHECK(repeat.code == yori::scheduler::ScheduleResultCode::kQueueEmpty);

  YORI_CHECK(manager.stop().ok());
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

}  // namespace

int main() {
  test_external_busy_unblocks_on_observation_transition();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "gpu manager scheduler integration failures: %d\n",
                 yori::testing::failure_count);
  }
  return yori::testing::failure_count == 0 ? 0 : 1;
}
