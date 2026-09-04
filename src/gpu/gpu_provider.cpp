#include <yori/gpu/gpu_provider.hpp>

namespace yori::gpu {

GpuObservationValidationResult validate(const GpuObservationSnapshot& snapshot) noexcept {
  if (snapshot.revision == 0) {
    return {GpuObservationErrorCode::kInvalidRevision, std::nullopt};
  }
  if (snapshot.observed_at.time_since_epoch() <= std::chrono::system_clock::duration::zero()) {
    return {GpuObservationErrorCode::kInvalidObservationTime, std::nullopt};
  }
  if (snapshot.devices.size() > GpuObservationSnapshot::kMaxDevices) {
    return {GpuObservationErrorCode::kTooManyDevices, std::nullopt};
  }

  for (std::size_t index = 0; index < snapshot.devices.size(); ++index) {
    const auto& device = snapshot.devices[index];
    if (!device.uuid.valid()) {
      return {GpuObservationErrorCode::kInvalidUuid, index};
    }
    switch (device.state) {
      case GpuObservedState::kFree:
      case GpuObservedState::kExternalBusy:
      case GpuObservedState::kUnavailable:
        break;
      default:
        return {GpuObservationErrorCode::kInvalidState, index};
    }
    if (device.telemetry.utilization_percent && *device.telemetry.utilization_percent > 100) {
      return {GpuObservationErrorCode::kInvalidUtilization, index};
    }
    if (device.telemetry.memory_used_bytes && !device.telemetry.memory_total_bytes) {
      return {GpuObservationErrorCode::kInvalidMemoryTelemetry, index};
    }
    if (device.telemetry.memory_total_bytes &&
        (*device.telemetry.memory_total_bytes == 0 ||
         (device.telemetry.memory_used_bytes &&
          *device.telemetry.memory_used_bytes > *device.telemetry.memory_total_bytes))) {
      return {GpuObservationErrorCode::kInvalidMemoryTelemetry, index};
    }

    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (snapshot.devices[earlier].uuid == device.uuid) {
        return {GpuObservationErrorCode::kDuplicateUuid, index};
      }
      if (snapshot.devices[earlier].index == device.index) {
        return {GpuObservationErrorCode::kDuplicateIndex, index};
      }
    }
  }
  return {};
}

const char* to_string(GpuObservedState state) noexcept {
  switch (state) {
    case GpuObservedState::kFree:
      return "FREE";
    case GpuObservedState::kExternalBusy:
      return "EXTERNAL_BUSY";
    case GpuObservedState::kUnavailable:
      return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

const char* to_string(GpuLogicalState state) noexcept {
  switch (state) {
    case GpuLogicalState::kFree:
      return "FREE";
    case GpuLogicalState::kAllocated:
      return "ALLOCATED";
    case GpuLogicalState::kExternalBusy:
      return "EXTERNAL_BUSY";
    case GpuLogicalState::kUnavailable:
      return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

const char* to_string(GpuObservationErrorCode code) noexcept {
  switch (code) {
    case GpuObservationErrorCode::kNone:
      return "NONE";
    case GpuObservationErrorCode::kInvalidRevision:
      return "INVALID_REVISION";
    case GpuObservationErrorCode::kInvalidObservationTime:
      return "INVALID_OBSERVATION_TIME";
    case GpuObservationErrorCode::kTooManyDevices:
      return "TOO_MANY_DEVICES";
    case GpuObservationErrorCode::kInvalidUuid:
      return "INVALID_UUID";
    case GpuObservationErrorCode::kDuplicateUuid:
      return "DUPLICATE_UUID";
    case GpuObservationErrorCode::kDuplicateIndex:
      return "DUPLICATE_INDEX";
    case GpuObservationErrorCode::kInvalidState:
      return "INVALID_STATE";
    case GpuObservationErrorCode::kInvalidUtilization:
      return "INVALID_UTILIZATION";
    case GpuObservationErrorCode::kInvalidMemoryTelemetry:
      return "INVALID_MEMORY_TELEMETRY";
  }
  return "UNKNOWN";
}

const char* to_string(GpuProviderErrorCode code) noexcept {
  switch (code) {
    case GpuProviderErrorCode::kNone:
      return "NONE";
    case GpuProviderErrorCode::kBackendUnavailable:
      return "BACKEND_UNAVAILABLE";
    case GpuProviderErrorCode::kPermissionDenied:
      return "PERMISSION_DENIED";
    case GpuProviderErrorCode::kObservationFailed:
      return "OBSERVATION_FAILED";
  }
  return "UNKNOWN";
}

}  // namespace yori::gpu
