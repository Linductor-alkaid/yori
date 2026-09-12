#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <yori/scheduler/scheduler.hpp>

#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

namespace {

yori::job::JobSpec base_spec(std::chrono::seconds submitted_at, std::uint32_t uid) {
  yori::job::JobSpec spec;
  spec.owner_uid = uid;
  spec.owner_gid = uid;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{submitted_at};
  return spec;
}

yori::store::StoredJob queued(std::uint64_t id, std::chrono::seconds submitted_at,
                              std::uint32_t uid) {
  return {yori::job::JobId{id}, base_spec(submitted_at, uid), yori::job::JobState::kQueued, 0};
}

// DEC-012：kRequired 硬亲和 Job（目标单卡）。
yori::store::StoredJob queued_required(std::uint64_t id, std::chrono::seconds submitted_at,
                                       std::uint32_t uid, const char* target_uuid) {
  yori::store::StoredJob record = queued(id, submitted_at, uid);
  record.spec.gpu_placement.mode = yori::job::GpuPlacementMode::kRequired;
  record.spec.gpu_placement.devices.push_back(yori::gpu::GpuUuid{target_uuid});
  return record;
}

yori::gpu::GpuObservation gpu(const char* uuid, std::uint32_t index,
                              yori::gpu::GpuObservedState state) {
  return {yori::gpu::GpuUuid{uuid}, index, state, {}};
}

yori::gpu::GpuObservationSnapshot snapshot(
    std::initializer_list<yori::gpu::GpuObservation> devices) {
  return {1, std::chrono::system_clock::time_point{std::chrono::seconds{1}}, devices};
}

std::unique_ptr<yori::queue::GlobalJobQueue> make_queue(std::size_t capacity) {
  yori::queue::QueueErrorCode error{};
  auto queue = yori::queue::GlobalJobQueue::create({capacity}, error);
  if (!queue || error != yori::queue::QueueErrorCode::kNone) {
    std::fprintf(stderr, "failed to create queue\n");
    std::exit(1);
  }
  return queue;
}

const yori::store::StoredJob& require_job(const yori::store::StateSnapshot& state,
                                          std::uint64_t id) {
  for (const auto& record : state.jobs) {
    if (record.id == yori::job::JobId{id}) {
      return record;
    }
  }
  std::fprintf(stderr, "required Job %llu is missing\n", static_cast<unsigned long long>(id));
  std::exit(1);
}

yori::queue::QueueEntry require_front(const yori::queue::GlobalJobQueue& queue) {
  const auto front = queue.front();
  if (!front) {
    std::fprintf(stderr, "required queue front is missing\n");
    std::exit(1);
  }
  return *front;
}

// 单场景载体：独立 store/queue，按 FIFO 顺序 admit。
struct Scenario final {
  Scenario(std::size_t jobs_capacity)
      : queue(make_queue(jobs_capacity)), store{{jobs_capacity, jobs_capacity}} {}

  void seed(std::initializer_list<yori::store::StoredJob> jobs) {
    yori::store::StateMutation create;
    create.create_jobs = jobs;
    YORI_CHECK(store.apply(create));
    const auto loaded = store.load();
    YORI_CHECK(loaded);
    YORI_CHECK(queue->restore(loaded.snapshot));
  }

  std::unique_ptr<yori::queue::GlobalJobQueue> queue;
  yori::testing::InMemoryStateStore store;
};

const yori::scheduler::ScheduleSkip* find_skip(
    const yori::scheduler::ScheduleEvaluation& evaluation, std::uint64_t id) {
  for (const auto& skip : evaluation.skipped) {
    if (skip.job == yori::job::JobId{id}) {
      return &skip;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  using yori::gpu::GpuObservedState;
  using yori::job::JobId;
  using yori::job::JobState;
  using yori::scheduler::ScheduleResultCode;
  using yori::scheduler::SchedulerTrigger;
  using yori::scheduler::WaitReason;
  using yori::store::StateStoreErrorCode;

  // ---- ANY 基线（issue #10 场景 1）：FIFO 顺序 + 物理索引升序选择 ----------
  {
    Scenario scenario{4};
    scenario.seed(
        {queued(2, std::chrono::seconds{10}, 1002), queued(1, std::chrono::seconds{10}, 1001)});
    YORI_CHECK(require_front(*scenario.queue).job_id == JobId{1});
    yori::scheduler::FifoScheduler scheduler{*scenario.queue, scenario.store};

    auto invalid_snapshot = snapshot({gpu("GPU-0", 0, GpuObservedState::kFree)});
    invalid_snapshot.revision = 0;
    auto result = scheduler.run_once(SchedulerTrigger::kGpuStateChanged, invalid_snapshot);
    YORI_CHECK(result.code == ScheduleResultCode::kInvalidGpuSnapshot);
    YORI_CHECK(result.failed());
    YORI_CHECK(result.event.gpu_validation.code ==
               yori::gpu::GpuObservationErrorCode::kInvalidRevision);
    YORI_CHECK(scenario.queue->size() == 2);

    const auto blocked_snapshot = snapshot({gpu("GPU-0", 0, GpuObservedState::kExternalBusy),
                                            gpu("GPU-1", 1, GpuObservedState::kUnavailable)});
    result = scheduler.run_once(SchedulerTrigger::kJobSubmitted, blocked_snapshot);
    YORI_CHECK(result.code == ScheduleResultCode::kNoCandidate);
    YORI_CHECK(result.evaluation.skipped.size() == 2);
    // 指针单次绑定：GCC -O3 对"同参重复调用 + 短路解引用"形态有
    // -Wnull-dereference 误报。
    const auto* first_skip = find_skip(result.evaluation, 1);
    const auto* second_skip = find_skip(result.evaluation, 2);
    YORI_CHECK(first_skip != nullptr && first_skip->reason == WaitReason::kNoFreeGpu);
    YORI_CHECK(second_skip != nullptr && second_skip->reason == WaitReason::kNoFreeGpu);
    YORI_CHECK(!result.evaluation.window_truncated);
    YORI_CHECK(scenario.queue->size() == 2);
    auto state = scenario.store.load();
    YORI_CHECK(require_job(state.snapshot, 1).state == JobState::kQueued);

    scenario.store.fail_with(StateStoreErrorCode::kBackendUnavailable);
    result = scheduler.run_once(SchedulerTrigger::kRecoveryCompleted, blocked_snapshot);
    YORI_CHECK(result.code == ScheduleResultCode::kStateLoadFailed);
    YORI_CHECK(result.event.store_error == StateStoreErrorCode::kBackendUnavailable);
    scenario.store.clear_failure();

    const auto free_snapshot = snapshot({gpu("GPU-2", 2, GpuObservedState::kFree),
                                         gpu("GPU-0", 0, GpuObservedState::kFree),
                                         gpu("GPU-1", 1, GpuObservedState::kExternalBusy)});
    scenario.store.fail_next_apply_with(StateStoreErrorCode::kRevisionConflict);
    result = scheduler.run_once(SchedulerTrigger::kJobSubmitted, free_snapshot);
    YORI_CHECK(result.code == ScheduleResultCode::kStateWriteFailed);
    YORI_CHECK(result.event.store_error == StateStoreErrorCode::kRevisionConflict);
    YORI_CHECK(scenario.queue->size() == 2);
    YORI_CHECK(require_front(*scenario.queue).job_id == JobId{1});
    state = scenario.store.load();
    YORI_CHECK(require_job(state.snapshot, 1).state == JobState::kQueued);
    YORI_CHECK(state.snapshot.leases.empty());

    result = scheduler.run_once(SchedulerTrigger::kJobSubmitted, free_snapshot);
    YORI_CHECK(result.scheduled());
    YORI_CHECK(!result.failed());
    YORI_CHECK(result.event.trigger == SchedulerTrigger::kJobSubmitted);
    YORI_CHECK(result.event.job_id == JobId{1});
    YORI_CHECK(result.event.gpu_uuid == yori::gpu::GpuUuid{"GPU-0"});
    YORI_CHECK(result.event.store_revision == 2);
    YORI_CHECK(scenario.queue->size() == 1);
    YORI_CHECK(require_front(*scenario.queue).job_id == JobId{2});
    state = scenario.store.load();
    YORI_CHECK(require_job(state.snapshot, 1).state == JobState::kStarting);
    YORI_CHECK(require_job(state.snapshot, 1).revision == 1);
    YORI_CHECK(state.snapshot.leases.size() == 1);
    YORI_CHECK(state.snapshot.leases[0].gpu_uuid == yori::gpu::GpuUuid{"GPU-0"});

    result = scheduler.run_once(SchedulerTrigger::kGpuStateChanged, free_snapshot);
    YORI_CHECK(result.scheduled());
    YORI_CHECK(result.event.job_id == JobId{2});
    YORI_CHECK(result.event.gpu_uuid == yori::gpu::GpuUuid{"GPU-2"});
    YORI_CHECK(scenario.queue->empty());
    state = scenario.store.load();
    YORI_CHECK(require_job(state.snapshot, 2).state == JobState::kStarting);
    YORI_CHECK(state.snapshot.leases.size() == 2);

    result = scheduler.run_once(SchedulerTrigger::kJobExited, free_snapshot);
    YORI_CHECK(result.code == ScheduleResultCode::kQueueEmpty);
    YORI_CHECK(!result.failed());
  }

  // ---- REQUIRED 空闲时只分配目标卡（issue #10 场景 2）---------------------
  {
    Scenario scenario{2};
    scenario.seed({queued_required(1, std::chrono::seconds{10}, 1001, "GPU-B")});
    yori::scheduler::FifoScheduler scheduler{*scenario.queue, scenario.store};
    // GPU-A 物理索引更小，但亲和目标唯一候选是 GPU-B。
    const auto free_snapshot = snapshot(
        {gpu("GPU-A", 0, GpuObservedState::kFree), gpu("GPU-B", 3, GpuObservedState::kFree)});
    const auto result = scheduler.run_once(SchedulerTrigger::kJobSubmitted, free_snapshot);
    YORI_CHECK(result.scheduled());
    YORI_CHECK(result.event.job_id == JobId{1});
    YORI_CHECK(result.event.gpu_uuid == yori::gpu::GpuUuid{"GPU-B"});
  }

  // ---- REQUIRED 目标被 lease / EXTERNAL_BUSY / UNAVAILABLE / 观测缺失 -----
  {
    Scenario scenario{4};
    scenario.seed({queued_required(1, std::chrono::seconds{1}, 1001, "GPU-A"),
                   queued_required(2, std::chrono::seconds{2}, 1002, "GPU-A"),
                   queued_required(3, std::chrono::seconds{3}, 1003, "GPU-X"),
                   queued_required(4, std::chrono::seconds{4}, 1004, "GPU-MISSING")});
    // J1 先正常调度占用 GPU-A（lease 事实），使 J2 的同目标亲和被 lease 阻塞。
    yori::scheduler::FifoScheduler setup_scheduler{*scenario.queue, scenario.store};
    const auto setup_observation = snapshot({gpu("GPU-A", 0, GpuObservedState::kFree)});
    const auto setup = setup_scheduler.run_once(SchedulerTrigger::kJobSubmitted, setup_observation);
    YORI_CHECK(setup.scheduled() && setup.event.job_id == JobId{1});

    yori::scheduler::FifoScheduler scheduler{*scenario.queue, scenario.store};
    const auto observation = snapshot({gpu("GPU-A", 0, GpuObservedState::kFree),
                                       gpu("GPU-X", 1, GpuObservedState::kExternalBusy),
                                       gpu("GPU-U", 2, GpuObservedState::kUnavailable),
                                       gpu("GPU-FREE", 3, GpuObservedState::kFree)});
    const auto result = scheduler.run_once(SchedulerTrigger::kGpuStateChanged, observation);
    // GPU-FREE 空闲但没有任何 kAny Job，三个 REQUIRED 全部不可满足：不 fallback。
    YORI_CHECK(result.code == ScheduleResultCode::kNoCandidate);
    YORI_CHECK(result.evaluation.skipped.size() == 3);
    const auto* leased_skip = find_skip(result.evaluation, 2);
    YORI_CHECK(leased_skip != nullptr && leased_skip->reason == WaitReason::kAffinityGpuAllocated);
    YORI_CHECK(leased_skip->target == yori::gpu::GpuUuid{"GPU-A"});
    const auto* external_skip = find_skip(result.evaluation, 3);
    const auto* missing_skip = find_skip(result.evaluation, 4);
    YORI_CHECK(external_skip != nullptr &&
               external_skip->reason == WaitReason::kAffinityGpuExternal);
    YORI_CHECK(missing_skip != nullptr &&
               missing_skip->reason == WaitReason::kAffinityGpuState);
    auto state = scenario.store.load();
    YORI_CHECK(require_job(state.snapshot, 2).state == JobState::kQueued);
    YORI_CHECK(require_job(state.snapshot, 3).state == JobState::kQueued);
    YORI_CHECK(require_job(state.snapshot, 4).state == JobState::kQueued);
    YORI_CHECK(state.snapshot.leases.size() == 1);
  }

  // ---- UNAVAILABLE 目标同样保持 QUEUED（场景 5 的显式形态） ---------------
  {
    Scenario scenario{1};
    scenario.seed({queued_required(1, std::chrono::seconds{10}, 1001, "GPU-U")});
    yori::scheduler::FifoScheduler scheduler{*scenario.queue, scenario.store};
    const auto observation = snapshot({gpu("GPU-U", 2, GpuObservedState::kUnavailable)});
    const auto result = scheduler.run_once(SchedulerTrigger::kJobSubmitted, observation);
    YORI_CHECK(result.code == ScheduleResultCode::kNoCandidate);
    const auto* unavailable_skip = find_skip(result.evaluation, 1);
    YORI_CHECK(unavailable_skip != nullptr &&
               unavailable_skip->reason == WaitReason::kAffinityGpuState);
    YORI_CHECK(scenario.queue->size() == 1);
  }

  // ---- 有界跳过：REQUIRED 阻塞不造成全局 HOL（issue #10 场景 8）----------
  {
    Scenario scenario{2};
    scenario.seed({queued_required(1, std::chrono::seconds{10}, 1001, "GPU-0"),
                   queued(2, std::chrono::seconds{20}, 1002)});
    yori::scheduler::FifoScheduler scheduler{*scenario.queue, scenario.store};
    // GPU-0 被 J1 的亲和目标占据（外部占用形态），GPU-1 空闲。
    const auto observation = snapshot({gpu("GPU-0", 0, GpuObservedState::kExternalBusy),
                                       gpu("GPU-1", 1, GpuObservedState::kFree)});
    const auto result = scheduler.run_once(SchedulerTrigger::kJobSubmitted, observation);
    YORI_CHECK(result.scheduled());
    YORI_CHECK(result.event.job_id == JobId{2});
    YORI_CHECK(result.event.gpu_uuid == yori::gpu::GpuUuid{"GPU-1"});
    // J1 保持 QUEUED 与原队列位置，且携带结构化跳过原因。
    const auto* skip = find_skip(result.evaluation, 1);
    YORI_CHECK(skip != nullptr && skip->reason == WaitReason::kAffinityGpuExternal);
    YORI_CHECK(skip->target == yori::gpu::GpuUuid{"GPU-0"});
    YORI_CHECK(scenario.queue->size() == 1);
    YORI_CHECK(require_front(*scenario.queue).job_id == JobId{1});
    const auto state = scenario.store.load();
    YORI_CHECK(require_job(state.snapshot, 1).state == JobState::kQueued);
  }

  // ---- 有界扫描窗口：窗口外的 Job 本轮不被考察，截断显式标记 -------------
  {
    Scenario scenario{5};
    yori::store::StateMutation create;
    create.create_jobs = {queued_required(1, std::chrono::seconds{1}, 1001, "GPU-BUSY"),
                          queued_required(2, std::chrono::seconds{2}, 1002, "GPU-BUSY"),
                          queued(3, std::chrono::seconds{3}, 1003),
                          queued(4, std::chrono::seconds{4}, 1004)};
    YORI_CHECK(scenario.store.apply(create));
    const auto loaded = scenario.store.load();
    YORI_CHECK(scenario.queue->restore(loaded.snapshot));

    // 窗口 = 2：J1/J2（REQUIRED，目标忙）被跳过并考察完毕；J3/J4 不进入本轮。
    yori::scheduler::SchedulerConfig config;
    config.scan_window = 2;
    yori::scheduler::FifoScheduler scheduler{*scenario.queue, scenario.store, config};
    const auto observation = snapshot({gpu("GPU-BUSY", 0, GpuObservedState::kExternalBusy),
                                       gpu("GPU-FREE", 1, GpuObservedState::kFree)});
    const auto result = scheduler.run_once(SchedulerTrigger::kJobSubmitted, observation);
    YORI_CHECK(result.code == ScheduleResultCode::kNoCandidate);
    YORI_CHECK(result.evaluation.skipped.size() == 2);
    YORI_CHECK(result.evaluation.window_truncated);
    YORI_CHECK(find_skip(result.evaluation, 3) == nullptr);
    YORI_CHECK(scenario.queue->size() == 4);
    YORI_CHECK(require_front(*scenario.queue).job_id == JobId{1});

    // 窗口 = 4：J3（kAny）被考察并调度（有界跳过跨过两个 REQUIRED）。
    yori::scheduler::FifoScheduler wide_scheduler{*scenario.queue, scenario.store};
    const auto wide = wide_scheduler.run_once(SchedulerTrigger::kGpuStateChanged, observation);
    YORI_CHECK(wide.scheduled());
    YORI_CHECK(wide.event.job_id == JobId{3});
    YORI_CHECK(wide.evaluation.skipped.size() == 2);
    YORI_CHECK(!wide.evaluation.window_truncated);
  }

  // ---- 队列一致性核对：store 与派生队列分歧显式失败 -----------------------
  {
    auto divergent_queue = make_queue(1);
    YORI_CHECK(divergent_queue->admit(queued(99, std::chrono::seconds{20}, 1099)));
    yori::testing::InMemoryStateStore empty_store{{1, 1}};
    yori::scheduler::FifoScheduler divergent_scheduler{*divergent_queue, empty_store};
    const auto observation = snapshot({gpu("GPU-0", 0, GpuObservedState::kFree)});
    auto result = divergent_scheduler.run_once(SchedulerTrigger::kRecoveryCompleted, observation);
    YORI_CHECK(result.code == ScheduleResultCode::kQueueStateDiverged);
    YORI_CHECK(result.event.queue_error == yori::queue::QueueErrorCode::kJobNotFound);
    YORI_CHECK(divergent_queue->size() == 1);

    auto missing_earlier_queue = make_queue(2);
    yori::testing::InMemoryStateStore missing_earlier_store{{2, 1}};
    yori::store::StateMutation create_missing_earlier;
    create_missing_earlier.create_jobs = {queued(10, std::chrono::seconds{5}, 1010),
                                          queued(20, std::chrono::seconds{10}, 1020)};
    YORI_CHECK(missing_earlier_store.apply(create_missing_earlier));
    YORI_CHECK(missing_earlier_queue->admit(queued(20, std::chrono::seconds{10}, 1020)));
    yori::scheduler::FifoScheduler missing_earlier_scheduler{*missing_earlier_queue,
                                                             missing_earlier_store};
    result = missing_earlier_scheduler.run_once(SchedulerTrigger::kRecoveryCompleted, observation);
    YORI_CHECK(result.code == ScheduleResultCode::kQueueStateDiverged);
    YORI_CHECK(result.event.job_id == JobId{10});
    YORI_CHECK(result.event.queue_error == yori::queue::QueueErrorCode::kJobNotFound);
    YORI_CHECK(missing_earlier_queue->size() == 1);
  }

  const auto cancelled = yori::scheduler::FifoScheduler::cancelled(SchedulerTrigger::kJobCancelled);
  YORI_CHECK(cancelled.code == ScheduleResultCode::kCancelled);
  YORI_CHECK(cancelled.event.trigger == SchedulerTrigger::kJobCancelled);

  constexpr std::array triggers{
      SchedulerTrigger::kJobSubmitted,      SchedulerTrigger::kJobExited,
      SchedulerTrigger::kJobCancelled,      SchedulerTrigger::kGpuStateChanged,
      SchedulerTrigger::kRecoveryCompleted, SchedulerTrigger::kAdminStateChanged,
  };
  for (const auto trigger : triggers) {
    YORI_CHECK(std::string(yori::scheduler::to_string(trigger)) != "UNKNOWN");
  }
  constexpr std::array result_codes{
      ScheduleResultCode::kScheduled,           ScheduleResultCode::kQueueEmpty,
      ScheduleResultCode::kNoCandidate,         ScheduleResultCode::kCancelled,
      ScheduleResultCode::kInvalidGpuSnapshot,  ScheduleResultCode::kStateLoadFailed,
      ScheduleResultCode::kQueueStateDiverged,  ScheduleResultCode::kStateWriteFailed,
      ScheduleResultCode::kQueueRollbackFailed,
  };
  for (const auto code : result_codes) {
    YORI_CHECK(std::string(yori::scheduler::to_string(code)) != "UNKNOWN");
  }
  constexpr std::array wait_reasons{
      WaitReason::kNone,
      WaitReason::kNoFreeGpu,
      WaitReason::kAffinityGpuAllocated,
      WaitReason::kAffinityGpuExternal,
      WaitReason::kAffinityGpuState,
  };
  for (const auto reason : wait_reasons) {
    YORI_CHECK(std::string(yori::scheduler::to_string(reason)) != "UNKNOWN");
  }

  // 扫描窗口配置边界（DEC-012：默认 32，[1, 4096]）。
  constexpr yori::scheduler::SchedulerConfig default_config{};
  YORI_CHECK(default_config.scan_window == 32 && default_config.valid());
  YORI_CHECK(!yori::scheduler::SchedulerConfig{0}.valid());
  YORI_CHECK(!yori::scheduler::SchedulerConfig{4097}.valid());
  YORI_CHECK(yori::scheduler::SchedulerConfig{4096}.valid());

  return yori::testing::failure_count == 0 ? 0 : 1;
}
