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

yori::store::StoredJob queued(std::uint64_t id, std::chrono::seconds submitted_at,
                              std::uint32_t uid) {
  yori::job::JobSpec spec;
  spec.owner_uid = uid;
  spec.owner_gid = uid;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{submitted_at};
  return {yori::job::JobId{id}, std::move(spec), yori::job::JobState::kQueued, 0};
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

}  // namespace

int main() {
  using yori::gpu::GpuObservedState;
  using yori::job::JobId;
  using yori::job::JobState;
  using yori::scheduler::ScheduleResultCode;
  using yori::scheduler::SchedulerTrigger;
  using yori::store::StateStoreErrorCode;

  auto queue = make_queue(4);
  yori::testing::InMemoryStateStore store{{4, 2}};
  yori::store::StateMutation create;
  create.create_jobs = {queued(2, std::chrono::seconds{10}, 1002),
                        queued(1, std::chrono::seconds{10}, 1001)};
  YORI_CHECK(store.apply(create));
  auto state = store.load();
  YORI_CHECK(state);
  YORI_CHECK(queue->restore(state.snapshot));
  YORI_CHECK(require_front(*queue).job_id == JobId{1});

  yori::scheduler::FifoScheduler scheduler{*queue, store};

  auto invalid_snapshot = snapshot({gpu("GPU-0", 0, GpuObservedState::kFree)});
  invalid_snapshot.revision = 0;
  auto scheduled = scheduler.run_once(SchedulerTrigger::kGpuStateChanged, invalid_snapshot);
  YORI_CHECK(scheduled.code == ScheduleResultCode::kInvalidGpuSnapshot);
  YORI_CHECK(scheduled.failed());
  YORI_CHECK(scheduled.event.gpu_validation.code ==
             yori::gpu::GpuObservationErrorCode::kInvalidRevision);
  YORI_CHECK(queue->size() == 2);

  const auto blocked_snapshot = snapshot({gpu("GPU-0", 0, GpuObservedState::kExternalBusy),
                                          gpu("GPU-1", 1, GpuObservedState::kUnavailable)});
  scheduled = scheduler.run_once(SchedulerTrigger::kJobSubmitted, blocked_snapshot);
  YORI_CHECK(scheduled.code == ScheduleResultCode::kHeadBlocked);
  YORI_CHECK(scheduled.event.job_id == JobId{1});
  YORI_CHECK(queue->size() == 2);

  store.fail_with(StateStoreErrorCode::kBackendUnavailable);
  scheduled = scheduler.run_once(SchedulerTrigger::kRecoveryCompleted, blocked_snapshot);
  YORI_CHECK(scheduled.code == ScheduleResultCode::kStateLoadFailed);
  YORI_CHECK(scheduled.event.store_error == StateStoreErrorCode::kBackendUnavailable);
  store.clear_failure();

  const auto free_snapshot =
      snapshot({gpu("GPU-2", 2, GpuObservedState::kFree), gpu("GPU-0", 0, GpuObservedState::kFree),
                gpu("GPU-1", 1, GpuObservedState::kExternalBusy)});
  store.fail_next_apply_with(StateStoreErrorCode::kRevisionConflict);
  scheduled = scheduler.run_once(SchedulerTrigger::kJobSubmitted, free_snapshot);
  YORI_CHECK(scheduled.code == ScheduleResultCode::kStateWriteFailed);
  YORI_CHECK(scheduled.event.store_error == StateStoreErrorCode::kRevisionConflict);
  YORI_CHECK(queue->size() == 2);
  YORI_CHECK(require_front(*queue).job_id == JobId{1});
  state = store.load();
  YORI_CHECK(require_job(state.snapshot, 1).state == JobState::kQueued);
  YORI_CHECK(state.snapshot.leases.empty());

  scheduled = scheduler.run_once(SchedulerTrigger::kJobSubmitted, free_snapshot);
  YORI_CHECK(scheduled.scheduled());
  YORI_CHECK(!scheduled.failed());
  YORI_CHECK(scheduled.event.trigger == SchedulerTrigger::kJobSubmitted);
  YORI_CHECK(scheduled.event.job_id == JobId{1});
  YORI_CHECK(scheduled.event.gpu_uuid == yori::gpu::GpuUuid{"GPU-0"});
  YORI_CHECK(scheduled.event.store_revision == 2);
  YORI_CHECK(queue->size() == 1);
  YORI_CHECK(require_front(*queue).job_id == JobId{2});
  state = store.load();
  YORI_CHECK(require_job(state.snapshot, 1).state == JobState::kStarting);
  YORI_CHECK(require_job(state.snapshot, 1).revision == 1);
  YORI_CHECK(state.snapshot.leases.size() == 1);
  YORI_CHECK(state.snapshot.leases[0].gpu_uuid == yori::gpu::GpuUuid{"GPU-0"});

  scheduled = scheduler.run_once(SchedulerTrigger::kGpuStateChanged, free_snapshot);
  YORI_CHECK(scheduled.scheduled());
  YORI_CHECK(scheduled.event.job_id == JobId{2});
  YORI_CHECK(scheduled.event.gpu_uuid == yori::gpu::GpuUuid{"GPU-2"});
  YORI_CHECK(queue->empty());
  state = store.load();
  YORI_CHECK(require_job(state.snapshot, 2).state == JobState::kStarting);
  YORI_CHECK(state.snapshot.leases.size() == 2);

  scheduled = scheduler.run_once(SchedulerTrigger::kJobExited, free_snapshot);
  YORI_CHECK(scheduled.code == ScheduleResultCode::kQueueEmpty);
  YORI_CHECK(!scheduled.failed());

  auto divergent_queue = make_queue(1);
  YORI_CHECK(divergent_queue->admit(queued(99, std::chrono::seconds{20}, 1099)));
  yori::testing::InMemoryStateStore empty_store{{1, 1}};
  yori::scheduler::FifoScheduler divergent_scheduler{*divergent_queue, empty_store};
  scheduled = divergent_scheduler.run_once(SchedulerTrigger::kRecoveryCompleted, free_snapshot);
  YORI_CHECK(scheduled.code == ScheduleResultCode::kQueueStateDiverged);
  YORI_CHECK(scheduled.event.queue_error == yori::queue::QueueErrorCode::kJobNotFound);
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
  scheduled =
      missing_earlier_scheduler.run_once(SchedulerTrigger::kRecoveryCompleted, free_snapshot);
  YORI_CHECK(scheduled.code == ScheduleResultCode::kQueueStateDiverged);
  YORI_CHECK(scheduled.event.job_id == JobId{10});
  YORI_CHECK(scheduled.event.queue_error == yori::queue::QueueErrorCode::kJobNotFound);
  YORI_CHECK(missing_earlier_queue->size() == 1);

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
      ScheduleResultCode::kHeadBlocked,         ScheduleResultCode::kCancelled,
      ScheduleResultCode::kInvalidGpuSnapshot,  ScheduleResultCode::kStateLoadFailed,
      ScheduleResultCode::kQueueStateDiverged,  ScheduleResultCode::kStateWriteFailed,
      ScheduleResultCode::kQueueRollbackFailed,
  };
  for (const auto code : result_codes) {
    YORI_CHECK(std::string(yori::scheduler::to_string(code)) != "UNKNOWN");
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
