#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace yori::job {

class JobId final {
 public:
  constexpr JobId() noexcept = default;
  explicit constexpr JobId(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  auto operator<=>(const JobId&) const = default;

 private:
  std::uint64_t value_{0};
};

struct JobSpecLimits final {
  static constexpr std::size_t kMaxArgumentCount = 256;
  static constexpr std::size_t kMaxArgumentBytes = 64 * 1024;
  static constexpr std::size_t kMaxSingleArgumentBytes = 16 * 1024;
  static constexpr std::size_t kMaxWorkingDirectoryBytes = 4096;
  static constexpr std::size_t kMaxEnvironmentVariables = 256;
  static constexpr std::size_t kMaxEnvironmentNameBytes = 255;
  static constexpr std::size_t kMaxEnvironmentValueBytes = 32 * 1024;
  static constexpr std::size_t kMaxEnvironmentBytes = 256 * 1024;
  static constexpr std::size_t kMaxLaunchProfileBytes = 128;
  static constexpr std::size_t kMaxTensorboardLogdirBytes = 4096;
};

struct JobSpec final {
  std::uint32_t owner_uid{0};
  std::uint32_t owner_gid{0};
  std::vector<std::string> argv;
  std::string cwd;
  std::map<std::string, std::string> env;
  std::uint32_t gpu_request{1};
  std::optional<std::string> launch_profile;
  std::optional<std::string> tensorboard_logdir;
  std::chrono::system_clock::time_point submit_time{};
};

enum class JobSpecErrorCode {
  kNone,
  kRootOwnerNotAllowed,
  kEmptyArguments,
  kTooManyArguments,
  kInvalidArgument,
  kArgumentTooLong,
  kArgumentsTooLarge,
  kInvalidWorkingDirectory,
  kTooManyEnvironmentVariables,
  kInvalidEnvironmentName,
  kEnvironmentNameTooLong,
  kEnvironmentValueTooLong,
  kEnvironmentTooLarge,
  kUnsupportedGpuRequest,
  kLaunchProfileTooLong,
  kInvalidTensorboardLogdir,
  kInvalidSubmitTime,
};

struct JobSpecValidationResult final {
  JobSpecErrorCode code{JobSpecErrorCode::kNone};
  std::optional<std::size_t> item_index;

  [[nodiscard]] constexpr bool ok() const noexcept { return code == JobSpecErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

[[nodiscard]] JobSpecValidationResult validate(const JobSpec& spec) noexcept;
[[nodiscard]] const char* to_string(JobSpecErrorCode code) noexcept;

enum class JobState {
  kQueued,
  kStarting,
  kRunning,
  kStopping,
  kFinished,
  kFailed,
  kCancelled,
  kLost,
};

[[nodiscard]] constexpr bool is_terminal(JobState state) noexcept {
  return state == JobState::kFinished || state == JobState::kFailed ||
         state == JobState::kCancelled || state == JobState::kLost;
}

[[nodiscard]] const char* to_string(JobState state) noexcept;
[[nodiscard]] bool can_transition(JobState from, JobState to) noexcept;

enum class TransitionOutcome {
  kApplied,
  kRejected,
  kIdempotentTerminal,
  kIgnoredAfterTerminal,
};

[[nodiscard]] const char* to_string(TransitionOutcome outcome) noexcept;

struct TransitionResult final {
  JobState from;
  JobState requested;
  TransitionOutcome outcome;
  std::uint64_t revision;

  [[nodiscard]] constexpr bool applied() const noexcept {
    return outcome == TransitionOutcome::kApplied;
  }
};

enum class JobCreationErrorCode {
  kNone,
  kInvalidJobId,
  kInvalidSpec,
};

struct JobCreationError final {
  JobCreationErrorCode code{JobCreationErrorCode::kNone};
  JobSpecValidationResult spec_validation{};
};

class Job final {
 public:
  [[nodiscard]] static std::unique_ptr<Job> create(JobId id, JobSpec spec, JobCreationError& error);

  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  Job(Job&&) = delete;
  Job& operator=(Job&&) = delete;
  ~Job() = default;

  [[nodiscard]] JobId id() const noexcept { return id_; }
  [[nodiscard]] const JobSpec& spec() const noexcept { return spec_; }
  [[nodiscard]] JobState state() const noexcept { return state_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

  // 单次调用只执行一次 O(1) 状态推进。所有拒绝、终态重复和迟到结果都通过
  // TransitionResult 显式返回，由上层投递观察事件或持久化。
  [[nodiscard]] TransitionResult transition_to(JobState requested) noexcept;

 private:
  Job(JobId id, JobSpec spec) : id_(id), spec_(std::move(spec)) {}

  JobId id_;
  JobSpec spec_;
  JobState state_{JobState::kQueued};
  std::uint64_t revision_{0};
};

}  // namespace yori::job
