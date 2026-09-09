#include <chrono>
#include <utility>
#include <yori/recovery/job_recovery.hpp>

namespace yori::recovery {
namespace {

bool is_active(job::JobState state) noexcept {
  return state == job::JobState::kStarting || state == job::JobState::kRunning ||
         state == job::JobState::kStopping;
}

const gpu::GpuUuid* find_lease_gpu(const store::StateSnapshot& snapshot, job::JobId id) noexcept {
  for (const auto& lease : snapshot.leases) {
    if (lease.job_id == id) {
      return &lease.gpu_uuid;
    }
  }
  return nullptr;
}

}  // namespace

const char* to_string(RecoveryDecisionCode code) noexcept {
  switch (code) {
    case RecoveryDecisionCode::kAdoptedRunning:
      return "ADOPTED_RUNNING";
    case RecoveryDecisionCode::kAdoptedPromotedToRunning:
      return "ADOPTED_PROMOTED_TO_RUNNING";
    case RecoveryDecisionCode::kAdoptedStoppingNeedsRecancel:
      return "ADOPTED_STOPPING_NEEDS_RECANCEL";
    case RecoveryDecisionCode::kQueuedRestored:
      return "QUEUED_RESTORED";
    case RecoveryDecisionCode::kLostProcessGone:
      return "LOST_PROCESS_GONE";
    case RecoveryDecisionCode::kLostIdentityMismatch:
      return "LOST_IDENTITY_MISMATCH";
    case RecoveryDecisionCode::kLostIdentityMissing:
      return "LOST_IDENTITY_MISSING";
    case RecoveryDecisionCode::kTerminalUntouched:
      return "TERMINAL_UNTOUCHED";
  }
  return "UNKNOWN";
}

const char* recovery_failure_reason(RecoveryDecisionCode code) noexcept {
  switch (code) {
    case RecoveryDecisionCode::kLostProcessGone:
      return "recovery: process gone before or during daemon restart";
    case RecoveryDecisionCode::kLostIdentityMismatch:
      return "recovery: pid reused by another process (pgid/start ticks mismatch)";
    case RecoveryDecisionCode::kLostIdentityMissing:
      return "recovery: active job without recorded process identity";
    case RecoveryDecisionCode::kAdoptedRunning:
    case RecoveryDecisionCode::kAdoptedPromotedToRunning:
    case RecoveryDecisionCode::kAdoptedStoppingNeedsRecancel:
    case RecoveryDecisionCode::kQueuedRestored:
    case RecoveryDecisionCode::kTerminalUntouched:
      break;
  }
  return "";
}

const char* to_string(IdentityVerification verification) noexcept {
  switch (verification) {
    case IdentityVerification::kVerified:
      return "VERIFIED";
    case IdentityVerification::kProcessGone:
      return "PROCESS_GONE";
    case IdentityVerification::kMismatch:
      return "MISMATCH";
  }
  return "UNKNOWN";
}

const char* to_string(RecoveryErrorCode code) noexcept {
  switch (code) {
    case RecoveryErrorCode::kNone:
      return "NONE";
    case RecoveryErrorCode::kStoreLoadFailed:
      return "STORE_LOAD_FAILED";
    case RecoveryErrorCode::kStoreWriteFailed:
      return "STORE_WRITE_FAILED";
    case RecoveryErrorCode::kQueueRestoreFailed:
      return "QUEUE_RESTORE_FAILED";
  }
  return "UNKNOWN";
}

IdentityVerification verify_identity_via_proc(const process::ProcessIdentity& identity) noexcept {
  if (!identity.valid()) {
    return IdentityVerification::kMismatch;
  }
  const auto live_ticks = process::read_process_start_ticks(identity.pid);
  if (!live_ticks.has_value()) {
    return IdentityVerification::kProcessGone;
  }
  if (*live_ticks != identity.start_ticks) {
    return IdentityVerification::kMismatch;
  }
  const auto live_pgid = process::read_process_pgid(identity.pid);
  if (!live_pgid.has_value()) {
    // ticks 读取成功后立即消失：按进程消失处理，不误报 PID reuse。
    return IdentityVerification::kProcessGone;
  }
  if (*live_pgid != identity.pgid) {
    return IdentityVerification::kMismatch;
  }
  return IdentityVerification::kVerified;
}

JobRecovery::JobRecovery(store::StateStore& store, queue::GlobalJobQueue& queue,
                         IdentityVerifier verifier)
    : store_(store),
      queue_(queue),
      verifier_(verifier ? std::move(verifier) : IdentityVerifier(&verify_identity_via_proc)) {}

RecoveryPlan JobRecovery::plan(const store::StateSnapshot& snapshot) const {
  RecoveryPlan plan_result;
  const auto now = std::chrono::system_clock::now();

  for (const auto& record : snapshot.jobs) {
    RecoveryJobOutcome outcome;
    outcome.job_id = record.id;
    outcome.previous_state = record.state;

    if (!is_active(record.state)) {
      outcome.decision = record.state == job::JobState::kQueued
                             ? RecoveryDecisionCode::kQueuedRestored
                             : RecoveryDecisionCode::kTerminalUntouched;
      plan_result.outcomes.push_back(std::move(outcome));
      continue;
    }

    const gpu::GpuUuid* leased_gpu = find_lease_gpu(snapshot, record.id);

    if (!record.execution.has_identity()) {
      // 崩溃窗口：lease 建立后、身份落盘前退出。绝不能重启或接管（RULE-06）。
      outcome.decision = RecoveryDecisionCode::kLostIdentityMissing;
    } else {
      switch (verifier_(record.execution.identity)) {
        case IdentityVerification::kVerified:
          switch (record.state) {
            case job::JobState::kStarting:
              // exec 已在崩溃前确认（身份存在即表示 spawn 完成），恢复为
              // RUNNING（DEC-008 第 2 条）。
              outcome.decision = RecoveryDecisionCode::kAdoptedPromotedToRunning;
              break;
            case job::JobState::kStopping:
              // 取消中的 Job 保留 STOPPING；宽限截止已丢失，标记需要重发取消
              // （动作由 daemon 守护层执行，M5 总装接入）。
              outcome.decision = RecoveryDecisionCode::kAdoptedStoppingNeedsRecancel;
              break;
            default:
              outcome.decision = RecoveryDecisionCode::kAdoptedRunning;
              break;
          }
          break;
        case IdentityVerification::kProcessGone:
          outcome.decision = RecoveryDecisionCode::kLostProcessGone;
          break;
        case IdentityVerification::kMismatch:
          outcome.decision = RecoveryDecisionCode::kLostIdentityMismatch;
          break;
      }
    }

    if (outcome.decision == RecoveryDecisionCode::kAdoptedPromotedToRunning) {
      store::StoredJob promoted = record;
      promoted.state = job::JobState::kRunning;
      promoted.revision = record.revision + 1;
      plan_result.mutations.push_back({std::move(promoted), std::nullopt});
    } else if (outcome.decision == RecoveryDecisionCode::kLostProcessGone ||
               outcome.decision == RecoveryDecisionCode::kLostIdentityMismatch ||
               outcome.decision == RecoveryDecisionCode::kLostIdentityMissing) {
      store::StoredJob lost = record;
      lost.state = job::JobState::kLost;
      lost.revision = record.revision + 1;
      lost.execution.failure_reason = recovery_failure_reason(outcome.decision);
      lost.execution.end_time = now;
      std::optional<gpu::GpuUuid> release;
      if (leased_gpu != nullptr) {
        outcome.released_gpu = *leased_gpu;
        release = *leased_gpu;
      }
      plan_result.mutations.push_back({std::move(lost), release});
    }

    plan_result.outcomes.push_back(std::move(outcome));
  }

  return plan_result;
}

RecoveryResult JobRecovery::recover() {
  RecoveryResult result;

  const auto loaded = store_.load();
  if (!loaded.ok()) {
    result.code = RecoveryErrorCode::kStoreLoadFailed;
    result.store_error = loaded.code;
    return result;
  }

  const auto recovery_plan = plan(loaded.snapshot);
  result.outcomes = recovery_plan.outcomes;

  // 每个 LOST Job 的状态转换与其 lease 释放必须位于同一 mutation（lease 矩阵：
  // LOST 不得持有 lease）；按条目上限分块，块间以递增后的 revision 链接。部分
  // 分块已落盘后失败时，已推进的终态保持幂等，可安全重入 recover()。
  store::StateMutation chunk;
  std::uint64_t expected_revision = loaded.snapshot.revision;

  const auto flush_chunk = [&]() -> bool {
    if (chunk.entry_count() == 0) {
      return true;
    }
    chunk.expected_revision = expected_revision;
    const auto written = store_.apply(chunk);
    if (!written.ok()) {
      result.code = RecoveryErrorCode::kStoreWriteFailed;
      result.store_error = written.code;
      result.store_revision = written.revision;
      return false;
    }
    expected_revision = written.revision;
    chunk = store::StateMutation{};
    return true;
  };

  for (const auto& entry : recovery_plan.mutations) {
    const std::size_t entries_needed = entry.release_gpu.has_value() ? 2 : 1;
    if (chunk.entry_count() + entries_needed > store::StateMutation::kMaxEntries) {
      if (!flush_chunk()) {
        return result;
      }
    }
    chunk.update_jobs.push_back(entry.update_job);
    if (entry.release_gpu.has_value()) {
      chunk.release_leases.push_back(*entry.release_gpu);
    }
  }
  if (!flush_chunk()) {
    return result;
  }

  const auto restored = queue_.restore(loaded.snapshot);
  if (!restored.ok()) {
    result.code = RecoveryErrorCode::kQueueRestoreFailed;
    result.queue_error = restored.code;
    return result;
  }

  result.code = RecoveryErrorCode::kNone;
  result.store_revision = expected_revision;
  return result;
}

}  // namespace yori::recovery
