#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

namespace {

yori::job::JobSpec spec(std::uint32_t uid = 1000) {
  yori::job::JobSpec value;
  value.owner_uid = uid;
  value.owner_gid = uid;
  value.argv = {"train"};
  value.cwd = "/srv/training";
  value.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{uid}};
  return value;
}

yori::store::StoredJob queued(std::uint64_t id, std::uint32_t uid = 1000) {
  return {yori::job::JobId{id}, spec(uid), yori::job::JobState::kQueued, 0};
}

yori::store::StoredJob advanced(const yori::store::StoredJob& current, yori::job::JobState state) {
  auto next = current;
  next.state = state;
  ++next.revision;
  return next;
}

yori::gpu::GpuLease lease(const char* uuid, std::uint64_t job_id) {
  return {yori::gpu::GpuUuid{uuid}, yori::job::JobId{job_id}};
}

const yori::store::StoredJob* find_job(const yori::store::StateSnapshot& snapshot,
                                       std::uint64_t id) {
  for (const auto& job : snapshot.jobs) {
    if (job.id == yori::job::JobId{id}) {
      return &job;
    }
  }
  return nullptr;
}

const yori::store::StoredJob& require_job(const yori::store::StateSnapshot& snapshot,
                                          std::uint64_t id) {
  const auto* found = find_job(snapshot, id);
  if (found == nullptr) {
    std::fprintf(stderr, "required Job %llu is missing\n", static_cast<unsigned long long>(id));
    std::exit(1);
  }
  return *found;
}

void check_error(const yori::store::StateStoreWriteResult& result,
                 yori::store::StateStoreErrorCode expected, std::uint64_t expected_revision) {
  YORI_CHECK(!result);
  YORI_CHECK(result.code == expected);
  YORI_CHECK(result.revision == expected_revision);
  YORI_CHECK(std::string(yori::store::to_string(result.code)) != "UNKNOWN");
}

}  // namespace

int main() {
  using yori::job::JobState;
  using yori::store::StateMutation;
  using yori::store::StateStoreErrorCode;

  yori::testing::InMemoryStateStore store{{2, 1}};

  StateMutation empty;
  check_error(store.apply(empty), StateStoreErrorCode::kInvalidMutation, 0);

  StateMutation create_first;
  create_first.create_jobs.push_back(queued(1));
  auto write = store.apply(create_first);
  YORI_CHECK(write);
  YORI_CHECK(write.revision == 1);

  auto snapshot = store.load();
  YORI_CHECK(snapshot);
  YORI_CHECK(snapshot.snapshot.revision == 1);
  YORI_CHECK(snapshot.snapshot.jobs.size() == 1);
  YORI_CHECK(snapshot.snapshot.leases.empty());
  YORI_CHECK(require_job(snapshot.snapshot, 1).state == JobState::kQueued);

  StateMutation stale;
  stale.expected_revision = 0;
  stale.create_jobs.push_back(queued(2, 1001));
  check_error(store.apply(stale), StateStoreErrorCode::kRevisionConflict, 1);

  StateMutation atomic_failure;
  atomic_failure.expected_revision = 1;
  atomic_failure.create_jobs.push_back(queued(2, 1001));
  atomic_failure.create_jobs.push_back(queued(1));
  check_error(store.apply(atomic_failure), StateStoreErrorCode::kJobAlreadyExists, 1);
  snapshot = store.load();
  YORI_CHECK(snapshot.snapshot.jobs.size() == 1);
  YORI_CHECK(find_job(snapshot.snapshot, 2) == nullptr);

  StateMutation schedule_first;
  schedule_first.expected_revision = 1;
  schedule_first.update_jobs.push_back(
      advanced(require_job(snapshot.snapshot, 1), JobState::kStarting));
  schedule_first.acquire_leases.push_back(lease("GPU-aaaa", 1));
  write = store.apply(schedule_first);
  YORI_CHECK(write);
  YORI_CHECK(write.revision == 2);

  snapshot = store.load();
  YORI_CHECK(snapshot.snapshot.jobs[0].state == JobState::kStarting);
  YORI_CHECK(snapshot.snapshot.jobs[0].revision == 1);
  YORI_CHECK(snapshot.snapshot.leases.size() == 1);
  YORI_CHECK(snapshot.snapshot.leases[0].job_id == yori::job::JobId{1});

  StateMutation create_second;
  create_second.expected_revision = 2;
  create_second.create_jobs.push_back(queued(2, 1001));
  YORI_CHECK(store.apply(create_second));

  snapshot = store.load();
  StateMutation gpu_conflict;
  gpu_conflict.expected_revision = 3;
  gpu_conflict.update_jobs.push_back(
      advanced(require_job(snapshot.snapshot, 2), JobState::kStarting));
  gpu_conflict.acquire_leases.push_back(lease("GPU-aaaa", 2));
  check_error(store.apply(gpu_conflict), StateStoreErrorCode::kGpuAlreadyLeased, 3);
  snapshot = store.load();
  YORI_CHECK(require_job(snapshot.snapshot, 2).state == JobState::kQueued);

  StateMutation active_without_lease;
  active_without_lease.expected_revision = 3;
  active_without_lease.update_jobs.push_back(
      advanced(require_job(snapshot.snapshot, 2), JobState::kStarting));
  check_error(store.apply(active_without_lease), StateStoreErrorCode::kInvalidLease, 3);

  StateMutation lease_queued_job;
  lease_queued_job.expected_revision = 3;
  lease_queued_job.acquire_leases.push_back(lease("GPU-bbbb", 2));
  check_error(store.apply(lease_queued_job), StateStoreErrorCode::kInvalidLease, 3);

  StateMutation changed_spec;
  changed_spec.expected_revision = 3;
  auto changed_record = advanced(require_job(snapshot.snapshot, 1), JobState::kRunning);
  changed_record.spec.argv.push_back("--changed");
  changed_spec.update_jobs.push_back(std::move(changed_record));
  check_error(store.apply(changed_spec), StateStoreErrorCode::kJobSpecChanged, 3);

  StateMutation invalid_job_revision;
  invalid_job_revision.expected_revision = 3;
  auto skipped_revision = advanced(require_job(snapshot.snapshot, 1), JobState::kRunning);
  ++skipped_revision.revision;
  invalid_job_revision.update_jobs.push_back(std::move(skipped_revision));
  check_error(store.apply(invalid_job_revision), StateStoreErrorCode::kInvalidJobRevision, 3);

  StateMutation missing_release;
  missing_release.expected_revision = 3;
  missing_release.release_leases.emplace_back("GPU-missing");
  check_error(store.apply(missing_release), StateStoreErrorCode::kLeaseNotFound, 3);

  StateMutation invalid_transition;
  invalid_transition.expected_revision = 3;
  invalid_transition.update_jobs.push_back(
      advanced(require_job(snapshot.snapshot, 1), JobState::kFinished));
  check_error(store.apply(invalid_transition), StateStoreErrorCode::kInvalidJobTransition, 3);

  StateMutation mark_running;
  mark_running.expected_revision = 3;
  mark_running.update_jobs.push_back(
      advanced(require_job(snapshot.snapshot, 1), JobState::kRunning));
  YORI_CHECK(store.apply(mark_running));

  snapshot = store.load();
  StateMutation terminal_without_release;
  terminal_without_release.expected_revision = 4;
  terminal_without_release.update_jobs.push_back(
      advanced(require_job(snapshot.snapshot, 1), JobState::kFinished));
  check_error(store.apply(terminal_without_release), StateStoreErrorCode::kInvalidLease, 4);

  StateMutation finish_and_release;
  finish_and_release.expected_revision = 4;
  finish_and_release.update_jobs = terminal_without_release.update_jobs;
  finish_and_release.release_leases.push_back(yori::gpu::GpuUuid{"GPU-aaaa"});
  write = store.apply(finish_and_release);
  YORI_CHECK(write);
  YORI_CHECK(write.revision == 5);
  snapshot = store.load();
  YORI_CHECK(require_job(snapshot.snapshot, 1).state == JobState::kFinished);
  YORI_CHECK(snapshot.snapshot.leases.empty());

  StateMutation over_capacity;
  over_capacity.expected_revision = 5;
  over_capacity.create_jobs.push_back(queued(3, 1002));
  check_error(store.apply(over_capacity), StateStoreErrorCode::kCapacityExceeded, 5);

  StateMutation too_large;
  too_large.expected_revision = 5;
  for (std::size_t index = 0; index <= StateMutation::kMaxEntries; ++index) {
    too_large.release_leases.emplace_back("GPU-" + std::to_string(index));
  }
  check_error(store.apply(too_large), StateStoreErrorCode::kInvalidMutation, 5);

  store.fail_with(StateStoreErrorCode::kBackendUnavailable);
  YORI_CHECK(store.load().code == StateStoreErrorCode::kBackendUnavailable);
  StateMutation backend_write;
  backend_write.expected_revision = 5;
  backend_write.update_jobs.push_back(queued(1));
  check_error(store.apply(backend_write), StateStoreErrorCode::kBackendUnavailable, 5);
  store.clear_failure();
  YORI_CHECK(store.load().snapshot.revision == 5);

  yori::testing::InMemoryStateStore lease_capacity_store{{2, 1}};
  StateMutation create_two;
  create_two.create_jobs = {queued(10, 1010), queued(11, 1011)};
  YORI_CHECK(lease_capacity_store.apply(create_two));
  auto two_jobs = lease_capacity_store.load();
  StateMutation exceed_lease_capacity;
  exceed_lease_capacity.expected_revision = 1;
  exceed_lease_capacity.update_jobs = {
      advanced(require_job(two_jobs.snapshot, 10), JobState::kStarting),
      advanced(require_job(two_jobs.snapshot, 11), JobState::kStarting)};
  exceed_lease_capacity.acquire_leases = {lease("GPU-0010", 10), lease("GPU-0011", 11)};
  check_error(lease_capacity_store.apply(exceed_lease_capacity),
              StateStoreErrorCode::kCapacityExceeded, 1);
  two_jobs = lease_capacity_store.load();
  YORI_CHECK(require_job(two_jobs.snapshot, 10).state == JobState::kQueued);
  YORI_CHECK(two_jobs.snapshot.leases.empty());

  yori::testing::InMemoryStateStore one_job_two_leases{{1, 2}};
  StateMutation create_one;
  create_one.create_jobs.push_back(queued(20, 1020));
  YORI_CHECK(one_job_two_leases.apply(create_one));
  auto one_job = one_job_two_leases.load();
  StateMutation schedule_one;
  schedule_one.expected_revision = 1;
  schedule_one.update_jobs.push_back(
      advanced(require_job(one_job.snapshot, 20), JobState::kStarting));
  schedule_one.acquire_leases.push_back(lease("GPU-0020", 20));
  YORI_CHECK(one_job_two_leases.apply(schedule_one));
  StateMutation second_lease_for_job;
  second_lease_for_job.expected_revision = 2;
  second_lease_for_job.acquire_leases.push_back(lease("GPU-0021", 20));
  check_error(one_job_two_leases.apply(second_lease_for_job),
              StateStoreErrorCode::kJobAlreadyLeased, 2);

  return yori::testing::failure_count == 0 ? 0 : 1;
}
