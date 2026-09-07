#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/process_exit_monitor.hpp"
#include "yori_test.hpp"

namespace {

using namespace yori;
using namespace yori::runtime;
using namespace std::chrono_literals;

bool wait_registered(const ProcessExitMonitor& monitor, std::size_t expected) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (monitor.registered_count() == expected) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return monitor.registered_count() == expected;
}

}  // namespace

int main() {
  ExecutorRuntime runtime;
  std::string error;
  ExecutorRuntimeConfig config;
  YORI_CHECK(runtime.initialize(config, error));
  YORI_CHECK(runtime.is_initialized());

  {
    ProcessExitMonitor monitor(runtime.executor());
    const auto start = monitor.start();
    YORI_CHECK(start.ok());

    // 六场景之"正常完成"：spawn -> 注册 -> 退出事件。
    process::ProcessSupervisor supervisor;
    auto spawned = supervisor.spawn(yori::testing::self_plan({"/bin/sh", "-c", "exit 0"}));
    YORI_CHECK(spawned);
    if (spawned) {
      const auto registered = monitor.register_process(spawned.identity);
      YORI_CHECK(registered.ok());
      YORI_CHECK(wait_registered(monitor, 1));

      ExitEvent event;
      YORI_CHECK(monitor.receive_exit_for(event, 5s));
      YORI_CHECK(event.pid == spawned.identity.pid);
      YORI_CHECK(event.identity_verified);
      YORI_CHECK(event.status.exited());
      YORI_CHECK(event.status.is_success());
      YORI_CHECK(monitor.registered_count() == 0);
      // 回收权在 monitor：supervisor 不再重复 waitpid（双回收是 ECHILD 错误用法）。
    }

    // 注册前已退出的竞态：注册后立即补扫仍能收到事件。
    process::ProcessSupervisor quick;
    auto finished = quick.spawn(yori::testing::self_plan({"/bin/true"}));
    YORI_CHECK(finished);
    if (finished) {
      // 等 /bin/true 退出成僵尸但不回收（supervisor 不再 poll）。
      std::this_thread::sleep_for(200ms);
      const auto registered = monitor.register_process(finished.identity);
      YORI_CHECK(registered.ok());
      ExitEvent event;
      YORI_CHECK(monitor.receive_exit_for(event, 5s));
      YORI_CHECK(event.pid == finished.identity.pid);
      YORI_CHECK(event.status.is_success());
    }

    // 多进程并发监视与注销。
    process::ProcessSupervisor long_runner;
    auto sleeper = long_runner.spawn(yori::testing::self_plan({"sleep", "30"}));
    YORI_CHECK(sleeper);
    if (sleeper) {
      const auto registered = monitor.register_process(sleeper.identity);
      YORI_CHECK(registered.ok());

      const auto unregistered = monitor.unregister_process(sleeper.identity.pid);
      YORI_CHECK(unregistered.ok());
      ExitEvent none;
      YORI_CHECK(!monitor.try_receive_exit(none));

      // 重复注销显式 NotFound；注销后进程仍在运行（RULE-10 语义）。
      YORI_CHECK(monitor.unregister_process(sleeper.identity.pid).code ==
                 ExitUnregisterCode::kNotFound);
      YORI_CHECK(::kill(static_cast<pid_t>(sleeper.identity.pid), 0) == 0);

      // shutdown 路径：monitor 停止不终止被监视进程。
      monitor.stop();
      YORI_CHECK(::kill(static_cast<pid_t>(sleeper.identity.pid), 0) == 0);

      // 进程由 supervisor 正常取消并回收（不留孤儿/僵尸）。
      YORI_CHECK(long_runner.request_cancel().terminating());
      const auto exit = yori::testing::wait_for_exit(long_runner);
      YORI_CHECK(exit.status.signaled());
    }

    monitor.stop();  // 幂等。
    // 无效身份的注册拒绝。
    const auto invalid = monitor.register_process(process::ProcessIdentity{});
    YORI_CHECK(invalid.code == ExitRegisterCode::kNotStarted);
  }

  // 六场景之"执行中取消 + 退出事件"：取消后由 monitor 收到 SIGTERM 退出。
  {
    ProcessExitMonitor monitor(runtime.executor());
    YORI_CHECK(monitor.start().ok());
    process::ProcessSupervisor supervisor(process::CancelPolicy{5000ms});
    auto spawned = supervisor.spawn(yori::testing::self_plan({"sleep", "30"}));
    YORI_CHECK(spawned);
    if (spawned) {
      YORI_CHECK(monitor.register_process(spawned.identity).ok());
      YORI_CHECK(supervisor.request_cancel().terminating());
      ExitEvent event;
      YORI_CHECK(monitor.receive_exit_for(event, 5s));
      YORI_CHECK(event.status.signaled());
      YORI_CHECK(event.status.signal_number == SIGTERM);
    }
    monitor.stop();
  }

  // start 拒绝路径：停止后的 monitor 不可重启（显式生命周期）。
  {
    ProcessExitMonitor monitor(runtime.executor());
    YORI_CHECK(monitor.start().ok());
    monitor.stop();
    const auto again = monitor.start();
    YORI_CHECK(again.code == ExitMonitorStartCode::kWorkerRejected);
  }

  YORI_CHECK(runtime.shutdown() == ExecutorRuntimeShutdownResult::kCompleted);
  return yori::testing::failure_count == 0 ? 0 : 1;
}
