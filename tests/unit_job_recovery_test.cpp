// JobRecovery 决策与恢复流程单测（M4-03）：以注入的 IdentityVerifier 覆盖
// 决策矩阵（采纳/提升/保留/LOST 三因/终态不动）、mutation 组装与 lease 释放、
// store/queue 失败显式化；真实进程路径在 integration_recovery_test 覆盖。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testing/in_memory_state_store.hpp"
#include "yori/queue/job_queue.hpp"
#include "yori/recovery/job_recovery.hpp"
#include "yori_test.hpp"

namespace {

using yori::job::JobState;
using yori::recovery::IdentityVerification;
using yori::recovery::RecoveryDecisionCode;
using yori::store::StateMutation;
using yori::store::StateStoreErrorCode;

yori::job::JobSpec spec(std::uint32_t uid) {
  yori::job::JobSpec value;
  value.owner_uid = uid;
  value.owner_gid = uid;
  value.argv = {"train"};
  value.cwd = "/srv/training";
  value.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{uid}};
  return value;
}

yori::store::StoredJob queued(std::uint64_t id, std::uint32_t uid) {
  return {yori::job::JobId{id}, spec(uid), JobState::kQueued, 0, {}};
}

yori::store::StoredJob running(std::uint64_t id, std::uint32_t uid, const char* gpu,
                               yori::process::ProcessIdentity identity) {
  yori::store::StoredJob record{yori::job::JobId{id}, spec(uid), JobState::kRunning, 2, {}};
  record.execution.identity = identity;
  record.execution.start_time = record.spec.submit_time + std::chrono::seconds{30};
  static_cast<void>(gpu);
  return record;
}

yori::process::ProcessIdentity identity(std::int64_t pid) {
  return {pid, pid, static_cast<std::uint64_t>(pid) * 100};
}

const yori::store::StoredJob* find_job(const yori::store::StateSnapshot& snapshot,
                                       std::uint64_t id) {
  for (const auto& job : snapshot.jobs) {
    if (job.id == yori::job::JobId{id}) {
      return &job;
    }
  }
  return nullptr;
}

const yori::store::StoredJob& require_job(const yori::store::StateSnapshot& snapshot,
                                          std::uint64_t id) {
  const auto* found = find_job(snapshot, id);
  if (found == nullptr) {
    std::fprintf(stderr, "required Job %llu is missing\n", static_cast<unsigned long long>(id));
    std::exit(1);
  }
  return *found;
}

const yori::recovery::RecoveryJobOutcome* find_outcome(
    const std::vector<yori::recovery::RecoveryJobOutcome>& outcomes, std::uint64_t id) {
  for (const auto& outcome : outcomes) {
    if (outcome.job_id == yori::job::JobId{id}) {
      return &outcome;
    }
  }
  return nullptr;
}

// 单 PID 可编程核验器：命中的 PID 返回预设结论，其余视为核验通过。
yori::recovery::IdentityVerifier scripted_verifier(std::int64_t pid,
                                                   IdentityVerification verification) {
  return [pid, verification](const yori::process::ProcessIdentity& probe) {
    return probe.pid == pid ? verification : IdentityVerification::kVerified;
  };
}

std::unique_ptr<yori::queue::GlobalJobQueue> make_queue() {
  yori::queue::QueueErrorCode error = yori::queue::QueueErrorCode::kNone;
  auto queue = yori::queue::GlobalJobQueue::create(yori::queue::QueueConfig{64}, error);
  YORI_CHECK(queue != nullptr);
  return queue;
}

}  // namespace

int main() {
  // ---- 决策矩阵（plan 纯决策，不落盘） --------------------------------------
  {
    yori::store::StateSnapshot snapshot;
    snapshot.revision = 6;
    snapshot.jobs = {
        queued(1, 1001),                           // QUEUED -> 入队
        running(2, 1002, "GPU-1", identity(200)),  // 核验通过 -> 保留
        [&] {                                      // STARTING 核验通过 -> RUNNING
          auto record = running(3, 1003, "GPU-2", identity(300));
          record.state = JobState::kStarting;
          record.revision = 1;
          return record;
        }(),
        [&] {  // STOPPING 核验通过 -> 需重发取消
          auto record = running(4, 1004, "GPU-3", identity(400));
          record.state = JobState::kStopping;
          record.revision = 3;
          return record;
        }(),
        running(5, 1005, "GPU-4", identity(500)),  // 进程消失 -> LOST
        running(6, 1006, "GPU-5", identity(600)),  // PID reuse -> LOST
        [&] {                                      // 无身份 -> LOST（崩溃窗口）
          auto record = running(7, 1007, "GPU-6", yori::process::ProcessIdentity{});
          record.execution.identity = yori::process::ProcessIdentity{};
          return record;
        }(),
        [&] {  // 终态 -> 不动
          auto record = running(8, 1008, nullptr, identity(800));
          record.state = JobState::kFinished;
          record.revision = 4;
          return record;
        }(),
    };
    snapshot.leases = {
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-1"}, yori::job::JobId{2}},
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-2"}, yori::job::JobId{3}},
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-3"}, yori::job::JobId{4}},
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-4"}, yori::job::JobId{5}},
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-5"}, yori::job::JobId{6}},
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-6"}, yori::job::JobId{7}},
    };

    // 按 PID 映射预设核验结论：500 -> 进程消失，600 -> PID reuse。
    const std::pair<std::int64_t, IdentityVerification> scripted[] = {
        {500, IdentityVerification::kProcessGone},
        {600, IdentityVerification::kMismatch},
    };
    const auto per_pid = [&scripted](const yori::process::ProcessIdentity& probe) {
      for (const auto& [pid, verification] : scripted) {
        if (probe.pid == pid) {
          return verification;
        }
      }
      return IdentityVerification::kVerified;
    };

    auto queue = make_queue();
    yori::testing::InMemoryStateStore store;
    yori::recovery::JobRecovery recovery(store, *queue, per_pid);

    const auto plan = recovery.plan(snapshot);
    YORI_CHECK(plan.outcomes.size() == 8);

    const auto* outcome_1 = find_outcome(plan.outcomes, 1);
    YORI_CHECK(outcome_1 != nullptr);
    if (outcome_1 != nullptr) {
      YORI_CHECK(outcome_1->decision == RecoveryDecisionCode::kQueuedRestored);
    }
    const auto* outcome_2 = find_outcome(plan.outcomes, 2);
    YORI_CHECK(outcome_2 != nullptr);
    if (outcome_2 != nullptr) {
      YORI_CHECK(outcome_2->decision == RecoveryDecisionCode::kAdoptedRunning);
    }
    const std::pair<std::uint64_t, RecoveryDecisionCode> expected[] = {
        {3, RecoveryDecisionCode::kAdoptedPromotedToRunning},
        {4, RecoveryDecisionCode::kAdoptedStoppingNeedsRecancel},
        {5, RecoveryDecisionCode::kLostProcessGone},
        {6, RecoveryDecisionCode::kLostIdentityMismatch},
        {7, RecoveryDecisionCode::kLostIdentityMissing},
        {8, RecoveryDecisionCode::kTerminalUntouched}};
    for (const auto& [job_id, decision] : expected) {
      const auto* outcome = find_outcome(plan.outcomes, job_id);
      YORI_CHECK(outcome != nullptr);
      if (outcome != nullptr) {
        YORI_CHECK(outcome->decision == decision);
      }
    }
    const auto* outcome_5 = find_outcome(plan.outcomes, 5);
    const auto* outcome_6 = find_outcome(plan.outcomes, 6);

    // LOST 的 lease 释放显式暴露在 outcome 上。
    if (outcome_5 != nullptr && outcome_5->released_gpu.has_value()) {
      YORI_CHECK(outcome_5->released_gpu->value() == "GPU-4");
    } else {
      YORI_CHECK(false);
    }
    if (outcome_6 != nullptr && outcome_6->released_gpu.has_value()) {
      YORI_CHECK(outcome_6->released_gpu->value() == "GPU-5");
    } else {
      YORI_CHECK(false);
    }
    const auto* outcome_7 = find_outcome(plan.outcomes, 7);
    if (outcome_7 != nullptr) {
      YORI_CHECK(outcome_7->released_gpu.has_value());
    } else {
      YORI_CHECK(false);
    }
    if (outcome_2 != nullptr) {
      YORI_CHECK(outcome_2->released_gpu == std::nullopt);
    } else {
      YORI_CHECK(false);
    }

    // mutation 组装：3 个 LOST + 1 个 STARTING 提升。
    YORI_CHECK(plan.mutations.size() == 4);
    std::size_t lost_count = 0;
    std::size_t promoted_count = 0;
    for (const auto& entry : plan.mutations) {
      if (entry.update_job.state == JobState::kLost) {
        ++lost_count;
        YORI_CHECK(entry.update_job.execution.failure_reason.has_value());
        YORI_CHECK(entry.update_job.execution.end_time.has_value());
      }
      if (entry.update_job.state == JobState::kRunning &&
          entry.update_job.id == yori::job::JobId{3}) {
        ++promoted_count;
        YORI_CHECK(entry.release_gpu == std::nullopt);
      }
    }
    YORI_CHECK(lost_count == 3);
    YORI_CHECK(promoted_count == 1);
  }

  // ---- recover()：采纳 + LOST + 队列重建 + 失败注入 --------------------------
  {
    auto store = std::make_unique<yori::testing::InMemoryStateStore>();
    auto queue = make_queue();

    // 构造持久化状态：QUEUED(1) + RUNNING(2, GPU-A, 核验通过) + RUNNING(3,
    // GPU-B, 进程消失)。create 只接受 QUEUED/revision=0，先建立再推进。
    StateMutation create;
    create.create_jobs.push_back(queued(1, 1001));
    create.create_jobs.push_back(queued(2, 1002));
    create.create_jobs.push_back(queued(3, 1003));
    YORI_CHECK(store->apply(create).ok());

    auto snapshot = store->load().snapshot;
    StateMutation advance;
    advance.expected_revision = 1;
    for (const auto& id : {std::uint64_t{2}, std::uint64_t{3}}) {
      auto record = require_job(snapshot, id);
      record.state = JobState::kStarting;  // QUEUED -> STARTING（合法边）
      ++record.revision;
      advance.update_jobs.push_back(std::move(record));
    }
    advance.acquire_leases.push_back(
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-A"}, yori::job::JobId{2}});
    advance.acquire_leases.push_back(
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-B"}, yori::job::JobId{3}});
    YORI_CHECK(store->apply(advance).ok());

    snapshot = store->load().snapshot;
    StateMutation run;
    run.expected_revision = 2;
    for (const auto& [id, pid] :
         std::vector<std::pair<std::uint64_t, std::int64_t>>{{2, 200}, {3, 300}}) {
      auto record = require_job(snapshot, id);
      record.state = JobState::kRunning;  // STARTING -> RUNNING，补写身份
      ++record.revision;
      record.execution.identity = identity(pid);
      record.execution.start_time = record.spec.submit_time;
      run.update_jobs.push_back(std::move(record));
    }
    YORI_CHECK(store->apply(run).ok());

    yori::recovery::JobRecovery recovery(
        *store, *queue, scripted_verifier(300, IdentityVerification::kProcessGone));

    const auto result = recovery.recover();
    YORI_CHECK(result.ok());
    YORI_CHECK(result.outcomes.size() == 3);

    const auto after = store->load().snapshot;
    YORI_CHECK(require_job(after, 1).state == JobState::kQueued);
    YORI_CHECK(require_job(after, 2).state == JobState::kRunning);  // 采纳
    YORI_CHECK(require_job(after, 3).state == JobState::kLost);     // LOST + 释放
    YORI_CHECK(require_job(after, 3).execution.failure_reason.has_value());
    YORI_CHECK(after.leases.size() == 1);
    YORI_CHECK(after.leases.front().gpu_uuid.value() == "GPU-A");

    // QUEUED 重建入队且排序稳定。
    YORI_CHECK(queue->size() == 1);
    if (const auto front = queue->front()) {
      YORI_CHECK(front->job_id == yori::job::JobId{1});
    }

    // 重入幂等：再次 recover 对已终态 Job 不再动作（无新 mutation），成功返回。
    const auto again = recovery.recover();
    YORI_CHECK(again.ok());
    const auto after_again = store->load().snapshot;
    YORI_CHECK(require_job(after_again, 3).state == JobState::kLost);
    YORI_CHECK(after_again.revision == after.revision);
  }

  // ---- recover()：store 与 queue 失败显式 -----------------------------------
  {
    auto store = std::make_unique<yori::testing::InMemoryStateStore>();
    auto queue = make_queue();
    StateMutation create;
    create.create_jobs.push_back(queued(1, 1001));
    YORI_CHECK(store->apply(create).ok());

    yori::recovery::JobRecovery recovery(*store, *queue);
    store->fail_with(StateStoreErrorCode::kBackendUnavailable);
    const auto load_failed = recovery.recover();
    YORI_CHECK(load_failed.code == yori::recovery::RecoveryErrorCode::kStoreLoadFailed);
    YORI_CHECK(load_failed.store_error == StateStoreErrorCode::kBackendUnavailable);
    store->clear_failure();

    // 写失败注入需要恢复产生 mutation：构造一个 RUNNING（进程已消失）Job。
    StateMutation start;
    start.expected_revision = 1;
    auto record = require_job(store->load().snapshot, 1);
    record.state = JobState::kStarting;
    ++record.revision;
    start.update_jobs.push_back(std::move(record));
    start.acquire_leases.push_back(
        yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-X"}, yori::job::JobId{1}});
    YORI_CHECK(store->apply(start).ok());
    auto snapshot = store->load().snapshot;
    StateMutation run;
    run.expected_revision = 2;
    auto running = require_job(snapshot, 1);
    running.state = JobState::kRunning;
    ++running.revision;
    running.execution.identity = identity(999999999);  // 不存在的进程 -> LOST 决策
    run.update_jobs.push_back(std::move(running));
    YORI_CHECK(store->apply(run).ok());

    store->fail_next_apply_with(StateStoreErrorCode::kBackendUnavailable);
    const auto write_failed = recovery.recover();
    YORI_CHECK(write_failed.code == yori::recovery::RecoveryErrorCode::kStoreWriteFailed);
    YORI_CHECK(write_failed.store_error == StateStoreErrorCode::kBackendUnavailable);
    store->clear_failure();
  }

  {
    // 队列容量不足 -> restore 显式失败，结果携带 QueueErrorCode。
    auto store = std::make_unique<yori::testing::InMemoryStateStore>();
    yori::queue::QueueErrorCode queue_error = yori::queue::QueueErrorCode::kNone;
    auto queue = yori::queue::GlobalJobQueue::create(yori::queue::QueueConfig{1}, queue_error);
    YORI_CHECK(queue != nullptr);

    StateMutation create;
    create.create_jobs.push_back(queued(1, 1001));
    create.create_jobs.push_back(queued(2, 1002));
    YORI_CHECK(store->apply(create).ok());

    yori::recovery::JobRecovery recovery(*store, *queue);
    const auto result = recovery.recover();
    YORI_CHECK(result.code == yori::recovery::RecoveryErrorCode::kQueueRestoreFailed);
    YORI_CHECK(result.queue_error == yori::queue::QueueErrorCode::kCapacityExceeded);
  }

  // ---- verify_identity_via_proc：无 /proc 依赖的直接分支 --------------------
  {
    YORI_CHECK(yori::recovery::verify_identity_via_proc(yori::process::ProcessIdentity{}) ==
               IdentityVerification::kMismatch);
    YORI_CHECK(yori::recovery::verify_identity_via_proc(yori::process::ProcessIdentity{
                   999999999, 999999999, 1}) == IdentityVerification::kProcessGone);
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
