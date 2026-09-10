#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/store/sqlite_state_store.hpp>

#include "process_test_support.hpp"
#include "runtime/daemon.hpp"
#include "runtime/executor_runtime.hpp"
#include "testing/fake_gpu_provider.hpp"
#include "yori_test.hpp"

// M7-04：守护总装收口的 daemon 级闭环（recovery 标签）：
// - submit -> 调度 -> spawn -> 退出 -> FINISHED（EXEC-10 ① 关闭不终止训练）；
// - daemon 停止时运行中进程 abandon（RULE-10）-> 新 daemon 恢复采纳为 RUNNING
//   （绝不重启，RULE-06）-> 采纳取消收敛 CANCELLED；
// - QUEUED Job 跨 daemon 重启不丢（恢复重新入队并调度）。
namespace {

using namespace yori;
using namespace std::chrono_literals;

std::string make_root() {
  char pattern[] = "/tmp/yori-closure-test-XXXXXX";
  char* directory = ::mkdtemp(pattern);
  YORI_CHECK(directory != nullptr);
  return directory;
}

// 按值返回：快照是 load() 的临时结果，返回其内部指针会在临时析构后悬空。
std::optional<store::StoredJob> find_stored(store::StateStore& store, std::uint64_t id) {
  const auto load = store.load();
  for (const auto& record : load.snapshot.jobs) {
    if (record.id.value() == id) {
      return record;
    }
  }
  return std::nullopt;
}

bool wait_state(store::StateStore& store, std::uint64_t id, job::JobState state,
                std::chrono::milliseconds timeout = 15s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (const auto record = find_stored(store, id); record.has_value() && record->state == state) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

// 打开（或复用）测试数据库：daemon 实例间共享同一文件。
std::unique_ptr<store::SqliteStateStore> open_sqlite(const std::string& path) {
  store::SqliteStateStoreConfig config;
  config.database_path = path;
  auto store = std::make_unique<store::SqliteStateStore>(config);
  const auto opened = store->open();
  YORI_CHECK(opened.ok());
  return store;
}

std::unique_ptr<store::StateStore> wrap_serial(std::unique_ptr<store::SqliteStateStore> store) {
  return std::make_unique<yori::runtime::SerialStateStore>(std::move(store));
}

void set_gpu(testing::FakeGpuProvider& provider) {
  gpu::GpuObservationSnapshot snapshot;
  snapshot.observed_at = std::chrono::system_clock::now();
  gpu::GpuObservation device;
  device.uuid = gpu::GpuUuid{"GPU-closure"};
  device.index = 0;
  device.state = gpu::GpuObservedState::kFree;
  snapshot.devices = {device};
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

}  // namespace

int main() {
  const std::string root = make_root();
  YORI_CHECK(::mkdir((root + "/jobs").c_str(), 0700) == 0);

  // ---- 第一代 daemon：训练存活中正常关闭（RULE-10：不终止训练）----------
  process::ProcessIdentity survived{};
  {
    yori::runtime::ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize({}, error));

    testing::FakeGpuProvider provider;
    set_gpu(provider);
    // 持久化后端（DEC-009）：跨 daemon 实例的 Job 事实。
    auto store = open_sqlite(root + "/state.db");
    store::StateStore* store_ptr = store.get();

    yori::runtime::DaemonConfig config;
    config.ipc.socket_path = root + "/yori.sock";
    config.ipc.socket_mode = 0600;
    config.job_manager.log_root = root + "/jobs";
    config.job_manager.cancel_grace = 400ms;
    yori::runtime::Daemon daemon(runtime.executor(), provider, wrap_serial(std::move(store)),
                                 config);
    YORI_CHECK(daemon.start().ok());

    // Job 1：长驻训练（占用唯一 GPU）；Job 2：排队。经守护委派入口提交
    // （与 IPC 服务同一入口）。
    const auto submit1 = daemon.job_control().submit_job(spec_for({"sleep", "60"}));
    YORI_CHECK(submit1.ok() && submit1.job_id == 1);
    YORI_CHECK(wait_state(*store_ptr, 1, job::JobState::kRunning));
    const auto submit2 = daemon.job_control().submit_job(spec_for({"true"}));
    YORI_CHECK(submit2.ok() && submit2.job_id == 2);
    {
      const auto record = find_stored(*store_ptr, 2);
      YORI_CHECK(record.has_value() && record->state == job::JobState::kQueued);
    }
    if (const auto record = find_stored(*store_ptr, 1); record.has_value()) {
      survived = record->execution.identity;
    }
    static_cast<void>(submit2);
    YORI_CHECK(survived.valid());

    // daemon 正常关闭：IPC/跟随/GPU/守护承载按 EXEC-10 回收，训练不被终止。
    // stop 前读取统计（stop 后守护承载被回收，统计口不再持有实现）。
    const auto pre_stop = daemon.job_manager_stats();
    YORI_CHECK(pre_stop.active_supervised == 1);
    YORI_CHECK(daemon.stop() == yori::runtime::DaemonStopCode::kStopped);
    YORI_CHECK(pre_stop.abandoned_at_stop == 0);  // 关闭动作前当然为 0

    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // 关闭后训练仍存活（RULE-10/DEC-008 语义）。
  YORI_CHECK(process::verify_process_identity(survived));

  // ---- 第二代 daemon：恢复采纳 RUNNING（不重启）、QUEUED 重新入队调度 ----
  {
    yori::runtime::ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize({}, error));

    testing::FakeGpuProvider provider;
    set_gpu(provider);
    auto store = open_sqlite(root + "/state.db");
    store::StateStore* store_ptr = store.get();

    yori::runtime::DaemonConfig config;
    config.ipc.socket_path = root + "/yori.sock";
    config.ipc.socket_mode = 0600;
    config.job_manager.log_root = root + "/jobs";
    config.job_manager.cancel_grace = 400ms;
    yori::runtime::Daemon daemon(runtime.executor(), provider, wrap_serial(std::move(store)),
                                 config);
    YORI_CHECK(daemon.start().ok());

    // 恢复决策：Job 1 采纳为 RUNNING（同一进程身份，未重启）；Job 2 重新入队。
    const auto& recovery = daemon.last_recovery();
    if (!recovery.has_value() || !recovery->ok()) {
      std::fprintf(stderr, "closure: recovery missing or failed\n");
      return 1;
    }
    YORI_CHECK(recovery->outcomes.size() == 2);
    {
      const auto running = find_stored(*store_ptr, 1);
      YORI_CHECK(running.has_value() && running->state == job::JobState::kRunning);
      if (running.has_value()) {
        // 未重启：采纳的是同一进程身份三元组（RULE-06）。
        YORI_CHECK(running->execution.identity.pid == survived.pid &&
                   running->execution.identity.pgid == survived.pgid &&
                   running->execution.identity.start_ticks == survived.start_ticks);
      }
    }
    // Job 2 因唯一 GPU 被采纳 Job 占用而保持 QUEUED。
    {
      const auto queued = find_stored(*store_ptr, 2);
      YORI_CHECK(queued.has_value() && queued->state == job::JobState::kQueued);
    }

    // 采纳 Job 的取消收敛：SIGTERM（sleep 不陷阱）-> CANCELLED + lease 释放
    // -> Job 2 自动调度完成。
    const auto cancelled = daemon.job_control().cancel_job(1);
    YORI_CHECK(cancelled.code == yori::ipc::JobCancelOutcome::Code::kStopping);
    YORI_CHECK(wait_state(*store_ptr, 1, job::JobState::kCancelled));
    YORI_CHECK(wait_state(*store_ptr, 2, job::JobState::kFinished));
    {
      const auto load = store_ptr->load();
      YORI_CHECK(load.snapshot.leases.empty());
    }

    YORI_CHECK(daemon.stop() == yori::runtime::DaemonStopCode::kStopped);
    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "supervision closure: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("supervision closure: all checks passed\n");
  return 0;
}
