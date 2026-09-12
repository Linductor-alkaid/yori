#include <algorithm>
#include <map>
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
                      std::uint64_t store_revision = 0, ScheduleEvaluation evaluation = {}) {
  return {code,
          {trigger, code, job_id, std::move(gpu_uuid), gpu_validation, store_error, queue_error,
           store_revision},
          std::move(evaluation)};
}

bool leased(const store::StateSnapshot& snapshot, const gpu::GpuUuid& uuid) noexcept {
  return std::any_of(snapshot.leases.begin(), snapshot.leases.end(),
                     [&uuid](const gpu::GpuLease& lease) { return lease.gpu_uuid == uuid; });
}

// kAny 候选选择（与既有确定性规则一致）：FREE 且未被 lease 的设备中物理
// index 最小者。候选集与具体 Job 无关，单次评估只计算一次。
const gpu::GpuObservation* select_any_gpu(const gpu::GpuObservationSnapshot& gpu_snapshot,
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

const gpu::GpuObservation* find_device(const gpu::GpuObservationSnapshot& gpu_snapshot,
                                       const gpu::GpuUuid& uuid) noexcept {
  for (const auto& device : gpu_snapshot.devices) {
    if (device.uuid == uuid) {
      return &device;
    }
  }
  return nullptr;
}

// kRequired 目标不可用时的等待原因（DEC-012 决策 5）。lease 是调度事实，
// 优先于观测（RULE-05）；观测缺失与 UNAVAILABLE 同归 AFFINITY_GPU_STATE。
WaitReason affinity_wait_reason(const gpu::GpuObservation* target,
                                const store::StateSnapshot& state_snapshot) noexcept {
  if (target == nullptr) {
    return WaitReason::kAffinityGpuState;
  }
  if (leased(state_snapshot, target->uuid)) {
    return WaitReason::kAffinityGpuAllocated;
  }
  switch (target->state) {
    case gpu::GpuObservedState::kExternalBusy:
      return WaitReason::kAffinityGpuExternal;
    case gpu::GpuObservedState::kUnavailable:
    case gpu::GpuObservedState::kFree:
      // kFree 但被 lease 在上方已排除；到达此处意味着 lease 排查与观测矛盾，
      // 保守归入状态类（不一致事实由 lease 矩阵校验兜底）。
      return WaitReason::kAffinityGpuState;
  }
  return WaitReason::kAffinityGpuState;
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

const char* to_string(WaitReason reason) noexcept {
  switch (reason) {
    case WaitReason::kNone:
      return "NONE";
    case WaitReason::kNoFreeGpu:
      return "NO_FREE_GPU";
    case WaitReason::kAffinityGpuAllocated:
      return "AFFINITY_GPU_ALLOCATED";
    case WaitReason::kAffinityGpuExternal:
      return "AFFINITY_GPU_EXTERNAL";
    case WaitReason::kAffinityGpuState:
      return "AFFINITY_GPU_STATE";
  }
  return "UNKNOWN";
}

const char* to_string(ScheduleResultCode code) noexcept {
  switch (code) {
    case ScheduleResultCode::kScheduled:
      return "SCHEDULED";
    case ScheduleResultCode::kQueueEmpty:
      return "QUEUE_EMPTY";
    case ScheduleResultCode::kNoCandidate:
      return "NO_CANDIDATE";
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

  const auto& entries = queue_.entries();
  const auto front = queue_.front();
  auto loaded = store_.load();
  if (!loaded) {
    return result(trigger, ScheduleResultCode::kStateLoadFailed,
                  front ? std::optional<job::JobId>{front->job_id} : std::nullopt, std::nullopt, {},
                  loaded.code);
  }

  // 队列一致性核对（防派生队列漏项；DEC-012 将核对扩展到扫描窗口内的被跳过
  // Job）：Store 中 QUEUED 记录与队列条目必须一一对应（数量 + 成员），窗口内
  // 条目还须满足 submit_time 与 revision=0 的字段级一致。
  std::map<job::JobId, const store::StoredJob*> queued_records;
  for (const auto& record : loaded.snapshot.jobs) {
    if (record.state == job::JobState::kQueued) {
      queued_records.emplace(record.id, &record);
    }
  }
  if (queued_records.size() != queue_.size()) {
    // 数量分歧：报告 store 中最早缺失于派生队列的 QUEUED Job（与旧队首核对
    // 的诊断语义一致，防"绕过更早 Job"的漏项定位）。
    const store::StoredJob* earliest_missing = nullptr;
    for (const auto& [id, record] : queued_records) {
      static_cast<void>(id);
      if (!queue_.contains(record->id) &&
          (earliest_missing == nullptr ||
           record->spec.submit_time < earliest_missing->spec.submit_time ||
           (record->spec.submit_time == earliest_missing->spec.submit_time &&
            record->id < earliest_missing->id))) {
        earliest_missing = record;
      }
    }
    const std::optional<job::JobId> reported =
        earliest_missing != nullptr
            ? std::optional<job::JobId>{earliest_missing->id}
            : (front ? std::optional<job::JobId>{front->job_id} : std::nullopt);
    return result(trigger, ScheduleResultCode::kQueueStateDiverged, reported, std::nullopt, {},
                  store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kJobNotFound,
                  loaded.snapshot.revision);
  }

  const std::size_t window = std::min<std::size_t>(config_.scan_window, entries.size());
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    const auto record_iter = queued_records.find(entry.job_id);
    if (record_iter == queued_records.end()) {
      return result(trigger, ScheduleResultCode::kQueueStateDiverged, entry.job_id, std::nullopt,
                    {}, store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kJobNotFound,
                    loaded.snapshot.revision);
    }
    if (index < window) {
      const store::StoredJob* record = record_iter->second;
      queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone;
      if (record->spec.submit_time != entry.submit_time) {
        queue_error = queue::QueueErrorCode::kInvalidJobSpec;
      } else if (record->revision != 0) {
        queue_error = queue::QueueErrorCode::kInvalidJobRevision;
      }
      if (queue_error != queue::QueueErrorCode::kNone) {
        return result(trigger, ScheduleResultCode::kQueueStateDiverged, entry.job_id, std::nullopt,
                      {}, store::StateStoreErrorCode::kNone, queue_error, loaded.snapshot.revision);
      }
    }
  }

  if (entries.empty()) {
    return result(trigger, ScheduleResultCode::kQueueEmpty, std::nullopt, std::nullopt, {},
                  store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kNone,
                  loaded.snapshot.revision);
  }

  ScheduleEvaluation evaluation;
  evaluation.window_truncated = entries.size() > window;

  // kAny 候选与具体 Job 无关，单次评估计算一次。
  const gpu::GpuObservation* any_candidate = select_any_gpu(gpu_snapshot, loaded.snapshot);

  for (std::size_t index = 0; index < window; ++index) {
    const queue::QueueEntry& entry = entries[index];
    const store::StoredJob* record = queued_records.at(entry.job_id);
    const job::GpuPlacement& placement = record->spec.gpu_placement;

    const gpu::GpuObservation* selected = nullptr;
    if (placement.mode == job::GpuPlacementMode::kRequired) {
      const gpu::GpuUuid& target = placement.devices.front();
      const gpu::GpuObservation* observation = find_device(gpu_snapshot, target);
      if (observation != nullptr && observation->state == gpu::GpuObservedState::kFree &&
          !leased(loaded.snapshot, target)) {
        selected = observation;
      }
    } else {
      selected = any_candidate;
    }

    if (selected != nullptr) {
      auto starting = *record;
      starting.state = job::JobState::kStarting;
      ++starting.revision;

      store::StateMutation mutation;
      mutation.expected_revision = loaded.snapshot.revision;
      mutation.update_jobs.push_back(std::move(starting));
      mutation.acquire_leases.push_back({selected->uuid, record->id});

      const auto removal = queue_.remove(record->id);
      if (!removal) {
        return result(trigger, ScheduleResultCode::kQueueStateDiverged, record->id, selected->uuid,
                      {}, store::StateStoreErrorCode::kNone, removal.code,
                      loaded.snapshot.revision);
      }

      const auto written = store_.apply(mutation);
      if (!written) {
        const auto rollback = queue_.admit(*record);
        if (!rollback) {
          return result(trigger, ScheduleResultCode::kQueueRollbackFailed, record->id,
                        selected->uuid, {}, written.code, rollback.code, written.revision);
        }
        return result(trigger, ScheduleResultCode::kStateWriteFailed, record->id, selected->uuid,
                      {}, written.code, queue::QueueErrorCode::kNone, written.revision);
      }

      return result(trigger, ScheduleResultCode::kScheduled, record->id, selected->uuid, {},
                    store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kNone,
                    written.revision, std::move(evaluation));
    }

    // 跳过：保持 QUEUED 与原队列位置，记录携带原因的结构化事件。
    ScheduleSkip skip;
    skip.job = entry.job_id;
    if (placement.mode == job::GpuPlacementMode::kRequired) {
      skip.reason = affinity_wait_reason(find_device(gpu_snapshot, placement.devices.front()),
                                         loaded.snapshot);
      skip.target = placement.devices.front();
    } else {
      skip.reason = WaitReason::kNoFreeGpu;
    }
    evaluation.skipped.push_back(std::move(skip));
  }

  return result(trigger, ScheduleResultCode::kNoCandidate, std::nullopt, std::nullopt, {},
                store::StateStoreErrorCode::kNone, queue::QueueErrorCode::kNone,
                loaded.snapshot.revision, std::move(evaluation));
}

ScheduleResult FifoScheduler::cancelled(SchedulerTrigger trigger) noexcept {
  return result(trigger, ScheduleResultCode::kCancelled);
}

}  // namespace yori::scheduler
