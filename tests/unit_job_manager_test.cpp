#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <yori/launch/launch_adapter.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/recovery/job_recovery.hpp>

#include "process_test_support.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/gpu_manager.hpp"
#include "runtime/job_manager.hpp"
#include "runtime/log_streamer.hpp"
#include "runtime/serial_state_store.hpp"
#include "gpu_test_support.hpp"
#include "testing/fake_gpu_provider.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

// M7-03：JobManager 守护承载（EXEC-06/07/08 收口）。
// 六场景（正常完成、任务异常、提交拒绝、执行中取消、宽限超时升级、shutdown）
// + Yori 特有（FIFO 链式、GPU 事件触发、启动失败收敛、恢复采纳与 STOPPING
// 重取消）。
namespace {

using namespace yori;
using namespace yori::queue;
using namespace yori::runtime;
using namespace std::chrono_literals;

std::string make_root() {
  char pattern[] = "/tmp/yori-job-manager-test-XXXXXX";
  char* directory = ::mkdtemp(pattern);
  YORI_CHECK(directory != nullptr);
  return directory;
}

void set_single_gpu(testing::FakeGpuProvider& provider, yori::gpu::GpuObservedState state) {
  yori::gpu::GpuObservationSnapshot snapshot;
  snapshot.observed_at = std::chrono::system_clock::now();
  yori::gpu::GpuObservation observation;
  observation.uuid = yori::gpu::GpuUuid{"GPU-jm"};
  observation.index = 0;
  observation.state = state;
  snapshot.devices = {observation};
  YORI_CHECK(provider.replace_observations(snapshot.devices, snapshot.observed_at).ok());
}

job::JobSpec spec_for(std::vector<std::string> argv) {
  job::JobSpec spec;
  spec.owner_uid = static_cast<std::uint32_t>(::geteuid());
  spec.owner_gid = static_cast<std::uint32_t>(::getegid());
  spec.argv = std::move(argv);
  spec.cwd = "/tmp";
  spec.submit_time = std::chrono::system_clock::now();
  return spec;
}

// 按值返回：load() 的快照是临时结果，返回其内部指针会在析构后悬空。
std::optional<store::StoredJob> find_stored_simple(store::StateStore& store, std::uint64_t id) {
  const auto load = store.load();
  for (const auto& record : load.snapshot.jobs) {
    if (record.id.value() == id) {
      return record;
    }
  }
  return std::nullopt;
}

const store::StoredJob* find_stored(store::StateStore& store, std::uint64_t id,
                                    store::StateSnapshot& snapshot) {
  const auto load = store.load();
  snapshot = load.snapshot;
  for (const auto& record : snapshot.jobs) {
    if (record.id.value() == id) {
      return &record;
    }
  }
  return nullptr;
}

bool wait_state(store::StateStore& store, std::uint64_t id, job::JobState state,
                std::chrono::milliseconds timeout = 15s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    store::StateSnapshot snapshot;
    const store::StoredJob* record = find_stored(store, id, snapshot);
    if (record != nullptr && record->state == state) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

std::string read_log(const std::string& root, std::uint64_t id, const char* name) {
  std::FILE* file = std::fopen((root + "/" + std::to_string(id) + "/" + name).c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::string content;
  char buffer[4096];
  std::size_t received = 0;
  while ((received = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    content.append(buffer, received);
  }
  std::fclose(file);
  return content;
}

// 终态落盘先于日志泵排空（EOF 延迟发布保证流完整）：文件读取需短暂等待。
std::string wait_log(const std::string& root, std::uint64_t id, const char* name,
                     const std::string& expect) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::string content;
  while (std::chrono::steady_clock::now() < deadline) {
    content = read_log(root, id, name);
    if (content == expect) {
      return content;
    }
    std::this_thread::sleep_for(20ms);
  }
  return content;
}

// 总装夹具：Executor + Fake GPU + SerialStateStore + 队列 + PhaseGate + 真实
// 身份解析与 LaunchAdapter。manager() 在夹具构造完成后创建（GpuManager 需先
// 启动）。
class ManagerFixture final {
 public:
  explicit ManagerFixture(std::chrono::milliseconds grace = 400ms,
                          std::chrono::milliseconds sample_period = 100ms,
                          std::size_t queue_capacity = 64,
                          gpu::GpuObservedState initial_state = gpu::GpuObservedState::kFree) {
    std::string error;
    ExecutorRuntimeConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    YORI_CHECK(runtime.initialize(config, error));
    // FakeGpuProvider 仅在 GpuManager 启动前配置（tick 期间非线程安全）；
    // 运行期状态翻转场景使用 AtomicGpuProvider。
    set_single_gpu(provider, initial_state);

    queue::QueueConfig queue_config;
    queue_config.capacity = queue_capacity;
    queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone;
    queue = GlobalJobQueue::create(queue_config, queue_error);
    YORI_CHECK(queue != nullptr);

    gpu_manager = std::make_unique<runtime::GpuManager>(runtime.executor(), provider,
                                                        runtime::GpuManagerConfig{sample_period});
    YORI_CHECK(gpu_manager->start().ok());

    // EXEC-09 启动编排：恢复与 GPU 观察就绪后开启调度（daemon 总装中的
    // PhaseGate 推进）。
    static_cast<void>(gate.advance_to(runtime::kPhaseSchedulingOpen));

    log_root = make_root();
    runtime::JobManagerConfig manager_config;
    manager_config.log_root = log_root;
    manager_config.cancel_grace = grace;
    manager_config.daemon_environment = {{"PATH", "/usr/bin:/bin"}};
    manager = std::make_unique<runtime::JobManager>(
        runtime.executor(), *store, *queue, *gpu_manager, streamer, resolver, adapter, gate,
        manager_config);
    YORI_CHECK(manager->start().ok());
  }

  ~ManagerFixture() {
    if (manager) {
      static_cast<void>(manager->stop());
    }
    if (gpu_manager) {
      static_cast<void>(gpu_manager->stop());
    }
    static_cast<void>(runtime.shutdown());
  }

  runtime::ExecutorRuntime runtime;
  testing::FakeGpuProvider provider;
  std::unique_ptr<store::StateStore> store =
      std::make_unique<runtime::SerialStateStore>(
          std::make_unique<testing::InMemoryStateStore>());
  std::unique_ptr<GlobalJobQueue> queue;
  std::unique_ptr<runtime::GpuManager> gpu_manager;
  runtime::LogStreamer streamer;
  launch::PosixIdentityResolver resolver;
  launch::DefaultLaunchAdapter adapter;
  executor::comm::PhaseGate gate{"jm-test"};
  std::string log_root;
  std::unique_ptr<runtime::JobManager> manager;
};

}  // namespace

int main() {
  // ---- 场景 A：正常完成（RUNNING -> FINISHED + lease 释放 + 日志落盘）----
  {
    ManagerFixture fixture;
    store::StateStore& store = *fixture.store;
    const auto submit = fixture.manager->submit_job(
        spec_for({"/bin/sh", "-c", "echo jm-stdout; echo jm-stderr 1>&2"}));
    YORI_CHECK(submit.ok());
    YORI_CHECK(wait_state(store, submit.job_id, job::JobState::kFinished));

    store::StateSnapshot snapshot;
    const store::StoredJob* record = find_stored(store, submit.job_id, snapshot);
    YORI_CHECK(record != nullptr);
    if (record != nullptr) {
      YORI_CHECK(record->execution.identity.valid());
      YORI_CHECK(record->execution.exit && record->execution.exit->is_success());
      YORI_CHECK(record->execution.log_path &&
                 *record->execution.log_path == fixture.log_root + "/" +
                                                    std::to_string(submit.job_id));
      YORI_CHECK(snapshot.leases.empty());  // lease 已释放
    }
    // 日志落盘（两路）。
    YORI_CHECK(wait_log(fixture.log_root, submit.job_id, "stdout.log", "jm-stdout\n") ==
               "jm-stdout\n");
    YORI_CHECK(wait_log(fixture.log_root, submit.job_id, "stderr.log", "jm-stderr\n") ==
               "jm-stderr\n");
    const auto stats = fixture.manager->stats();
    YORI_CHECK(stats.jobs_submitted == 1 && stats.jobs_launched == 1 &&
               stats.jobs_finished == 1 && stats.scheduler_scheduled == 1);
  }

  // ---- 场景 B：任务异常（非零退出 -> FAILED + 原因记录 + lease 释放）------
  {
    ManagerFixture fixture;
    store::StateStore& store = *fixture.store;
    const auto submit =
        fixture.manager->submit_job(spec_for({"/bin/sh", "-c", "exit 3"}));
    YORI_CHECK(submit.ok());
    YORI_CHECK(wait_state(store, submit.job_id, job::JobState::kFailed));
    store::StateSnapshot snapshot;
    const store::StoredJob* record = find_stored(store, submit.job_id, snapshot);
    YORI_CHECK(record != nullptr && snapshot.leases.empty());
    if (record != nullptr) {
      YORI_CHECK(record->execution.exit && record->execution.exit->exit_code == 3);
      YORI_CHECK(record->execution.failure_reason &&
                 record->execution.failure_reason->find("code 3") != std::string::npos);
    }
  }

  // ---- 场景 C：提交拒绝（队列容量 -> 回滚 CANCELLED 审计事实）-------------
  {
    // GPU 初始即 EXTERNAL_BUSY：Job 保持 QUEUED（不离开队列），容量 1 触发拒绝。
    ManagerFixture fixture(400ms, 100ms, 1, gpu::GpuObservedState::kExternalBusy);
    store::StateStore& store = *fixture.store;
    const auto first = fixture.manager->submit_job(spec_for({"true"}));
    YORI_CHECK(first.ok() && first.job_id == 1);
    const auto second = fixture.manager->submit_job(spec_for({"true"}));
    YORI_CHECK(second.code == ipc::JobSubmitOutcome::Code::kQueueRejected);
    const auto load = store.load();
    YORI_CHECK(load.snapshot.jobs.size() == 2);
    YORI_CHECK(load.snapshot.jobs[1].state == job::JobState::kCancelled &&
               load.snapshot.jobs[1].revision == 1);
    YORI_CHECK(load.snapshot.jobs[0].state == job::JobState::kQueued);
  }

  // ---- 场景 D：执行中取消（STOPPING ack -> SIGTERM -> CANCELLED）----------
  {
    ManagerFixture fixture;
    store::StateStore& store = *fixture.store;
    const auto submit = fixture.manager->submit_job(spec_for({"sleep", "30"}));
    YORI_CHECK(submit.ok());
    YORI_CHECK(wait_state(store, submit.job_id, job::JobState::kRunning));

    const auto cancel = fixture.manager->cancel_job(submit.job_id);
    YORI_CHECK(cancel.code == ipc::JobCancelOutcome::Code::kStopping);
    YORI_CHECK(cancel.state == static_cast<std::uint8_t>(job::JobState::kStopping));
    YORI_CHECK(wait_state(store, submit.job_id, job::JobState::kCancelled));

    store::StateSnapshot snapshot;
    const store::StoredJob* record = find_stored(store, submit.job_id, snapshot);
    YORI_CHECK(record != nullptr && snapshot.leases.empty());
    if (record != nullptr) {
      YORI_CHECK(record->execution.exit && record->execution.exit->signaled() &&
                 record->execution.exit->signal_number == SIGTERM);
    }
  }

  // ---- 场景 E：宽限超时升级（忽略 SIGTERM 的进程被 SIGKILL）---------------
  {
    ManagerFixture fixture;
    store::StateStore& store = *fixture.store;
    const auto submit = fixture.manager->submit_job(
        spec_for({"/bin/sh", "-c", "trap '' TERM; echo armed; sleep 300"}));
    YORI_CHECK(submit.ok());
    YORI_CHECK(wait_state(store, submit.job_id, job::JobState::kRunning));
    // 等 trap 安装（exec 确认 != trap 已执行）。
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (read_log(fixture.log_root, submit.job_id, "stdout.log").find("armed") ==
               std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(50ms);
    }

    YORI_CHECK(fixture.manager->cancel_job(submit.job_id).code ==
               ipc::JobCancelOutcome::Code::kStopping);
    YORI_CHECK(wait_state(store, submit.job_id, job::JobState::kCancelled, 20s));
    store::StateSnapshot snapshot;
    const store::StoredJob* record = find_stored(store, submit.job_id, snapshot);
    YORI_CHECK(record != nullptr && snapshot.leases.empty());
    if (record != nullptr) {
      YORI_CHECK(record->execution.exit && record->execution.exit->signal_number == SIGKILL);
    }
  }

  // ---- 场景 F：shutdown（运行中进程 abandon，RULE-10：不终止训练）---------
  {
    process::ProcessIdentity identity{};
    {
      ManagerFixture fixture;
      const auto submit = fixture.manager->submit_job(spec_for({"sleep", "30"}));
      YORI_CHECK(submit.ok());
      YORI_CHECK(wait_state(*fixture.store, submit.job_id, job::JobState::kRunning));
      store::StateSnapshot snapshot;
      const store::StoredJob* record = find_stored(*fixture.store, submit.job_id, snapshot);
      YORI_CHECK(record != nullptr);
      if (record != nullptr) {
        identity = record->execution.identity;
      }
      static_cast<void>(submit.job_id);
      YORI_CHECK(fixture.manager->stop() == runtime::JobManagerStopCode::kStopped);
      const auto stats = fixture.manager->stats();
      YORI_CHECK(stats.abandoned_at_stop == 1);
    }
    // manager 停止后进程仍存活（abandon 未发信号）且身份不变。
    YORI_CHECK(process::verify_process_identity(identity));
    // 清理：终止并回收测试进程。
    static_cast<void>(::kill(-static_cast<pid_t>(identity.pgid), SIGKILL));
    int raw_status = 0;
    while (::waitpid(static_cast<pid_t>(identity.pid), &raw_status, 0) < 0 && errno == EINTR) {
    }
  }

  // ---- Yori 特有 1：FIFO 链式（前一 Job 释放 GPU -> 队首自动启动）---------
  {
    ManagerFixture fixture;
    store::StateStore& store = *fixture.store;
    const auto first = fixture.manager->submit_job(spec_for({"sleep", "1"}));
    YORI_CHECK(first.ok());
    const auto second = fixture.manager->submit_job(spec_for({"true"}));
    YORI_CHECK(second.ok());
    // 第二个 Job 在唯一 GPU 被占用时保持 QUEUED。
    YORI_CHECK(wait_state(store, first.job_id, job::JobState::kRunning));
    store::StateSnapshot snapshot;
    const store::StoredJob* queued = find_stored(store, second.job_id, snapshot);
    YORI_CHECK(queued != nullptr && queued->state == job::JobState::kQueued);

    YORI_CHECK(wait_state(store, first.job_id, job::JobState::kFinished));
    YORI_CHECK(wait_state(store, second.job_id, job::JobState::kFinished));
    const auto stats = fixture.manager->stats();
    YORI_CHECK(stats.scheduler_scheduled == 2);
  }

  // ---- Yori 特有 2：GPU 状态事件触发（EXTERNAL_BUSY -> FREE -> 调度）------
  {
    // 运行期状态翻转用 AtomicGpuProvider（FakeGpuProvider 的整体替换仅限
    // 启动前配置，tick 期间非线程安全）。
    ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize({}, error));
    testing::AtomicGpuProvider provider;
    provider.set_uuid(0, "GPU-jm-event");
    provider.set_present(0, true);
    provider.set_state(0, gpu::GpuObservedState::kExternalBusy);

    auto store = std::make_unique<runtime::SerialStateStore>(
        std::make_unique<testing::InMemoryStateStore>());
    queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone;
    auto queue = GlobalJobQueue::create({}, queue_error);
    runtime::GpuManager gpu_manager(runtime.executor(), provider,
                                    runtime::GpuManagerConfig{100ms});
    YORI_CHECK(gpu_manager.start().ok());
    runtime::LogStreamer streamer;
    launch::PosixIdentityResolver resolver;
    launch::DefaultLaunchAdapter adapter;
    executor::comm::PhaseGate gate{"jm-gpu-event"};
    runtime::JobManagerConfig manager_config;
    manager_config.log_root = make_root();
    manager_config.daemon_environment = {{"PATH", "/usr/bin:/bin"}};
    runtime::JobManager manager(runtime.executor(), *store, *queue, gpu_manager, streamer,
                                resolver, adapter, gate, manager_config);
    static_cast<void>(gate.advance_to(runtime::kPhaseSchedulingOpen));
    YORI_CHECK(manager.start().ok());

    const auto submit = manager.submit_job(spec_for({"true"}));
    YORI_CHECK(submit.ok());
    // 无空闲 GPU：保持 QUEUED。
    std::this_thread::sleep_for(400ms);
    {
      const auto record = find_stored_simple(*store, submit.job_id);
      YORI_CHECK(record.has_value() && record->state == job::JobState::kQueued);
    }

    // GPU 空闲 -> 观测状态迁移事件 -> 调度触发。
    provider.set_state(0, gpu::GpuObservedState::kFree);
    YORI_CHECK(wait_state(*store, submit.job_id, job::JobState::kFinished));

    static_cast<void>(manager.stop());
    static_cast<void>(gpu_manager.stop());
    YORI_CHECK(runtime.shutdown() == runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // ---- Yori 特有 3：启动失败收敛（不存在的二进制 -> FAILED + lease 释放）--
  {
    ManagerFixture fixture;
    store::StateStore& store = *fixture.store;
    const auto submit = fixture.manager->submit_job(
        spec_for({"/nonexistent-training-binary", "--epochs", "1"}));
    YORI_CHECK(submit.ok());
    YORI_CHECK(wait_state(store, submit.job_id, job::JobState::kFailed));
    store::StateSnapshot snapshot;
    const store::StoredJob* record = find_stored(store, submit.job_id, snapshot);
    YORI_CHECK(record != nullptr && snapshot.leases.empty());
    if (record != nullptr) {
      YORI_CHECK(record->execution.failure_reason &&
                 record->execution.failure_reason->find("spawn failed") != std::string::npos);
    }
    const auto stats = fixture.manager->stats();
    YORI_CHECK(stats.launch_failures == 1);
    // 失败释放的 GPU 可被下一个 Job 使用。
    const auto next = fixture.manager->submit_job(spec_for({"true"}));
    YORI_CHECK(next.ok());
    YORI_CHECK(wait_state(store, next.job_id, job::JobState::kFinished));
  }

  // ---- Yori 特有 4：恢复采纳 + STOPPING 重取消（守护重启语义）-------------
  {
    // 在夹具外构造，控制 start(recovery) 时序。
    ExecutorRuntime runtime;
    std::string error;
    ExecutorRuntimeConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    YORI_CHECK(runtime.initialize(config, error));

    testing::FakeGpuProvider provider;
    set_single_gpu(provider, gpu::GpuObservedState::kFree);
    auto store = std::make_unique<runtime::SerialStateStore>(
        std::make_unique<testing::InMemoryStateStore>());

    // 直接构造一个 RUNNING 事实：spawn 真实进程（模拟 daemon 崩溃前启动的
    // 训练），写入 store（RUNNING + identity + lease）。
    process::ProcessSupervisor holder;
    const auto spawned =
        holder.spawn(testing::self_plan({"/bin/sh", "-c", "trap '' TERM; sleep 120"}));
    YORI_CHECK(spawned);

    job::JobSpec spec = spec_for({"adopted"});
    spec.argv = {"/bin/sh", "-c", "adopted"};
    store::StoredJob running;
    running.id = job::JobId{7};
    running.spec = spec;
    running.state = job::JobState::kQueued;
    running.revision = 0;
    store::StateMutation seed;
    seed.expected_revision = 0;
    seed.create_jobs.push_back(running);
    YORI_CHECK(store->apply(seed).ok());

    store::StoredJob starting = running;
    starting.state = job::JobState::kStarting;
    starting.revision = 1;
    store::StateMutation to_starting;
    to_starting.expected_revision = store->load().snapshot.revision;
    to_starting.update_jobs.push_back(starting);
    to_starting.acquire_leases.push_back(gpu::GpuLease{gpu::GpuUuid{"GPU-jm"}, job::JobId{7}});
    YORI_CHECK(store->apply(to_starting).ok());

    store::StoredJob promoted = starting;
    promoted.state = job::JobState::kRunning;
    promoted.revision = 2;
    promoted.execution.identity = spawned.identity;
    promoted.execution.log_path = "/tmp/adopted-logs";
    promoted.execution.start_time = std::chrono::system_clock::now();
    store::StateMutation promote;
    promote.expected_revision = store->load().snapshot.revision;
    promote.update_jobs.push_back(std::move(promoted));
    YORI_CHECK(store->apply(promote).ok());

    // holder 不再管理该进程（模拟旧 daemon 消失）：abandon 而非击杀。
    YORI_CHECK(holder.abandon() == process::AbandonCode::kAbandoned);

    // 恢复：JobRecovery 核验身份 -> 采纳 RUNNING。
    queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone;
    auto queue = GlobalJobQueue::create({}, queue_error);
    YORI_CHECK(queue != nullptr);
    recovery::JobRecovery recovery(*store, *queue);
    const auto recovery_result = recovery.recover();
    YORI_CHECK(recovery_result.ok());
    YORI_CHECK(recovery_result.outcomes.size() == 1);
    YORI_CHECK(recovery_result.outcomes[0].decision ==
               recovery::RecoveryDecisionCode::kAdoptedRunning);

    runtime::GpuManager gpu_manager(runtime.executor(), provider);
    YORI_CHECK(gpu_manager.start().ok());
    runtime::LogStreamer streamer;
    launch::PosixIdentityResolver resolver;
    launch::DefaultLaunchAdapter adapter;
    executor::comm::PhaseGate gate{"jm-adopt"};
    runtime::JobManagerConfig manager_config;
    manager_config.log_root = make_root();
    manager_config.cancel_grace = 300ms;
    runtime::JobManager manager(runtime.executor(), *store, *queue, gpu_manager, streamer,
                                resolver, adapter, gate, manager_config);
    YORI_CHECK(manager.start(recovery_result).ok());
    {
      const auto stats = manager.stats();
      YORI_CHECK(stats.jobs_adopted == 1 && stats.active_supervised == 1);
    }

    // 采纳后取消：SIGTERM 被忽略 -> 宽限升级 SIGKILL -> CANCELLED + lease 释放。
    const auto cancel = manager.cancel_job(7);
    YORI_CHECK(cancel.code == ipc::JobCancelOutcome::Code::kStopping);
    YORI_CHECK(wait_state(*store, 7, job::JobState::kCancelled, 20s));
    {
      const auto load = store->load();
      YORI_CHECK(load.snapshot.leases.empty());
    }

    static_cast<void>(manager.stop());
    static_cast<void>(gpu_manager.stop());
    YORI_CHECK(runtime.shutdown() == runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // ---- Yori 特有 5：STOPPING 恢复重发取消（kAdoptedStoppingNeedsRecancel）--
  {
    ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize({}, error));

    testing::FakeGpuProvider provider;
    set_single_gpu(provider, gpu::GpuObservedState::kFree);
    auto store = std::make_unique<runtime::SerialStateStore>(
        std::make_unique<testing::InMemoryStateStore>());

    process::ProcessSupervisor holder;
    const auto spawned =
        holder.spawn(testing::self_plan({"/bin/sh", "-c", "trap '' TERM; sleep 120"}));
    YORI_CHECK(spawned);

    job::JobSpec spec = spec_for({"recancel"});
    store::StoredJob stopping;
    stopping.id = job::JobId{9};
    stopping.spec = spec;
    stopping.state = job::JobState::kQueued;
    stopping.revision = 0;
    store::StateMutation seed;
    seed.expected_revision = 0;
    seed.create_jobs.push_back(stopping);
    YORI_CHECK(store->apply(seed).ok());

    store::StoredJob starting = stopping;
    starting.state = job::JobState::kStarting;
    starting.revision = 1;
    store::StateMutation to_starting;
    to_starting.expected_revision = store->load().snapshot.revision;
    to_starting.update_jobs.push_back(starting);
    to_starting.acquire_leases.push_back(gpu::GpuLease{gpu::GpuUuid{"GPU-jm"}, job::JobId{9}});
    YORI_CHECK(store->apply(to_starting).ok());

    store::StoredJob cancelled_phase = starting;
    cancelled_phase.state = job::JobState::kRunning;
    cancelled_phase.revision = 2;
    cancelled_phase.execution.identity = spawned.identity;
    cancelled_phase.execution.start_time = std::chrono::system_clock::now();
    store::StoredJob stopping_phase = cancelled_phase;
    store::StateMutation to_running;
    to_running.expected_revision = store->load().snapshot.revision;
    to_running.update_jobs.push_back(std::move(cancelled_phase));
    YORI_CHECK(store->apply(to_running).ok());

    stopping_phase.state = job::JobState::kStopping;
    stopping_phase.revision = 3;
    store::StateMutation to_stopping;
    to_stopping.expected_revision = store->load().snapshot.revision;
    to_stopping.update_jobs.push_back(std::move(stopping_phase));
    YORI_CHECK(store->apply(to_stopping).ok());
    YORI_CHECK(holder.abandon() == process::AbandonCode::kAbandoned);

    queue::QueueErrorCode queue_error = queue::QueueErrorCode::kNone;
    auto queue = GlobalJobQueue::create({}, queue_error);
    recovery::JobRecovery recovery(*store, *queue);
    const auto recovery_result = recovery.recover();
    YORI_CHECK(recovery_result.ok());
    YORI_CHECK(recovery_result.outcomes.size() == 1);
    YORI_CHECK(recovery_result.outcomes[0].decision ==
               recovery::RecoveryDecisionCode::kAdoptedStoppingNeedsRecancel);

    runtime::GpuManager gpu_manager(runtime.executor(), provider);
    YORI_CHECK(gpu_manager.start().ok());
    runtime::LogStreamer streamer;
    launch::PosixIdentityResolver resolver;
    launch::DefaultLaunchAdapter adapter;
    executor::comm::PhaseGate gate{"jm-recancel"};
    runtime::JobManagerConfig manager_config;
    manager_config.log_root = make_root();
    manager_config.cancel_grace = 300ms;
    runtime::JobManager manager(runtime.executor(), *store, *queue, gpu_manager, streamer,
                                resolver, adapter, gate, manager_config);
    YORI_CHECK(manager.start(recovery_result).ok());
    {
      const auto stats = manager.stats();
      YORI_CHECK(stats.jobs_adopted == 1 && stats.recancels_armed == 1);
    }
    // 重发的取消 + 重建的宽限在忽略 SIGTERM 时升级 SIGKILL。
    YORI_CHECK(wait_state(*store, 9, job::JobState::kCancelled, 20s));
    {
      const auto load = store->load();
      YORI_CHECK(load.snapshot.leases.empty());
    }

    static_cast<void>(manager.stop());
    static_cast<void>(gpu_manager.stop());
    YORI_CHECK(runtime.shutdown() == runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "job manager: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("job manager: all checks passed\n");
  return 0;
}
