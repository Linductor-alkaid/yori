#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <yori/queue/job_queue.hpp>

#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

namespace {

yori::job::JobSpec spec(std::chrono::seconds submitted_at, std::uint32_t uid = 1000) {
  yori::job::JobSpec value;
  value.owner_uid = uid;
  value.owner_gid = uid;
  value.argv = {"train"};
  value.cwd = "/srv/training";
  value.submit_time = std::chrono::system_clock::time_point{submitted_at};
  return value;
}

yori::store::StoredJob queued(std::uint64_t id, std::chrono::seconds submitted_at,
                              std::uint32_t uid = 1000) {
  return {yori::job::JobId{id}, spec(submitted_at, uid), yori::job::JobState::kQueued, 0};
}

const yori::store::StoredJob& require_job(const yori::store::StateSnapshot& snapshot,
                                          std::uint64_t id) {
  for (const auto& record : snapshot.jobs) {
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

std::unique_ptr<yori::queue::GlobalJobQueue> make_queue(std::size_t capacity) {
  yori::queue::QueueErrorCode error{};
  auto queue = yori::queue::GlobalJobQueue::create({capacity}, error);
  YORI_CHECK(queue != nullptr);
  YORI_CHECK(error == yori::queue::QueueErrorCode::kNone);
  return queue;
}

void check_rejection(const yori::queue::QueueOperationResult& result,
                     yori::queue::QueueEventKind event_kind, yori::queue::QueueErrorCode error,
                     std::size_t queue_size) {
  YORI_CHECK(!result);
  YORI_CHECK(result.code == error);
  YORI_CHECK(result.event.kind == event_kind);
  YORI_CHECK(result.event.code == error);
  YORI_CHECK(result.event.size_before == queue_size);
  YORI_CHECK(result.event.size_after == queue_size);
}

}  // namespace

int main() {
  using yori::job::JobId;
  using yori::job::JobState;
  using yori::queue::QueueErrorCode;
  using yori::queue::QueueEventKind;

  QueueErrorCode creation_error{};
  YORI_CHECK(yori::queue::GlobalJobQueue::create({0}, creation_error) == nullptr);
  YORI_CHECK(creation_error == QueueErrorCode::kInvalidCapacity);
  YORI_CHECK(yori::queue::GlobalJobQueue::create({yori::queue::QueueConfig::kMaxCapacity + 1},
                                                 creation_error) == nullptr);
  YORI_CHECK(creation_error == QueueErrorCode::kInvalidCapacity);
  auto maximum_queue =
      yori::queue::GlobalJobQueue::create({yori::queue::QueueConfig::kMaxCapacity}, creation_error);
  YORI_CHECK(maximum_queue != nullptr);
  YORI_CHECK(creation_error == QueueErrorCode::kNone);
  YORI_CHECK(maximum_queue->capacity() == yori::queue::QueueConfig::kMaxCapacity);

  auto queue = make_queue(3);
  YORI_CHECK(queue->capacity() == 3);
  YORI_CHECK(queue->empty());
  YORI_CHECK(!queue->front());

  auto admission = queue->admit(queued(30, std::chrono::seconds{20}, 1001));
  YORI_CHECK(admission);
  YORI_CHECK(admission.position == 0);
  YORI_CHECK(admission.event.kind == QueueEventKind::kAdmissionAccepted);
  YORI_CHECK(admission.event.code == QueueErrorCode::kNone);
  YORI_CHECK(admission.event.job_id == JobId{30});
  YORI_CHECK(admission.event.size_before == 0);
  YORI_CHECK(admission.event.size_after == 1);
  YORI_CHECK(admission.event.capacity == 3);

  admission = queue->admit(queued(20, std::chrono::seconds{10}, 1002));
  YORI_CHECK(admission);
  YORI_CHECK(admission.position == 0);
  admission = queue->admit(queued(10, std::chrono::seconds{10}, 1003));
  YORI_CHECK(admission);
  YORI_CHECK(admission.position == 0);

  YORI_CHECK(queue->size() == 3);
  YORI_CHECK(queue->entries()[0].job_id == JobId{10});
  YORI_CHECK(queue->entries()[1].job_id == JobId{20});
  YORI_CHECK(queue->entries()[2].job_id == JobId{30});
  YORI_CHECK(require_front(*queue).job_id == JobId{10});
  YORI_CHECK(queue->contains(JobId{20}));

  auto rejected = queue->admit(queued(20, std::chrono::seconds{30}));
  check_rejection(rejected, QueueEventKind::kAdmissionRejected, QueueErrorCode::kDuplicateJob, 3);
  YORI_CHECK(rejected.event.job_id == JobId{20});

  rejected = queue->admit(queued(40, std::chrono::seconds{30}));
  check_rejection(rejected, QueueEventKind::kAdmissionRejected, QueueErrorCode::kCapacityExceeded,
                  3);

  auto invalid_id = queued(1, std::chrono::seconds{30});
  invalid_id.id = {};
  rejected = queue->admit(invalid_id);
  check_rejection(rejected, QueueEventKind::kAdmissionRejected, QueueErrorCode::kInvalidJobId, 3);
  YORI_CHECK(!rejected.event.job_id);

  auto invalid_spec = queued(41, std::chrono::seconds{30});
  invalid_spec.spec.owner_uid = 0;
  rejected = queue->admit(invalid_spec);
  check_rejection(rejected, QueueEventKind::kAdmissionRejected, QueueErrorCode::kInvalidJobSpec, 3);
  YORI_CHECK(rejected.spec_validation.code == yori::job::JobSpecErrorCode::kRootOwnerNotAllowed);

  auto not_queued = queued(41, std::chrono::seconds{30});
  not_queued.state = JobState::kCancelled;
  not_queued.revision = 1;
  rejected = queue->admit(not_queued);
  check_rejection(rejected, QueueEventKind::kAdmissionRejected, QueueErrorCode::kJobNotQueued, 3);

  auto invalid_revision = queued(41, std::chrono::seconds{30});
  invalid_revision.revision = 1;
  rejected = queue->admit(invalid_revision);
  check_rejection(rejected, QueueEventKind::kAdmissionRejected, QueueErrorCode::kInvalidJobRevision,
                  3);

  auto invalid_state = queued(41, std::chrono::seconds{30});
  invalid_state.state = static_cast<JobState>(999);
  rejected = queue->admit(invalid_state);
  check_rejection(rejected, QueueEventKind::kAdmissionRejected, QueueErrorCode::kInvalidJobState,
                  3);

  auto removal = queue->remove(JobId{20});
  YORI_CHECK(removal);
  YORI_CHECK(removal.position == 1);
  YORI_CHECK(removal.event.kind == QueueEventKind::kRemovalApplied);
  YORI_CHECK(removal.event.size_before == 3);
  YORI_CHECK(removal.event.size_after == 2);
  YORI_CHECK(!queue->contains(JobId{20}));

  auto removal_rejected = queue->remove(JobId{999});
  check_rejection(removal_rejected, QueueEventKind::kRemovalRejected, QueueErrorCode::kJobNotFound,
                  2);
  removal_rejected = queue->remove({});
  check_rejection(removal_rejected, QueueEventKind::kRemovalRejected, QueueErrorCode::kInvalidJobId,
                  2);

  admission = queue->admit(queued(40, std::chrono::seconds{5}));
  YORI_CHECK(admission);
  YORI_CHECK(admission.position == 0);
  YORI_CHECK(require_front(*queue).job_id == JobId{40});

  yori::testing::InMemoryStateStore store{{4, 1}};
  yori::store::StateMutation create;
  create.create_jobs = {queued(103, std::chrono::seconds{30}, 1013),
                        queued(102, std::chrono::seconds{20}, 1012),
                        queued(101, std::chrono::seconds{20}, 1011)};
  YORI_CHECK(store.apply(create));
  auto stored = store.load();
  YORI_CHECK(stored);
  const auto all_queued_snapshot = stored.snapshot;

  auto recovered = make_queue(3);
  auto restored = recovered->restore(stored.snapshot);
  YORI_CHECK(restored);
  YORI_CHECK(restored.event.kind == QueueEventKind::kRestoreApplied);
  YORI_CHECK(restored.event.size_before == 0);
  YORI_CHECK(restored.event.size_after == 3);
  YORI_CHECK(recovered->entries()[0].job_id == JobId{101});
  YORI_CHECK(recovered->entries()[1].job_id == JobId{102});
  YORI_CHECK(recovered->entries()[2].job_id == JobId{103});

  yori::store::StateMutation start;
  start.expected_revision = stored.snapshot.revision;
  auto starting = require_job(stored.snapshot, 103);
  starting.state = JobState::kStarting;
  ++starting.revision;
  start.update_jobs.push_back(starting);
  start.acquire_leases.push_back({yori::gpu::GpuUuid{"GPU-active"}, yori::job::JobId{103}});
  YORI_CHECK(store.apply(start));
  stored = store.load();
  restored = recovered->restore(stored.snapshot);
  YORI_CHECK(restored);
  YORI_CHECK(recovered->size() == 2);
  YORI_CHECK(!recovered->contains(JobId{103}));

  yori::store::StateMutation cancel;
  cancel.expected_revision = stored.snapshot.revision;
  auto cancelled = require_job(stored.snapshot, 101);
  cancelled.state = JobState::kCancelled;
  ++cancelled.revision;
  cancel.update_jobs.push_back(cancelled);
  YORI_CHECK(store.apply(cancel));
  stored = store.load();
  restored = recovered->restore(stored.snapshot);
  YORI_CHECK(restored);
  YORI_CHECK(recovered->size() == 1);
  YORI_CHECK(!recovered->contains(cancelled.id));

  const auto retained_front = require_front(*recovered);
  auto invalid_snapshot = stored.snapshot;
  invalid_snapshot.revision = 0;
  auto restore_rejected = recovered->restore(invalid_snapshot);
  check_rejection(restore_rejected, QueueEventKind::kRestoreRejected,
                  QueueErrorCode::kInvalidSnapshotRevision, 1);
  YORI_CHECK(require_front(*recovered).job_id == retained_front.job_id);

  invalid_snapshot = stored.snapshot;
  invalid_snapshot.jobs.push_back(invalid_snapshot.jobs.front());
  restore_rejected = recovered->restore(invalid_snapshot);
  check_rejection(restore_rejected, QueueEventKind::kRestoreRejected, QueueErrorCode::kDuplicateJob,
                  1);
  YORI_CHECK(restore_rejected.item_index == invalid_snapshot.jobs.size() - 1);
  YORI_CHECK(require_front(*recovered).job_id == retained_front.job_id);

  auto capacity_one = make_queue(1);
  restore_rejected = capacity_one->restore(all_queued_snapshot);
  check_rejection(restore_rejected, QueueEventKind::kRestoreRejected,
                  QueueErrorCode::kCapacityExceeded, 0);
  YORI_CHECK(capacity_one->empty());

  yori::store::StateSnapshot invalid_record_snapshot;
  invalid_record_snapshot.revision = 1;
  invalid_record_snapshot.jobs.push_back(queued(201, std::chrono::seconds{40}));
  invalid_record_snapshot.jobs[0].revision = 1;
  restore_rejected = recovered->restore(invalid_record_snapshot);
  check_rejection(restore_rejected, QueueEventKind::kRestoreRejected,
                  QueueErrorCode::kInvalidJobRevision, 1);
  YORI_CHECK(restore_rejected.item_index == 0);

  constexpr std::array error_codes{
      QueueErrorCode::kNone,
      QueueErrorCode::kInvalidCapacity,
      QueueErrorCode::kInvalidSnapshotRevision,
      QueueErrorCode::kInvalidJobId,
      QueueErrorCode::kInvalidJobSpec,
      QueueErrorCode::kInvalidJobState,
      QueueErrorCode::kInvalidJobRevision,
      QueueErrorCode::kJobNotQueued,
      QueueErrorCode::kDuplicateJob,
      QueueErrorCode::kCapacityExceeded,
      QueueErrorCode::kJobNotFound,
  };
  for (const auto code : error_codes) {
    YORI_CHECK(std::string(yori::queue::to_string(code)) != "UNKNOWN");
  }
  constexpr std::array event_kinds{
      QueueEventKind::kNone,
      QueueEventKind::kAdmissionAccepted,
      QueueEventKind::kAdmissionRejected,
      QueueEventKind::kRemovalApplied,
      QueueEventKind::kRemovalRejected,
      QueueEventKind::kRestoreApplied,
      QueueEventKind::kRestoreRejected,
  };
  for (const auto kind : event_kinds) {
    YORI_CHECK(std::string(yori::queue::to_string(kind)) != "UNKNOWN");
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
