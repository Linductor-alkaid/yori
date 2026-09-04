#include <algorithm>
#include <utility>
#include <yori/scheduler/scheduler.hpp>

namespace yori::scheduler {
namespace {

ScheduleResult result(SchedulerTrigger trigger, ScheduleResultCode code,
                      std::optional<job::JobId> job_id = std::nullopt,
                      std::optional<gpu::GpuUuid> gpu_uuid = std::nullopt,
                      gpu::GpuObservationValidationResult gpu_validation = {},
                      store::StateStoreErrorCode store_error = store::StateStoreErrorCode::kNone,
                      queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone,
                      std::uint64_t store_revision = 0) {
  return {code,
          {trigger, code, std::move(job_id), std::move(gpu_uuid), gpu_validation, store_error,
           queue_error, store_revision}};
}

bool leased(const store::StateSnapshot& snapshot, const gpu::GpuUuid& uuid) noexcept {
  return std::any_of(snapshot.leases.begin(), snapshot.leases.end(),
                     [&uuid](const gpu::GpuLease& lease) { return lease.gpu_uuid == uuid; });
}

const gpu::GpuObservation* select_gpu(const gpu::GpuObservationSnapshot& gpu_snapshot,
                                      const store::StateSnapshot& state_snapshot) noexcept {
  const gpu::GpuObservation* selected = nullptr;
  for (const auto& device : gpu_snapshot.devices) {
    if (device.state != gpu::GpuObservedState::kFree || leased(state_snapshot, device.uuid)) {
      continue;
    }
    if (selected == nullptr || device.index < selected->index) {
      selected = &device;
    }
  }
  return selected;
}

}  // namespace

const char* to_string(SchedulerTrigger trigger) noexcept {
  switch (trigger) {
    case SchedulerTrigger::kJobSubmitted:
      return "JOB_SUBMITTED";
    case SchedulerTrigger::kJobExited:
      return "JOB_EXITED";
    case SchedulerTrigger::kJobCancelled:
      return "JOB_CANCELLED";
    case SchedulerTrigger::kGpuStateChanged:
      return "GPU_STATE_CHANGED";
    case SchedulerTrigger::kRecoveryCompleted:
      return "RECOVERY_COMPLETED";
    case SchedulerTrigger::kAdminStateChanged:
      return "ADMIN_STATE_CHANGED";
  }
  return "UNKNOWN";
}

const char* to_string(ScheduleResultCode code) noexcept {
  switch (code) {
    case ScheduleResultCode::kScheduled:
      return "SCHEDULED";
    case ScheduleResultCode::kQueueEmpty:
      return "QUEUE_EMPTY";
    case ScheduleResultCode::kHeadBlocked:
      return "HEAD_BLOCKED";
    case ScheduleResultCode::kCancelled:
      return "CANCELLED";
    case ScheduleResultCode::kInvalidGpuSnapshot:
      return "INVALID_GPU_SNAPSHOT";
    case ScheduleResultCode::kStateLoadFailed:
      return "STATE_LOAD_FAILED";
    case ScheduleResultCode::kQueueStateDiverged:
      return "QUEUE_STATE_DIVERGED";
    case ScheduleResultCode::kStateWriteFailed:
      return "STATE_WRITE_FAILED";
    case ScheduleResultCode::kQueueRollbackFailed:
      return "QUEUE_ROLLBACK_FAILED";
  }
  return "UNKNOWN";
}

ScheduleResult FifoScheduler::run_once(SchedulerTrigger trigger,
                                       const gpu::GpuObservationSnapshot& gpu_snapshot) {
  const auto gpu_validation = gpu::validate(gpu_snapshot);
  if (!gpu_validation) {
    return result(trigger, ScheduleResultCode::kInvalidGpuSnapshot, std::nullopt, std::nullopt,
                  gpu_validation);
  }

  const auto front = queue_.front();
  auto loaded = store_.load();
  if (!loaded) {
    return result(trigger, ScheduleResultCode::kStateLoadFailed,
                  front ? std::optional<job::JobId>{front->job_id} : std::nullopt, std::nullopt, {},
                  loaded.code);
  }

  const store::StoredJob* earliest = nullptr;
  std::size_t queued_count = 0;
  for (const auto& record : loaded.snapshot.jobs) {
    if (record.state != job::JobState::kQueued) {
      continue;
    }
    ++queued_count;
    if (earliest == nullptr || record.spec.submit_time < earliest->spec.submit_time ||
        (record.spec.submit_time == earliest->spec.submit_time && record.id < earliest->id)) {
      earliest = &record;
    }
  }

  if (!front) {
    if (queued_count == 0 || earliest == nullptr) {
      return result(trigger, ScheduleResultCode::kQueueEmpty, std::nullopt, std::nullopt, {},
                    store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kNone,
                    loaded.snapshot.revision);
    }
    return result(trigger, ScheduleResultCode::kQueueStateDiverged, earliest->id, std::nullopt, {},
                  store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kJobNotFound,
                  loaded.snapshot.revision);
  }

  if (earliest == nullptr || queued_count != queue_.size() || earliest->id != front->job_id ||
      earliest->spec.submit_time != front->submit_time || earliest->revision != 0) {
    auto queue_error = queue::QueueErrorCode::kJobNotFound;
    if (earliest != nullptr && earliest->id == front->job_id &&
        earliest->spec.submit_time != front->submit_time) {
      queue_error = queue::QueueErrorCode::kInvalidJobSpec;
    } else if (earliest != nullptr && earliest->id == front->job_id && earliest->revision != 0) {
      queue_error = queue::QueueErrorCode::kInvalidJobRevision;
    }
    return result(trigger, ScheduleResultCode::kQueueStateDiverged,
                  earliest != nullptr ? std::optional<job::JobId>{earliest->id}
                                      : std::optional<job::JobId>{front->job_id},
                  std::nullopt, {}, store::StateStoreErrorCode::kNone, queue_error,
                  loaded.snapshot.revision);
  }

  const auto* record = earliest;

  const auto* selected = select_gpu(gpu_snapshot, loaded.snapshot);
  if (selected == nullptr) {
    return result(trigger, ScheduleResultCode::kHeadBlocked, front->job_id, std::nullopt, {},
                  store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kNone,
                  loaded.snapshot.revision);
  }

  auto starting = *record;
  starting.state = job::JobState::kStarting;
  ++starting.revision;

  store::StateMutation mutation;
  mutation.expected_revision = loaded.snapshot.revision;
  mutation.update_jobs.push_back(starting);
  mutation.acquire_leases.push_back({selected->uuid, record->id});

  const auto removal = queue_.remove(record->id);
  if (!removal) {
    return result(trigger, ScheduleResultCode::kQueueStateDiverged, record->id, selected->uuid, {},
                  store::StateStoreErrorCode::kNone, removal.code, loaded.snapshot.revision);
  }

  const auto written = store_.apply(mutation);
  if (!written) {
    const auto rollback = queue_.admit(*record);
    if (!rollback) {
      return result(trigger, ScheduleResultCode::kQueueRollbackFailed, record->id, selected->uuid,
                    {}, written.code, rollback.code, written.revision);
    }
    return result(trigger, ScheduleResultCode::kStateWriteFailed, record->id, selected->uuid, {},
                  written.code, queue::QueueErrorCode::kNone, written.revision);
  }

  return result(trigger, ScheduleResultCode::kScheduled, record->id, selected->uuid, {},
                store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kNone, written.revision);
}

ScheduleResult FifoScheduler::cancelled(SchedulerTrigger trigger) noexcept {
  return result(trigger, ScheduleResultCode::kCancelled);
}

}  // namespace yori::scheduler
