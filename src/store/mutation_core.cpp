#include "mutation_core.hpp"

#include <limits>
#include <utility>

namespace yori::store::mutation_core {
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

MutationOutcome failure(StateStoreErrorCode code) { return {code, 0, {}, {}}; }

}  // namespace

MutationOutcome apply_mutation(const std::map<job::JobId, StoredJob>& jobs,
                               const std::map<gpu::GpuUuid, gpu::GpuLease>& leases,
                               std::uint64_t current_revision, const ValidationConfig& config,
                               const StateMutation& mutation) {
  if (mutation.expected_revision != current_revision) {
    return failure(StateStoreErrorCode::kRevisionConflict);
  }
  if (mutation.entry_count() == 0 || mutation.entry_count() > StateMutation::kMaxEntries ||
      current_revision == std::numeric_limits<std::uint64_t>::max()) {
    return failure(StateStoreErrorCode::kInvalidMutation);
  }

  MutationOutcome outcome;
  outcome.next_jobs = jobs;
  outcome.next_leases = leases;

  for (const auto& record : mutation.create_jobs) {
    if (!record.id.valid() || !job::validate(record.spec) ||
        record.state != job::JobState::kQueued || record.revision != 0) {
      return failure(StateStoreErrorCode::kInvalidJob);
    }
    if (!validate_execution(record.execution, record.state, record.spec)) {
      return failure(StateStoreErrorCode::kInvalidExecutionRecord);
    }
    if (outcome.next_jobs.contains(record.id)) {
      return failure(StateStoreErrorCode::kJobAlreadyExists);
    }
    if (outcome.next_jobs.size() >= config.max_jobs) {
      return failure(StateStoreErrorCode::kCapacityExceeded);
    }
    outcome.next_jobs.emplace(record.id, record);
  }

  for (const auto& record : mutation.update_jobs) {
    if (!record.id.valid() || !job::validate(record.spec)) {
      return failure(StateStoreErrorCode::kInvalidJob);
    }
    if (!validate_execution(record.execution, record.state, record.spec)) {
      return failure(StateStoreErrorCode::kInvalidExecutionRecord);
    }
    const auto existing = outcome.next_jobs.find(record.id);
    if (existing == outcome.next_jobs.end()) {
      return failure(StateStoreErrorCode::kJobNotFound);
    }
    if (existing->second.revision == std::numeric_limits<std::uint64_t>::max() ||
        record.revision != existing->second.revision + 1) {
      return failure(StateStoreErrorCode::kInvalidJobRevision);
    }
    if (!same_spec(existing->second.spec, record.spec)) {
      return failure(StateStoreErrorCode::kJobSpecChanged);
    }
    if (!job::can_transition(existing->second.state, record.state)) {
      return failure(StateStoreErrorCode::kInvalidJobTransition);
    }
    existing->second = record;
  }

  for (const auto& uuid : mutation.release_leases) {
    if (!uuid.valid()) {
      return failure(StateStoreErrorCode::kInvalidLease);
    }
    const auto existing = outcome.next_leases.find(uuid);
    if (existing == outcome.next_leases.end()) {
      return failure(StateStoreErrorCode::kLeaseNotFound);
    }
    outcome.next_leases.erase(existing);
  }

  for (const auto& lease : mutation.acquire_leases) {
    if (!lease.gpu_uuid.valid() || !lease.job_id.valid()) {
      return failure(StateStoreErrorCode::kInvalidLease);
    }
    if (outcome.next_leases.contains(lease.gpu_uuid)) {
      return failure(StateStoreErrorCode::kGpuAlreadyLeased);
    }
    const auto job_record = outcome.next_jobs.find(lease.job_id);
    if (job_record == outcome.next_jobs.end() || !leaseable(job_record->second.state)) {
      return failure(StateStoreErrorCode::kInvalidLease);
    }
    for (const auto& [uuid, existing] : outcome.next_leases) {
      static_cast<void>(uuid);
      if (existing.job_id == lease.job_id) {
        return failure(StateStoreErrorCode::kJobAlreadyLeased);
      }
    }
    if (outcome.next_leases.size() >= config.max_leases) {
      return failure(StateStoreErrorCode::kCapacityExceeded);
    }
    outcome.next_leases.emplace(lease.gpu_uuid, lease);
  }

  for (const auto& [uuid, lease] : outcome.next_leases) {
    static_cast<void>(uuid);
    const auto job_record = outcome.next_jobs.find(lease.job_id);
    if (job_record == outcome.next_jobs.end() || !leaseable(job_record->second.state)) {
      return failure(StateStoreErrorCode::kInvalidLease);
    }
  }
  for (const auto& [id, record] : outcome.next_jobs) {
    std::size_t lease_count = 0;
    for (const auto& [uuid, lease] : outcome.next_leases) {
      static_cast<void>(uuid);
      if (lease.job_id == id) {
        ++lease_count;
      }
    }
    if ((leaseable(record.state) && lease_count != 1) ||
        (!leaseable(record.state) && lease_count != 0)) {
      return failure(StateStoreErrorCode::kInvalidLease);
    }
  }

  outcome.code = StateStoreErrorCode::kNone;
  outcome.next_revision = current_revision + 1;
  return outcome;
}

}  // namespace yori::store::mutation_core
