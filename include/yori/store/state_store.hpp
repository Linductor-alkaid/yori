#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/job/job.hpp>

namespace yori::store {

struct StoredJob final {
  job::JobId id;
  job::JobSpec spec;
  job::JobState state{job::JobState::kQueued};
  std::uint64_t revision{0};
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
