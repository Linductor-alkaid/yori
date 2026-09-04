#include <yori/store/state_store.hpp>

namespace yori::store {

const char* to_string(StateStoreErrorCode code) noexcept {
  switch (code) {
    case StateStoreErrorCode::kNone:
      return "NONE";
    case StateStoreErrorCode::kBackendUnavailable:
      return "BACKEND_UNAVAILABLE";
    case StateStoreErrorCode::kRevisionConflict:
      return "REVISION_CONFLICT";
    case StateStoreErrorCode::kInvalidMutation:
      return "INVALID_MUTATION";
    case StateStoreErrorCode::kCapacityExceeded:
      return "CAPACITY_EXCEEDED";
    case StateStoreErrorCode::kInvalidJob:
      return "INVALID_JOB";
    case StateStoreErrorCode::kJobAlreadyExists:
      return "JOB_ALREADY_EXISTS";
    case StateStoreErrorCode::kJobNotFound:
      return "JOB_NOT_FOUND";
    case StateStoreErrorCode::kInvalidJobRevision:
      return "INVALID_JOB_REVISION";
    case StateStoreErrorCode::kJobSpecChanged:
      return "JOB_SPEC_CHANGED";
    case StateStoreErrorCode::kInvalidJobTransition:
      return "INVALID_JOB_TRANSITION";
    case StateStoreErrorCode::kInvalidLease:
      return "INVALID_LEASE";
    case StateStoreErrorCode::kGpuAlreadyLeased:
      return "GPU_ALREADY_LEASED";
    case StateStoreErrorCode::kJobAlreadyLeased:
      return "JOB_ALREADY_LEASED";
    case StateStoreErrorCode::kLeaseNotFound:
      return "LEASE_NOT_FOUND";
  }
  return "UNKNOWN";
}

}  // namespace yori::store
