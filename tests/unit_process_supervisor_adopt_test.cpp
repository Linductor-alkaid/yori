#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "yori_test.hpp"

// M7-01：ProcessSupervisor 的 adopt/abandon 契约（恢复采纳与 RULE-10 关闭）。
namespace {

using namespace yori;
using namespace std::chrono_literals;

}  // namespace

int main() {
  // ---- adopt：无效身份 / 已运行 / 已消亡进程 / 正常采纳 --------------------
  {
    process::ProcessSupervisor supervisor;
    YORI_CHECK(supervisor.adopt(process::ProcessIdentity{}).code ==
               process::AdoptErrorCode::kInvalidIdentity);

    auto spawned = supervisor.spawn(yori::testing::self_plan({"sleep", "2"}));
    YORI_CHECK(spawned);
    if (spawned) {
      // 已管理进程时拒绝采纳。
      YORI_CHECK(supervisor.adopt(spawned.identity).code ==
                 process::AdoptErrorCode::kAlreadyRunning);

      // 第二个守护者可以采纳同一进程（恢复语义：新 daemon 实例）。
      process::ProcessSupervisor successor;
      YORI_CHECK(successor.adopt(spawned.identity).ok());
      YORI_CHECK(successor.phase() == process::ProcessSupervisor::Phase::kRunning);

      // 采纳后取消语义可用：SIGTERM 进程组。
      YORI_CHECK(successor.request_cancel().terminating());
      const auto exited = yori::testing::wait_for_exit(successor);
      // 同一进程内的采纳者仍可 waitpid（真实跨进程重启场景为 ECHILD，退出
      // 状态由 ExitMonitor 的采纳探测提供，见 unit_exit_monitor_adopted_test）。
      YORI_CHECK(exited.exited());
      YORI_CHECK(exited.status.signaled() && exited.status.signal_number == SIGTERM);

      // 原守护者 abandon 后不再持有进程：析构不重复发信号（进程已被回收）。
      YORI_CHECK(supervisor.abandon() == process::AbandonCode::kAbandoned);
      YORI_CHECK(supervisor.phase() == process::ProcessSupervisor::Phase::kIdle);
    }
  }

  // ---- adopt：进程已消亡（身份核验失败，PID reuse 防护）--------------------
  {
    process::ProcessSupervisor holder;
    auto spawned = holder.spawn(yori::testing::self_plan({"true"}));
    YORI_CHECK(spawned);
    if (spawned) {
      const process::ProcessIdentity identity = spawned.identity;
      const auto exited = yori::testing::wait_for_exit(holder);
      YORI_CHECK(exited.exited());
      process::ProcessSupervisor successor;
      // 已退出（僵尸或已回收）进程的采纳被身份核验拒绝。
      YORI_CHECK(successor.adopt(identity).code == process::AdoptErrorCode::kIdentityVerifyFailed);
      YORI_CHECK(successor.phase() == process::ProcessSupervisor::Phase::kIdle);
    }
  }

  // ---- abandon：空闲时显式空结果；运行中忘记进程且不发信号（RULE-10）------
  {
    process::ProcessSupervisor supervisor;
    YORI_CHECK(supervisor.abandon() == process::AbandonCode::kNotRunning);

    auto spawned = supervisor.spawn(yori::testing::self_plan({"sleep", "2"}));
    YORI_CHECK(spawned);
    if (spawned) {
      YORI_CHECK(supervisor.abandon() == process::AbandonCode::kAbandoned);
      YORI_CHECK(supervisor.phase() == process::ProcessSupervisor::Phase::kIdle);
      YORI_CHECK(!supervisor.identity().valid());
      // abandon 后实例可复用：重新 spawn 正常。
      auto respawned = supervisor.spawn(yori::testing::self_plan({"true"}));
      YORI_CHECK(respawned);
      if (respawned) {
        YORI_CHECK(yori::testing::wait_for_exit(supervisor).status.is_success());
      }
      // 第一个进程未被信号（abandon 语义）：仍存活，测试回收。
      int raw_status = 0;
      const pid_t pid = static_cast<pid_t>(spawned.identity.pid);
      while (::kill(pid, SIGKILL) == 0 && ::waitpid(pid, &raw_status, 0) < 0 && errno == EINTR) {
      }
    }
  }

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "supervisor adopt/abandon: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("supervisor adopt/abandon: all checks passed\n");
  return 0;
}
