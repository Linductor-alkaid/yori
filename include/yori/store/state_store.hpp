#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/job/job.hpp>
#include <yori/process/process_supervisor.hpp>

namespace yori::store {

// M4 执行记录（设计第 12 节）：持久化 JobSpec 之外的运行期事实。identity 在
// exec 确认后写入（STARTING 期间），是恢复核验依据（RULE-06）；start/end 时间、
// 退出状态与 failure_reason 仅在对应状态合法；log_path 在调度建立日志后写入。
struct JobExecutionRecord final {
  process::ProcessIdentity identity{};
  std::optional<std::chrono::system_clock::time_point> start_time{};
  std::optional<std::chrono::system_clock::time_point> end_time{};
  std::optional<process::ExitStatus> exit{};
  std::optional<std::string> failure_reason{};
  std::optional<std::string> log_path{};

  [[nodiscard]] bool has_identity() const noexcept { return identity.valid(); }
};

struct JobExecutionLimits final {
  static constexpr std::size_t kMaxFailureReasonBytes = 4096;
  static constexpr std::size_t kMaxLogPathBytes = 4096;
};

enum class JobExecutionValidationCode {
  kNone,
  kInvalidIdentity,
  kIdentityNotAllowed,
  kIdentityRequired,
  kExitNotAllowed,
  kStartTimeNotAllowed,
  kInvalidStartTime,
  kEndTimeNotAllowed,
  kInvalidEndTime,
  kFailureReasonNotAllowed,
  kInvalidFailureReason,
  kInvalidLogPath,
  kLogPathNotAllowed,
};

[[nodiscard]] const char* to_string(JobExecutionValidationCode code) noexcept;

struct JobExecutionValidationResult final {
  JobExecutionValidationCode code{JobExecutionValidationCode::kNone};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == JobExecutionValidationCode::kNone;
  }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// 执行记录在给定 Job 状态下的结构校验（M4 规则）：
// - identity：要么全零（未记录），要么三元组有效；QUEUED 禁止携带；
//   RUNNING/STOPPING 必须携带（到达路径必然经过 exec 确认）；STARTING 与终态
//   可选（spawn 前窗口 / 审计事实）。
// - exit：仅终态可携带。
// - start_time：仅 identity 存在时可携带，且不早于 submit_time。
// - end_time：仅终态可携带；与 start_time 同时存在时不早于 start_time。
// - failure_reason：仅终态可携带；1..4096 bytes 且不含 NUL。
// - log_path：QUEUED 禁止；其余状态可选；绝对路径 1..4096 bytes 且不含 NUL。
[[nodiscard]] JobExecutionValidationResult validate_execution(const JobExecutionRecord& execution,
                                                              job::JobState state,
                                                              const job::JobSpec& spec) noexcept;

struct StoredJob final {
  job::JobId id;
  job::JobSpec spec;
  job::JobState state{job::JobState::kQueued};
  std::uint64_t revision{0};
  JobExecutionRecord execution{};
};

struct StateSnapshot final {
  std::uint64_t revision{0};
  std::vector<StoredJob> jobs;
  std::vector<gpu::GpuLease> leases;
};

struct StateMutation final {
  static constexpr std::size_t kMaxEntries = 64;

  std::uint64_t expected_revision{0};
  std::vector<StoredJob> create_jobs;
  std::vector<StoredJob> update_jobs;
  std::vector<gpu::GpuLease> acquire_leases;
  std::vector<gpu::GpuUuid> release_leases;

  [[nodiscard]] std::size_t entry_count() const noexcept {
    if (create_jobs.size() > kMaxEntries) {
      return kMaxEntries + 1;
    }
    std::size_t total = create_jobs.size();
    if (update_jobs.size() > kMaxEntries - total) {
      return kMaxEntries + 1;
    }
    total += update_jobs.size();
    if (acquire_leases.size() > kMaxEntries - total) {
      return kMaxEntries + 1;
    }
    total += acquire_leases.size();
    if (release_leases.size() > kMaxEntries - total) {
      return kMaxEntries + 1;
    }
    return total + release_leases.size();
  }
};

enum class StateStoreErrorCode {
  kNone,
  kBackendUnavailable,
  kRevisionConflict,
  kInvalidMutation,
  kCapacityExceeded,
  kInvalidJob,
  kInvalidExecutionRecord,
  kJobAlreadyExists,
  kJobNotFound,
  kInvalidJobRevision,
  kJobSpecChanged,
  kInvalidJobTransition,
  kInvalidLease,
  kGpuAlreadyLeased,
  kJobAlreadyLeased,
  kLeaseNotFound,
};

[[nodiscard]] const char* to_string(StateStoreErrorCode code) noexcept;

struct StateStoreLoadResult final {
  StateStoreErrorCode code{StateStoreErrorCode::kNone};
  StateSnapshot snapshot;

  [[nodiscard]] constexpr bool ok() const noexcept { return code == StateStoreErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

struct StateStoreWriteResult final {
  StateStoreErrorCode code{StateStoreErrorCode::kNone};
  std::uint64_t revision{0};

  [[nodiscard]] constexpr bool ok() const noexcept { return code == StateStoreErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

class StateStore {
 public:
  virtual ~StateStore() = default;

  // 返回 Job 与 lease 的同一 revision 一致快照。
  [[nodiscard]] virtual StateStoreLoadResult load() = 0;

  // 原子应用一个有界 mutation。expected_revision 不匹配必须明确冲突，任何条目
  // 失败都不得留下部分写入。异步串行化、admission 与 shutdown 由外部 Executor
  // owner 负责，StateStore adapter 不得隐藏线程或写队列。
  [[nodiscard]] virtual StateStoreWriteResult apply(const StateMutation& mutation) = 0;
};

}  // namespace yori::store
