#include <utility>
#include <yori/job/job.hpp>

namespace yori::job {
namespace {

bool contains_nul(const std::string& value) noexcept {
  return value.find('\0') != std::string::npos;
}

bool add_within_limit(std::size_t value, std::size_t& total, std::size_t limit) noexcept {
  if (value > limit - total) {
    return false;
  }
  total += value;
  return true;
}

bool is_valid_environment_name(const std::string& name) noexcept {
  return !name.empty() && !contains_nul(name) && name.find('=') == std::string::npos;
}

bool is_valid_tensorboard_logdir(const std::string& path) noexcept {
  if (path.empty() || path.size() > JobSpecLimits::kMaxTensorboardLogdirBytes ||
      contains_nul(path) || path.front() == '/') {
    return false;
  }

  std::size_t component_begin = 0;
  while (component_begin <= path.size()) {
    const auto component_end = path.find('/', component_begin);
    const auto component_length =
        (component_end == std::string::npos ? path.size() : component_end) - component_begin;
    if (component_length == 2 && path.compare(component_begin, 2, "..") == 0) {
      return false;
    }
    if (component_end == std::string::npos) {
      break;
    }
    component_begin = component_end + 1;
  }
  return true;
}

bool is_allowed_transition(JobState from, JobState to) noexcept {
  switch (from) {
    case JobState::kQueued:
      return to == JobState::kStarting || to == JobState::kCancelled;
    case JobState::kStarting:
      return to == JobState::kRunning || to == JobState::kStopping || to == JobState::kFailed ||
             to == JobState::kLost;
    case JobState::kRunning:
      return to == JobState::kStopping || to == JobState::kFinished || to == JobState::kFailed ||
             to == JobState::kLost;
    case JobState::kStopping:
      return to == JobState::kCancelled || to == JobState::kFailed || to == JobState::kLost;
    case JobState::kFinished:
    case JobState::kFailed:
    case JobState::kCancelled:
    case JobState::kLost:
      return false;
  }
  return false;
}

}  // namespace

JobSpecValidationResult validate(const JobSpec& spec) noexcept {
  if (spec.owner_uid == 0) {
    return {JobSpecErrorCode::kRootOwnerNotAllowed, std::nullopt};
  }
  if (spec.argv.empty()) {
    return {JobSpecErrorCode::kEmptyArguments, std::nullopt};
  }
  if (spec.argv.size() > JobSpecLimits::kMaxArgumentCount) {
    return {JobSpecErrorCode::kTooManyArguments, std::nullopt};
  }

  std::size_t argument_bytes = 0;
  for (std::size_t index = 0; index < spec.argv.size(); ++index) {
    const auto& argument = spec.argv[index];
    if (contains_nul(argument)) {
      return {JobSpecErrorCode::kInvalidArgument, index};
    }
    if (argument.size() > JobSpecLimits::kMaxSingleArgumentBytes) {
      return {JobSpecErrorCode::kArgumentTooLong, index};
    }
    if (!add_within_limit(argument.size(), argument_bytes, JobSpecLimits::kMaxArgumentBytes)) {
      return {JobSpecErrorCode::kArgumentsTooLarge, index};
    }
  }
  if (spec.argv.front().empty()) {
    return {JobSpecErrorCode::kEmptyArguments, 0};
  }

  if (spec.cwd.empty() || spec.cwd.size() > JobSpecLimits::kMaxWorkingDirectoryBytes ||
      contains_nul(spec.cwd) || spec.cwd.front() != '/') {
    return {JobSpecErrorCode::kInvalidWorkingDirectory, std::nullopt};
  }
  if (spec.env.size() > JobSpecLimits::kMaxEnvironmentVariables) {
    return {JobSpecErrorCode::kTooManyEnvironmentVariables, std::nullopt};
  }

  std::size_t environment_bytes = 0;
  std::size_t environment_index = 0;
  for (const auto& [name, value] : spec.env) {
    if (!is_valid_environment_name(name)) {
      return {JobSpecErrorCode::kInvalidEnvironmentName, environment_index};
    }
    if (name.size() > JobSpecLimits::kMaxEnvironmentNameBytes) {
      return {JobSpecErrorCode::kEnvironmentNameTooLong, environment_index};
    }
    if (value.size() > JobSpecLimits::kMaxEnvironmentValueBytes || contains_nul(value)) {
      return {JobSpecErrorCode::kEnvironmentValueTooLong, environment_index};
    }
    if (!add_within_limit(name.size(), environment_bytes, JobSpecLimits::kMaxEnvironmentBytes) ||
        !add_within_limit(value.size(), environment_bytes, JobSpecLimits::kMaxEnvironmentBytes)) {
      return {JobSpecErrorCode::kEnvironmentTooLarge, environment_index};
    }
    ++environment_index;
  }

  if (spec.gpu_request != 1) {
    return {JobSpecErrorCode::kUnsupportedGpuRequest, std::nullopt};
  }
  if (spec.launch_profile && (spec.launch_profile->empty() ||
                              spec.launch_profile->size() > JobSpecLimits::kMaxLaunchProfileBytes ||
                              contains_nul(*spec.launch_profile))) {
    return {JobSpecErrorCode::kLaunchProfileTooLong, std::nullopt};
  }
  if (spec.tensorboard_logdir && !is_valid_tensorboard_logdir(*spec.tensorboard_logdir)) {
    return {JobSpecErrorCode::kInvalidTensorboardLogdir, std::nullopt};
  }
  if (spec.submit_time.time_since_epoch() <= std::chrono::system_clock::duration::zero()) {
    return {JobSpecErrorCode::kInvalidSubmitTime, std::nullopt};
  }
  return {};
}

const char* to_string(JobSpecErrorCode code) noexcept {
  switch (code) {
    case JobSpecErrorCode::kNone:
      return "NONE";
    case JobSpecErrorCode::kRootOwnerNotAllowed:
      return "ROOT_OWNER_NOT_ALLOWED";
    case JobSpecErrorCode::kEmptyArguments:
      return "EMPTY_ARGUMENTS";
    case JobSpecErrorCode::kTooManyArguments:
      return "TOO_MANY_ARGUMENTS";
    case JobSpecErrorCode::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case JobSpecErrorCode::kArgumentTooLong:
      return "ARGUMENT_TOO_LONG";
    case JobSpecErrorCode::kArgumentsTooLarge:
      return "ARGUMENTS_TOO_LARGE";
    case JobSpecErrorCode::kInvalidWorkingDirectory:
      return "INVALID_WORKING_DIRECTORY";
    case JobSpecErrorCode::kTooManyEnvironmentVariables:
      return "TOO_MANY_ENVIRONMENT_VARIABLES";
    case JobSpecErrorCode::kInvalidEnvironmentName:
      return "INVALID_ENVIRONMENT_NAME";
    case JobSpecErrorCode::kEnvironmentNameTooLong:
      return "ENVIRONMENT_NAME_TOO_LONG";
    case JobSpecErrorCode::kEnvironmentValueTooLong:
      return "ENVIRONMENT_VALUE_TOO_LONG";
    case JobSpecErrorCode::kEnvironmentTooLarge:
      return "ENVIRONMENT_TOO_LARGE";
    case JobSpecErrorCode::kUnsupportedGpuRequest:
      return "UNSUPPORTED_GPU_REQUEST";
    case JobSpecErrorCode::kLaunchProfileTooLong:
      return "LAUNCH_PROFILE_TOO_LONG";
    case JobSpecErrorCode::kInvalidTensorboardLogdir:
      return "INVALID_TENSORBOARD_LOGDIR";
    case JobSpecErrorCode::kInvalidSubmitTime:
      return "INVALID_SUBMIT_TIME";
  }
  return "UNKNOWN";
}

const char* to_string(JobState state) noexcept {
  switch (state) {
    case JobState::kQueued:
      return "QUEUED";
    case JobState::kStarting:
      return "STARTING";
    case JobState::kRunning:
      return "RUNNING";
    case JobState::kStopping:
      return "STOPPING";
    case JobState::kFinished:
      return "FINISHED";
    case JobState::kFailed:
      return "FAILED";
    case JobState::kCancelled:
      return "CANCELLED";
    case JobState::kLost:
      return "LOST";
  }
  return "UNKNOWN";
}

const char* to_string(TransitionOutcome outcome) noexcept {
  switch (outcome) {
    case TransitionOutcome::kApplied:
      return "APPLIED";
    case TransitionOutcome::kRejected:
      return "REJECTED";
    case TransitionOutcome::kIdempotentTerminal:
      return "IDEMPOTENT_TERMINAL";
    case TransitionOutcome::kIgnoredAfterTerminal:
      return "IGNORED_AFTER_TERMINAL";
  }
  return "UNKNOWN";
}

std::unique_ptr<Job> Job::create(JobId id, JobSpec spec, JobCreationError& error) {
  error = {};
  if (!id.valid()) {
    error.code = JobCreationErrorCode::kInvalidJobId;
    return nullptr;
  }

  const auto validation = validate(spec);
  if (!validation) {
    error.code = JobCreationErrorCode::kInvalidSpec;
    error.spec_validation = validation;
    return nullptr;
  }

  return std::unique_ptr<Job>(new Job(id, std::move(spec)));
}

TransitionResult Job::transition_to(JobState requested) noexcept {
  const auto from = state_;
  if (is_terminal(from)) {
    return {from, requested,
            requested == from ? TransitionOutcome::kIdempotentTerminal
                              : TransitionOutcome::kIgnoredAfterTerminal,
            revision_};
  }

  if (!is_allowed_transition(from, requested)) {
    return {from, requested, TransitionOutcome::kRejected, revision_};
  }

  state_ = requested;
  ++revision_;
  return {from, requested, TransitionOutcome::kApplied, revision_};
}

}  // namespace yori::job
