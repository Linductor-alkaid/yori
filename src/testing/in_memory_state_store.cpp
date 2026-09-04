#include "testing/in_memory_state_store.hpp"

#include <limits>
#include <utility>

namespace yori::testing {
namespace {

bool same_spec(const job::JobSpec& lhs, const job::JobSpec& rhs) {
  return lhs.owner_uid == rhs.owner_uid && lhs.owner_gid == rhs.owner_gid && lhs.argv == rhs.argv &&
         lhs.cwd == rhs.cwd && lhs.env == rhs.env && lhs.gpu_request == rhs.gpu_request &&
         lhs.launch_profile == rhs.launch_profile &&
         lhs.tensorboard_logdir == rhs.tensorboard_logdir && lhs.submit_time == rhs.submit_time;
}

bool leaseable(job::JobState state) noexcept {
  return state == job::JobState::kStarting || state == job::JobState::kRunning ||
         state == job::JobState::kStopping;
}

store::StateStoreWriteResult failure(store::StateStoreErrorCode code, std::uint64_t revision) {
  return {code, revision};
}

}  // namespace

void InMemoryStateStore::fail_with(store::StateStoreErrorCode error) noexcept {
  if (error == store::StateStoreErrorCode::kNone) {
    failure_.reset();
    return;
  }
  failure_ = error;
}

void InMemoryStateStore::clear_failure() noexcept { failure_.reset(); }

store::StateStoreLoadResult InMemoryStateStore::load() {
  if (failure_) {
    return {*failure_, {}};
  }

  store::StateSnapshot snapshot;
  snapshot.revision = revision_;
  snapshot.jobs.reserve(jobs_.size());
  snapshot.leases.reserve(leases_.size());
  for (const auto& [id, record] : jobs_) {
    static_cast<void>(id);
    snapshot.jobs.push_back(record);
  }
  for (const auto& [uuid, lease] : leases_) {
    static_cast<void>(uuid);
    snapshot.leases.push_back(lease);
  }
  return {store::StateStoreErrorCode::kNone, std::move(snapshot)};
}

store::StateStoreWriteResult InMemoryStateStore::apply(const store::StateMutation& mutation) {
  if (failure_) {
    return failure(*failure_, revision_);
  }
  if (mutation.expected_revision != revision_) {
    return failure(store::StateStoreErrorCode::kRevisionConflict, revision_);
  }
  if (mutation.entry_count() == 0 || mutation.entry_count() > store::StateMutation::kMaxEntries ||
      revision_ == std::numeric_limits<std::uint64_t>::max()) {
    return failure(store::StateStoreErrorCode::kInvalidMutation, revision_);
  }

  auto next_jobs = jobs_;
  auto next_leases = leases_;

  for (const auto& record : mutation.create_jobs) {
    if (!record.id.valid() || !job::validate(record.spec) ||
        record.state != job::JobState::kQueued || record.revision != 0) {
      return failure(store::StateStoreErrorCode::kInvalidJob, revision_);
    }
    if (next_jobs.contains(record.id)) {
      return failure(store::StateStoreErrorCode::kJobAlreadyExists, revision_);
    }
    if (next_jobs.size() >= config_.max_jobs) {
      return failure(store::StateStoreErrorCode::kCapacityExceeded, revision_);
    }
    next_jobs.emplace(record.id, record);
  }

  for (const auto& record : mutation.update_jobs) {
    if (!record.id.valid() || !job::validate(record.spec)) {
      return failure(store::StateStoreErrorCode::kInvalidJob, revision_);
    }
    const auto existing = next_jobs.find(record.id);
    if (existing == next_jobs.end()) {
      return failure(store::StateStoreErrorCode::kJobNotFound, revision_);
    }
    if (existing->second.revision == std::numeric_limits<std::uint64_t>::max() ||
        record.revision != existing->second.revision + 1) {
      return failure(store::StateStoreErrorCode::kInvalidJobRevision, revision_);
    }
    if (!same_spec(existing->second.spec, record.spec)) {
      return failure(store::StateStoreErrorCode::kJobSpecChanged, revision_);
    }
    if (!job::can_transition(existing->second.state, record.state)) {
      return failure(store::StateStoreErrorCode::kInvalidJobTransition, revision_);
    }
    existing->second = record;
  }

  for (const auto& uuid : mutation.release_leases) {
    if (!uuid.valid()) {
      return failure(store::StateStoreErrorCode::kInvalidLease, revision_);
    }
    const auto existing = next_leases.find(uuid);
    if (existing == next_leases.end()) {
      return failure(store::StateStoreErrorCode::kLeaseNotFound, revision_);
    }
    next_leases.erase(existing);
  }

  for (const auto& lease : mutation.acquire_leases) {
    if (!lease.gpu_uuid.valid() || !lease.job_id.valid()) {
      return failure(store::StateStoreErrorCode::kInvalidLease, revision_);
    }
    if (next_leases.contains(lease.gpu_uuid)) {
      return failure(store::StateStoreErrorCode::kGpuAlreadyLeased, revision_);
    }
    const auto job_record = next_jobs.find(lease.job_id);
    if (job_record == next_jobs.end() || !leaseable(job_record->second.state)) {
      return failure(store::StateStoreErrorCode::kInvalidLease, revision_);
    }
    for (const auto& [uuid, existing] : next_leases) {
      static_cast<void>(uuid);
      if (existing.job_id == lease.job_id) {
        return failure(store::StateStoreErrorCode::kJobAlreadyLeased, revision_);
      }
    }
    if (next_leases.size() >= config_.max_leases) {
      return failure(store::StateStoreErrorCode::kCapacityExceeded, revision_);
    }
    next_leases.emplace(lease.gpu_uuid, lease);
  }

  for (const auto& [uuid, lease] : next_leases) {
    static_cast<void>(uuid);
    const auto job_record = next_jobs.find(lease.job_id);
    if (job_record == next_jobs.end() || !leaseable(job_record->second.state)) {
      return failure(store::StateStoreErrorCode::kInvalidLease, revision_);
    }
  }
  for (const auto& [id, record] : next_jobs) {
    std::size_t lease_count = 0;
    for (const auto& [uuid, lease] : next_leases) {
      static_cast<void>(uuid);
      if (lease.job_id == id) {
        ++lease_count;
      }
    }
    if ((leaseable(record.state) && lease_count != 1) ||
        (!leaseable(record.state) && lease_count != 0)) {
      return failure(store::StateStoreErrorCode::kInvalidLease, revision_);
    }
  }

  jobs_ = std::move(next_jobs);
  leases_ = std::move(next_leases);
  ++revision_;
  return {store::StateStoreErrorCode::kNone, revision_};
}

}  // namespace yori::testing
