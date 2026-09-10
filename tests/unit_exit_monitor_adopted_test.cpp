#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/process_exit_monitor.hpp"
#include "yori_test.hpp"

// M7：ExitMonitor 对采纳进程（非本进程子进程，daemon 重启后恢复的 Job）的
// 退出探测：waitpid ECHILD 分类 + 周期 /proc 存在性扫描；状态不可得（显式
// identity_verified=false）。
namespace {

using namespace yori;
using namespace std::chrono_literals;

// 双 fork 产生一个不属于本进程的孙进程（重定向到 init/subreaper），经管道
// 回传其 PID 与身份。
process::ProcessIdentity spawn_detached() {
  std::array<int, 2> report_pipe{};
  YORI_CHECK(::pipe(report_pipe.data()) == 0);

  const pid_t middle = ::fork();
  YORI_CHECK(middle >= 0);
  if (middle == 0) {
    YORI_CHECK(::close(report_pipe[0]) == 0);
    const pid_t grand = ::fork();
    if (grand == 0) {
      // 孙进程：尽力脱离会话（会话领导身份时 setsid 失败不影响测试语义），
      // 存活 2 秒后退出。
      static_cast<void>(::setsid());
      YORI_CHECK(::close(report_pipe[1]) == 0);
      ::sleep(2);
      ::_exit(0);
    }
    std::array<char, 8> pid_bytes{};
    const int written =
        ::std::snprintf(pid_bytes.data(), pid_bytes.size(), "%lld", static_cast<long long>(grand));
    YORI_CHECK(written > 0);
    YORI_CHECK(::write(report_pipe[1], pid_bytes.data(), static_cast<std::size_t>(written)) ==
               written);
    YORI_CHECK(::close(report_pipe[1]) == 0);
    ::_exit(0);
  }
  YORI_CHECK(::close(report_pipe[1]) == 0);
  std::array<char, 8> pid_bytes{};
  std::size_t received = 0;
  while (received < pid_bytes.size()) {
    const auto n = ::read(report_pipe[0], pid_bytes.data() + received, 1);
    if (n <= 0 || pid_bytes[received] == '\0') {
      break;
    }
    ++received;
  }
  YORI_CHECK(::close(report_pipe[0]) == 0);
  pid_bytes[received] = '\0';

  int raw_status = 0;
  while (::waitpid(middle, &raw_status, 0) < 0 && errno == EINTR) {
  }

  const auto pid = std::atoll(pid_bytes.data());
  const auto pgid = process::read_process_pgid(pid);
  const auto ticks = process::read_process_start_ticks(pid);
  if (!pgid.has_value() || !ticks.has_value()) {
    return process::ProcessIdentity{};
  }
  return process::ProcessIdentity{pid, *pgid, *ticks};
}

}  // namespace

int main() {
  yori::runtime::ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize({}, error));

  const process::ProcessIdentity identity = spawn_detached();
  YORI_CHECK(identity.valid());

  yori::runtime::ProcessExitMonitor monitor(runtime.executor());
  YORI_CHECK(monitor.start().ok());

  // 注册即分类：非子进程（ECHILD）-> 采纳路径（周期 /proc 探测）。
  const auto registered = monitor.register_process(identity);
  YORI_CHECK(registered.code == yori::runtime::ExitRegisterCode::kRegistered);

  // 孙进程 2 秒后退出；采纳路径的探测周期为 1 秒 -> 5 秒内必达。
  yori::runtime::ExitEvent event;
  bool received = false;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (monitor.receive_exit_for(event, 200ms)) {
      received = true;
      break;
    }
  }
  YORI_CHECK(received);
  if (received) {
    YORI_CHECK(event.pid == identity.pid);
    // 采纳进程的退出状态不可得（waitpid 不可用）：显式 unknown + 未核验。
    YORI_CHECK(!event.identity_verified);
    YORI_CHECK(event.status.reason == process::ExitReason::kUnknown);
  }

  monitor.stop();
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "exit monitor adopted: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("exit monitor adopted: all checks passed\n");
  return 0;
}
