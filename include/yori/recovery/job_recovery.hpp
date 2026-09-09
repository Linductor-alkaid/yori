#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <yori/process/process_supervisor.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/store/state_store.hpp>

namespace yori::recovery {

// 单个 Job 的恢复决策（设计第 6.2 节，RULE-06）。LOST 各原因区分进程消失、
// 身份不符（PID reuse 防护）与身份缺失（崩溃窗口），供审计与事件投递。
enum class RecoveryDecisionCode {
  kAdoptedRunning,
  kAdoptedPromotedToRunning,
  kAdoptedStoppingNeedsRecancel,
  kQueuedRestored,
  kLostProcessGone,
  kLostIdentityMismatch,
  kLostIdentityMissing,
  kTerminalUntouched,
};

[[nodiscard]] const char* to_string(RecoveryDecisionCode code) noexcept;

// 恢复核验失败原因（与 RecoveryDecisionCode 的 LOST 族对应的稳定字符串）。
[[nodiscard]] const char* recovery_failure_reason(RecoveryDecisionCode code) noexcept;

struct RecoveryJobOutcome final {
  job::JobId job_id{0};
  job::JobState previous_state{job::JobState::kQueued};
  RecoveryDecisionCode decision{RecoveryDecisionCode::kQueuedRestored};
  std::optional<gpu::GpuUuid> released_gpu{};
};

// 身份核验三态结论：通过 / 进程不存在（含解析失败）/ PID 存在但 PGID 或启动
// ticks 不符（PID reuse 防护，不得接管）。
enum class IdentityVerification {
  kVerified,
  kProcessGone,
  kMismatch,
};

[[nodiscard]] const char* to_string(IdentityVerification verification) noexcept;

// 身份核验入口。生产实现为 /proc 比对 PGID 与启动 ticks；测试注入用。
using IdentityVerifier = std::function<IdentityVerification(const process::ProcessIdentity&)>;

[[nodiscard]] IdentityVerification verify_identity_via_proc(
    const process::ProcessIdentity& identity) noexcept;

// 恢复决策计划：不触碰 StateStore 与队列，可独立审计/测试。每个 mutation
// 条目携带一个 Job 的写后副本（LOST / STARTING->RUNNING）及其需要在同一
// mutation 内释放的 lease（lease 矩阵：LOST 不得持有 lease）。
struct RecoveryMutationEntry final {
  store::StoredJob update_job{};
  std::optional<gpu::GpuUuid> release_gpu{};
};

struct RecoveryPlan final {
  std::vector<RecoveryJobOutcome> outcomes;
  std::vector<RecoveryMutationEntry> mutations;
};

enum class RecoveryErrorCode {
  kNone,
  kStoreLoadFailed,
  kStoreWriteFailed,
  kQueueRestoreFailed,
};

[[nodiscard]] const char* to_string(RecoveryErrorCode code) noexcept;

struct RecoveryResult final {
  RecoveryErrorCode code{RecoveryErrorCode::kNone};
  std::vector<RecoveryJobOutcome> outcomes;
  store::StateStoreErrorCode store_error{store::StateStoreErrorCode::kNone};
  queue::QueueErrorCode queue_error{queue::QueueErrorCode::kNone};
  std::uint64_t store_revision{0};

  [[nodiscard]] constexpr bool ok() const noexcept { return code == RecoveryErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// daemon 重启恢复的同步 Core 服务（设计第 6.2 节、RULE-06）：单 owner 调用，
// 无内部线程、定时器或队列；Executor 承载与启动编排（EXEC-09 PhaseGate）由
// daemon 总装负责。绝不重新启动数据库中的 RUNNING/STARTING Job；无法核验
// 身份的活动 Job 一律转 LOST 并释放 lease。
class JobRecovery final {
 public:
  JobRecovery(store::StateStore& store, queue::GlobalJobQueue& queue,
              IdentityVerifier verifier = &verify_identity_via_proc);

  JobRecovery(const JobRecovery&) = delete;
  JobRecovery& operator=(const JobRecovery&) = delete;
  JobRecovery(JobRecovery&&) = delete;
  JobRecovery& operator=(JobRecovery&&) = delete;
  ~JobRecovery() = default;

  // 纯决策：对快照中每个 Job 产出采纳/丢失/保留结论与写后副本，不落盘。
  [[nodiscard]] RecoveryPlan plan(const store::StateSnapshot& snapshot) const;

  // 完整恢复：load() -> plan() -> 分块原子 mutation（LOST + lease 释放 +
  // STARTING 提升）-> GlobalJobQueue::restore()。任一步失败显式返回，不静默
  // 重试；已成功的 mutation 分块保持落盘（终态幂等，可安全重入恢复）。
  [[nodiscard]] RecoveryResult recover();

 private:
  store::StateStore& store_;
  queue::GlobalJobQueue& queue_;
  IdentityVerifier verifier_;
};

}  // namespace yori::recovery
