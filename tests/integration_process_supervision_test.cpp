#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <yori/launch/launch_adapter.hpp>
#include <yori/observe/log_sink.hpp>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/grace_escalation.hpp"
#include "runtime/log_pump.hpp"
#include "runtime/process_exit_monitor.hpp"
#include "yori_test.hpp"

namespace {

using namespace yori;
using namespace yori::runtime;
using namespace std::chrono_literals;

std::string make_directory() {
  char pattern[] = "/tmp/yori-supervision-test-XXXXXX";
  char* dir = ::mkdtemp(pattern);
  YORI_CHECK(dir != nullptr);
  return dir;
}

std::string read_file(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::string data;
  char buffer[4096];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, n);
  }
  std::fclose(file);
  return data;
}

}  // namespace

int main() {
  // 端到端链路：LaunchAdapter 构造 plan -> spawn（进程组/降权/SIGPIPE 策略）->
  // ExitMonitor 注册 -> LogPump 落盘 -> 退出事件 -> 取消升级定时。
  ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(ExecutorRuntimeConfig{}, error));

  const launch::IdentityInfo identity = yori::testing::current_identity();
  YORI_CHECK(identity.uid != 0);

  // ---- 场景 A：正常完成（训练脚本输出日志后退出 0） ---------------------------
  {
    ProcessExitMonitor monitor(runtime.executor());
    LogPump pump(runtime.executor());
    YORI_CHECK(monitor.start().ok());
    YORI_CHECK(pump.start().ok());

    const std::string directory = make_directory();
    launch::DefaultLaunchAdapter adapter;
    adapter.set_daemon_environment({{"PATH", "/usr/bin:/bin"}, {"SECRET", "leak"}});

    job::JobSpec spec;
    spec.owner_uid = identity.uid;
    spec.owner_gid = identity.gid;
    spec.argv = {"/bin/sh", "-c", "echo stdout-from-training; echo stderr-from-training 1>&2"};
    spec.cwd = "/tmp";
    spec.env = {{"TRAIN_STEP", "42"}};
    spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};

    const auto plan =
        adapter.prepare(spec, launch::GpuAssignment{yori::gpu::GpuUuid{"GPU-it"}, 2, 0},
                        launch::LaunchProfile{}, identity);
    YORI_CHECK(plan);

    observe::LogSinkConfig sink_config;
    sink_config.directory = directory;
    observe::LogSinkOpenOptions open_options;
    open_options.owner_uid = identity.uid;
    open_options.owner_gid = identity.gid;
    observe::LogSink sink;
    std::string open_error;
    YORI_CHECK(sink.open(sink_config, open_options, open_error) ==
               observe::LogSinkErrorCode::kNone);

    process::ProcessSupervisor supervisor;
    auto spawned = supervisor.spawn(plan.plan);
    YORI_CHECK(spawned);
    if (spawned) {
      YORI_CHECK(monitor.register_process(spawned.identity).ok());

      LogPumpJobInput input;
      input.job = job::JobId{100};
      input.stdout_read = std::move(spawned.stdout_read);
      input.stderr_read = std::move(spawned.stderr_read);
      input.sink = std::move(sink);
      YORI_CHECK(pump.attach(std::move(input)).ok());

      ExitEvent event;
      YORI_CHECK(monitor.receive_exit_for(event, 5s));
      YORI_CHECK(event.identity_verified);
      YORI_CHECK(event.status.is_success());

      LogPumpDone done;
      YORI_CHECK(pump.receive_done_for(done, 5s));
      YORI_CHECK(done.completed);
      YORI_CHECK(read_file(directory + "/stdout.log") == "stdout-from-training\n");
      YORI_CHECK(read_file(directory + "/stderr.log") == "stderr-from-training\n");
    }
    pump.stop();
    monitor.stop();
  }

  // ---- 场景 B：执行中取消 + 宽限升级（忽略 SIGTERM 的"训练"被 SIGKILL）-------
  {
    ProcessExitMonitor monitor(runtime.executor());
    LogPump pump(runtime.executor());
    YORI_CHECK(monitor.start().ok());
    YORI_CHECK(pump.start().ok());

    const std::string directory = make_directory();
    launch::DefaultLaunchAdapter adapter;
    job::JobSpec spec;
    spec.owner_uid = identity.uid;
    spec.owner_gid = identity.gid;
    spec.argv = {"/bin/sh", "-c", "echo starting; trap '' TERM; sleep 300"};
    spec.cwd = "/tmp";
    spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
    const auto plan =
        adapter.prepare(spec, launch::GpuAssignment{yori::gpu::GpuUuid{"GPU-it"}, 0, 0},
                        launch::LaunchProfile{}, identity);
    YORI_CHECK(plan);

    observe::LogSink sink;
    observe::LogSinkConfig sink_config;
    sink_config.directory = directory;
    observe::LogSinkOpenOptions open_options;
    open_options.owner_uid = identity.uid;
    open_options.owner_gid = identity.gid;
    std::string open_error;
    YORI_CHECK(sink.open(sink_config, open_options, open_error) ==
               observe::LogSinkErrorCode::kNone);

    process::ProcessSupervisor supervisor(process::CancelPolicy{150ms});
    auto spawned = supervisor.spawn(plan.plan);
    YORI_CHECK(spawned);
    if (spawned) {
      YORI_CHECK(monitor.register_process(spawned.identity).ok());
      LogPumpJobInput input;
      input.job = job::JobId{101};
      input.stdout_read = std::move(spawned.stdout_read);
      input.stderr_read = std::move(spawned.stderr_read);
      input.sink = std::move(sink);
      YORI_CHECK(pump.attach(std::move(input)).ok());

      // 等 shell 输出 starting 并安装 TERM 忽略后再取消（exec 确认 != trap 已执行）。
      std::this_thread::sleep_for(300ms);
      YORI_CHECK(supervisor.request_cancel().terminating());
      GraceEscalation escalation(runtime.executor(), supervisor);
      YORI_CHECK(escalation.arm() == GraceArmCode::kArmed);

      ExitEvent event;
      YORI_CHECK(monitor.receive_exit_for(event, 10s));
      YORI_CHECK(event.status.signaled());
      YORI_CHECK(event.status.signal_number == SIGKILL);

      const auto consumed = escalation.try_consume();
      YORI_CHECK(consumed.consumed());
      YORI_CHECK(consumed.escalation.has_value());
      YORI_CHECK(consumed.escalation->killed());

      LogPumpDone done;
      YORI_CHECK(pump.receive_done_for(done, 5s));
      YORI_CHECK(read_file(directory + "/stdout.log") == "starting\n");
    }
    pump.stop();
    monitor.stop();
  }

  // ---- 场景 C：取消后进程在宽限期内自然退出，升级定时被 disarm ---------------
  {
    ProcessExitMonitor monitor(runtime.executor());
    YORI_CHECK(monitor.start().ok());

    process::ProcessSupervisor supervisor(process::CancelPolicy{5000ms});
    auto spawned = supervisor.spawn(yori::testing::self_plan({"sleep", "30"}));
    YORI_CHECK(spawned);
    if (spawned) {
      YORI_CHECK(monitor.register_process(spawned.identity).ok());
      YORI_CHECK(supervisor.request_cancel().terminating());
      GraceEscalation escalation(runtime.executor(), supervisor);
      YORI_CHECK(escalation.arm() == GraceArmCode::kArmed);
      YORI_CHECK(escalation.arm() == GraceArmCode::kAlreadyArmed);

      ExitEvent event;
      YORI_CHECK(monitor.receive_exit_for(event, 5s));
      YORI_CHECK(event.status.signal_number == SIGTERM);
      const auto disarmed = escalation.disarm();
      YORI_CHECK(disarmed == GraceDisarmCode::kCancelledBeforeDispatch ||
                 disarmed == GraceDisarmCode::kConsumedCompletion);
      YORI_CHECK(!escalation.armed());
    }
    monitor.stop();
  }

  YORI_CHECK(runtime.shutdown() == ExecutorRuntimeShutdownResult::kCompleted);
  return yori::testing::failure_count == 0 ? 0 : 1;
}
