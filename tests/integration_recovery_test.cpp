// daemon 重启恢复集成闭环（M4-03/M4-02，recovery 标签）：SQLite 持久化 ->
// 关闭并重开（模拟 daemon 重启，训练进程存活）-> 身份核验采纳（lease 保留、
// QUEUED 重建）-> 进程消失后再次恢复 -> LOST + lease 释放 ->
// kRecoveryCompleted 触发 FifoScheduler 将释放的 GPU 分配给排队 Job；
// 以及 PID reuse 防护（进程存活但身份不符 -> LOST，绝不接管，RULE-06）。
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "yori/queue/job_queue.hpp"
#include "yori/recovery/job_recovery.hpp"
#include "yori/scheduler/scheduler.hpp"
#include "yori/store/sqlite_state_store.hpp"
#include "yori_test.hpp"

namespace {

using yori::job::JobState;
using yori::store::SqliteStateStore;
using yori::store::SqliteStateStoreConfig;
using yori::store::StateMutation;

std::string make_directory() {
  char pattern[] = "/tmp/yori-recovery-test-XXXXXX";
  char* dir = ::mkdtemp(pattern);
  YORI_CHECK(dir != nullptr);
  return dir;
}

yori::job::JobSpec spec(std::uint32_t uid, std::uint64_t submit_seconds) {
  yori::job::JobSpec value;
  value.owner_uid = uid;
  value.owner_gid = uid;
  value.argv = {"/usr/bin/python3", "train.py"};
  value.cwd = "/tmp";
  value.submit_time = std::chrono::system_clock::time_point{
      std::chrono::seconds{static_cast<long long>(submit_seconds)}};
  return value;
}

yori::store::StoredJob queued(std::uint64_t id, std::uint32_t uid) {
  return {yori::job::JobId{id}, spec(uid, id), JobState::kQueued, 0, {}};
}

// 直接 fork/exec 的长命进程（独立进程组）：测试进程扮演"重启后的 init/新
// daemon 之外的存活训练"。不走 ProcessSupervisor，避免其析构兜底 SIGKILL
// 与"进程必须跨 daemon 存活"的语义冲突。
struct LiveProcess final {
  std::int64_t pid{0};
  yori::process::ProcessIdentity identity{};

  void shutdown() {
    if (pid > 0) {
      static_cast<void>(::kill(static_cast<pid_t>(pid), SIGKILL));
      int status = 0;
      static_cast<void>(::waitpid(static_cast<pid_t>(pid), &status, 0));
      pid = 0;
    }
  }
};

LiveProcess spawn_sleep(std::int64_t seconds) {
  const std::string arg = std::to_string(seconds);
  const pid_t child = ::fork();
  YORI_CHECK(child >= 0);
  if (child == 0) {
    // 子进程：独立进程组 + 静默输出（DEC-008：exec 前 SIGPIPE 忽略）。
    static_cast<void>(::setpgid(0, 0));
    ::signal(SIGPIPE, SIG_IGN);
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      static_cast<void>(::dup2(null_fd, STDOUT_FILENO));
      static_cast<void>(::dup2(null_fd, STDERR_FILENO));
    }
    ::execl("/bin/sleep", "sleep", arg.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  // 双端 setpgid：任一侧先完成都成立，ESRCH/EACCES 竞态可接受。
  static_cast<void>(::setpgid(child, child));

  const auto ticks = yori::process::read_process_start_ticks(child);
  const auto pgid = yori::process::read_process_pgid(child);
  YORI_CHECK(ticks.has_value());
  YORI_CHECK(pgid.has_value());
  return {child, yori::process::ProcessIdentity{child, pgid.value_or(0), ticks.value_or(0)}};
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

yori::store::StoredJob& mutable_job(yori::store::StateSnapshot& snapshot, std::uint64_t id) {
  for (auto& job : snapshot.jobs) {
    if (job.id == yori::job::JobId{id}) {
      return job;
    }
  }
  YORI_CHECK(false);
  return snapshot.jobs.front();
}

SqliteStateStoreConfig config_for(const std::string& path) {
  SqliteStateStoreConfig config;
  config.database_path = path;
  config.max_jobs = 16;
  config.max_leases = 8;
  return config;
}

// 一次性写一个 mutation（expected_revision 取当前 load）。
void apply_mutation(yori::store::StateStore& store, StateMutation mutation) {
  const auto snapshot = store.load();
  YORI_CHECK(snapshot.ok());
  mutation.expected_revision = snapshot.snapshot.revision;
  const auto result = store.apply(mutation);
  if (!result.ok()) {
    std::fprintf(stderr, "apply_mutation failed: %s\n", yori::store::to_string(result.code));
  }
  YORI_CHECK(result.ok());
}

yori::gpu::GpuObservationSnapshot free_gpu_snapshot(std::uint64_t revision,
                                                    std::initializer_list<const char*> uuids) {
  yori::gpu::GpuObservationSnapshot snapshot;
  snapshot.revision = revision;
  snapshot.observed_at = std::chrono::system_clock::now();
  std::uint32_t index = 0;
  for (const char* uuid : uuids) {
    snapshot.devices.push_back(yori::gpu::GpuObservation{
        yori::gpu::GpuUuid{uuid}, index++, yori::gpu::GpuObservedState::kFree, {}});
  }
  return snapshot;
}

const yori::gpu::GpuLease* find_lease(const yori::store::StateSnapshot& snapshot,
                                      const char* uuid) {
  for (const auto& lease : snapshot.leases) {
    if (lease.gpu_uuid.value() == uuid) {
      return &lease;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  const std::string directory = make_directory();
  const std::string database = directory + "/yori.db";

  LiveProcess surviving;    // Job 2 的训练进程：跨"daemon 重启"存活
  LiveProcess second_run;   // Job 1 的训练进程：第二轮恢复时仍存活
  LiveProcess reuse_guard;  // PID reuse 场景：进程存活但身份被篡改

  // ---- 场景 A/B：重启恢复闭环 + LOST + 调度接力 ------------------------------
  {
    // "第一次 daemon 生命周期"：写入 Job 1(QUEUED)/2(RUNNING+GPU-A)/3(QUEUED)。
    {
      SqliteStateStore store{config_for(database)};
      YORI_CHECK(store.open().ok());

      StateMutation create;
      create.create_jobs = {queued(1, 1001), queued(2, 1002), queued(3, 1003)};
      apply_mutation(store, create);

      surviving = spawn_sleep(120);

      auto snapshot = store.load().snapshot;
      StateMutation start;
      auto& job_two = mutable_job(snapshot, 2);
      job_two.state = JobState::kStarting;
      ++job_two.revision;
      start.update_jobs.push_back(job_two);
      start.acquire_leases.push_back(
          yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-A"}, yori::job::JobId{2}});
      apply_mutation(store, start);

      snapshot = store.load().snapshot;
      StateMutation run;
      auto& running_two = mutable_job(snapshot, 2);
      running_two.state = JobState::kRunning;
      ++running_two.revision;
      running_two.execution.identity = surviving.identity;
      running_two.execution.start_time = std::chrono::system_clock::now();
      running_two.execution.log_path = directory + "/job-2.log";
      run.update_jobs.push_back(running_two);
      apply_mutation(store, run);
    }
    // store 已销毁：模拟 daemon 关闭/崩溃，训练进程仍存活（RULE-10 语义）。

    // "重启后的 daemon"：重开同一数据库执行恢复。
    {
      SqliteStateStore store{config_for(database)};
      YORI_CHECK(store.open().ok());

      yori::queue::QueueErrorCode queue_error = yori::queue::QueueErrorCode::kNone;
      auto queue = yori::queue::GlobalJobQueue::create(yori::queue::QueueConfig{64}, queue_error);
      YORI_CHECK(queue != nullptr);

      yori::recovery::JobRecovery recovery(store, *queue);
      const auto result = recovery.recover();
      YORI_CHECK(result.ok());

      const auto snapshot = store.load().snapshot;
      YORI_CHECK(require_job(snapshot, 2).state == JobState::kRunning);  // 采纳
      const auto* kept_lease = find_lease(snapshot, "GPU-A");            // lease 保留
      YORI_CHECK(kept_lease != nullptr);
      if (kept_lease != nullptr) {
        YORI_CHECK(kept_lease->job_id == yori::job::JobId{2});
      }
      YORI_CHECK(queue->size() == 2);  // 1 与 3 重新入队
      if (const auto front = queue->front()) {
        YORI_CHECK(front->job_id == yori::job::JobId{1});
      }

      // 恢复完成后调度：GPU-A 被 lease 占用（观测 FREE 也不可分配，RULE-05），
      // GPU-B 空闲 -> 队首 Job 1 启动。
      yori::scheduler::FifoScheduler scheduler(*queue, store);
      const auto scheduled =
          scheduler.run_once(yori::scheduler::SchedulerTrigger::kRecoveryCompleted,
                             free_gpu_snapshot(1, {"GPU-A", "GPU-B"}));
      YORI_CHECK(scheduled.scheduled());
      YORI_CHECK(scheduled.event.job_id == yori::job::JobId{1});
      YORI_CHECK(scheduled.event.gpu_uuid.has_value());
      YORI_CHECK(scheduled.event.gpu_uuid.value_or(yori::gpu::GpuUuid{}).value() == "GPU-B");

      // Job 1 进入 STARTING 后由"守护层"补写身份（第二轮恢复前）。
      second_run = spawn_sleep(120);
      auto after = store.load().snapshot;
      StateMutation confirm;
      auto& job_one = mutable_job(after, 1);
      job_one.state = JobState::kRunning;
      ++job_one.revision;
      job_one.execution.identity = second_run.identity;
      confirm.update_jobs.push_back(job_one);
      apply_mutation(store, confirm);
    }

    // 训练进程退出（模拟 Job 2 结束但 daemon 错过退出事件）后再次恢复。
    surviving.shutdown();
    {
      SqliteStateStore store{config_for(database)};
      YORI_CHECK(store.open().ok());

      yori::queue::QueueErrorCode queue_error = yori::queue::QueueErrorCode::kNone;
      auto queue = yori::queue::GlobalJobQueue::create(yori::queue::QueueConfig{64}, queue_error);
      YORI_CHECK(queue != nullptr);

      yori::recovery::JobRecovery recovery(store, *queue);
      const auto result = recovery.recover();
      YORI_CHECK(result.ok());

      const auto snapshot = store.load().snapshot;
      YORI_CHECK(require_job(snapshot, 2).state == JobState::kLost);  // 进程消失
      YORI_CHECK(require_job(snapshot, 2).execution.failure_reason.has_value());
      YORI_CHECK(find_lease(snapshot, "GPU-A") == nullptr);              // lease 释放
      YORI_CHECK(require_job(snapshot, 1).state == JobState::kRunning);  // 仍存活 -> 保留

      // 释放后的 GPU-A 可被调度给排队 Job 3。
      yori::scheduler::FifoScheduler scheduler(*queue, store);
      const auto scheduled =
          scheduler.run_once(yori::scheduler::SchedulerTrigger::kRecoveryCompleted,
                             free_gpu_snapshot(2, {"GPU-A", "GPU-B"}));
      YORI_CHECK(scheduled.scheduled());
      YORI_CHECK(scheduled.event.job_id == yori::job::JobId{3});
      YORI_CHECK(scheduled.event.gpu_uuid.has_value());
      YORI_CHECK(scheduled.event.gpu_uuid.value_or(yori::gpu::GpuUuid{}).value() == "GPU-A");
    }
  }

  // ---- 场景 C：PID reuse 防护（RULE-06）--------------------------------------
  {
    reuse_guard = spawn_sleep(120);

    const std::string reuse_database = directory + "/reuse.db";
    {
      SqliteStateStore store{config_for(reuse_database)};
      YORI_CHECK(store.open().ok());

      StateMutation create;
      create.create_jobs.push_back(queued(9, 1009));
      apply_mutation(store, create);

      // 持久化身份的启动 ticks 与真实进程不一致（模拟 PID 被复用后的陈旧记录）。
      auto stale_identity = reuse_guard.identity;
      stale_identity.start_ticks += 1;

      auto snapshot = store.load().snapshot;
      StateMutation start;
      auto& job = mutable_job(snapshot, 9);
      job.state = JobState::kStarting;
      ++job.revision;
      start.update_jobs.push_back(job);
      start.acquire_leases.push_back(
          yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-R"}, yori::job::JobId{9}});
      apply_mutation(store, start);

      snapshot = store.load().snapshot;
      StateMutation run;
      auto& running = mutable_job(snapshot, 9);
      running.state = JobState::kRunning;
      ++running.revision;
      running.execution.identity = stale_identity;
      run.update_jobs.push_back(running);
      apply_mutation(store, run);
    }

    SqliteStateStore store{config_for(reuse_database)};
    YORI_CHECK(store.open().ok());
    yori::queue::QueueErrorCode queue_error = yori::queue::QueueErrorCode::kNone;
    auto queue = yori::queue::GlobalJobQueue::create(yori::queue::QueueConfig{64}, queue_error);
    YORI_CHECK(queue != nullptr);

    yori::recovery::JobRecovery recovery(store, *queue);
    const auto result = recovery.recover();
    YORI_CHECK(result.ok());

    const auto snapshot = store.load().snapshot;
    YORI_CHECK(require_job(snapshot, 9).state == JobState::kLost);
    YORI_CHECK(find_lease(snapshot, "GPU-R") == nullptr);
    // 真实进程未被接管也未被终止：核验失败路径不发送任何信号。
    const auto still_alive = yori::process::read_process_start_ticks(reuse_guard.identity.pid);
    YORI_CHECK(still_alive.has_value());
  }

  second_run.shutdown();
  reuse_guard.shutdown();
  return yori::testing::failure_count == 0 ? 0 : 1;
}
