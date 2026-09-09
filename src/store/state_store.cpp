#include <yori/store/state_store.hpp>

namespace yori::store {
namespace {

bool contains_nul(const std::string& value) noexcept {
  return value.find('\0') != std::string::npos;
}

bool identity_is_blank(const process::ProcessIdentity& identity) noexcept {
  return identity.pid == 0 && identity.pgid == 0 && identity.start_ticks == 0;
}

}  // namespace

const char* to_string(JobExecutionValidationCode code) noexcept {
  switch (code) {
    case JobExecutionValidationCode::kNone:
      return "NONE";
    case JobExecutionValidationCode::kInvalidIdentity:
      return "INVALID_IDENTITY";
    case JobExecutionValidationCode::kIdentityNotAllowed:
      return "IDENTITY_NOT_ALLOWED";
    case JobExecutionValidationCode::kIdentityRequired:
      return "IDENTITY_REQUIRED";
    case JobExecutionValidationCode::kExitNotAllowed:
      return "EXIT_NOT_ALLOWED";
    case JobExecutionValidationCode::kStartTimeNotAllowed:
      return "START_TIME_NOT_ALLOWED";
    case JobExecutionValidationCode::kInvalidStartTime:
      return "INVALID_START_TIME";
    case JobExecutionValidationCode::kEndTimeNotAllowed:
      return "END_TIME_NOT_ALLOWED";
    case JobExecutionValidationCode::kInvalidEndTime:
      return "INVALID_END_TIME";
    case JobExecutionValidationCode::kFailureReasonNotAllowed:
      return "FAILURE_REASON_NOT_ALLOWED";
    case JobExecutionValidationCode::kInvalidFailureReason:
      return "INVALID_FAILURE_REASON";
    case JobExecutionValidationCode::kInvalidLogPath:
      return "INVALID_LOG_PATH";
    case JobExecutionValidationCode::kLogPathNotAllowed:
      return "LOG_PATH_NOT_ALLOWED";
  }
  return "UNKNOWN";
}

JobExecutionValidationResult validate_execution(const JobExecutionRecord& execution,
                                                job::JobState state,
                                                const job::JobSpec& spec) noexcept {
  using Code = JobExecutionValidationCode;

  if (!identity_is_blank(execution.identity) && !execution.identity.valid()) {
    return {Code::kInvalidIdentity};
  }
  const bool has_identity = execution.has_identity();
  if (has_identity && state == job::JobState::kQueued) {
    return {Code::kIdentityNotAllowed};
  }
  if (!has_identity && (state == job::JobState::kRunning || state == job::JobState::kStopping)) {
    return {Code::kIdentityRequired};
  }

  if (execution.exit.has_value() && !job::is_terminal(state)) {
    return {Code::kExitNotAllowed};
  }

  if (execution.start_time.has_value()) {
    if (!has_identity) {
      return {Code::kStartTimeNotAllowed};
    }
    if (*execution.start_time < spec.submit_time) {
      return {Code::kInvalidStartTime};
    }
  }

  if (execution.end_time.has_value()) {
    if (!job::is_terminal(state)) {
      return {Code::kEndTimeNotAllowed};
    }
    if (execution.start_time.has_value() && *execution.end_time < *execution.start_time) {
      return {Code::kInvalidEndTime};
    }
  }

  if (execution.failure_reason.has_value()) {
    if (!job::is_terminal(state)) {
      return {Code::kFailureReasonNotAllowed};
    }
    const auto& reason = *execution.failure_reason;
    if (reason.empty() || reason.size() > JobExecutionLimits::kMaxFailureReasonBytes ||
        contains_nul(reason)) {
      return {Code::kInvalidFailureReason};
    }
  }

  if (execution.log_path.has_value()) {
    const auto& path = *execution.log_path;
    if (state == job::JobState::kQueued) {
      return {Code::kLogPathNotAllowed};
    }
    if (path.empty() || path.size() > JobExecutionLimits::kMaxLogPathBytes || contains_nul(path) ||
        path.front() != '/') {
      return {Code::kInvalidLogPath};
    }
  }

  return {Code::kNone};
}

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
    case StateStoreErrorCode::kInvalidExecutionRecord:
      return "INVALID_EXECUTION_RECORD";
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
