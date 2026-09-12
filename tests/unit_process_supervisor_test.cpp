#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "yori_test.hpp"

namespace {

using namespace yori::process;

// 有界等待 read 全部内容（测试内的管道读取 helper）。
std::string read_all(const FileDescriptor& fd, std::chrono::milliseconds timeout) {
  std::string data;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  char buffer[4096];
  while (std::chrono::steady_clock::now() < deadline) {
    const ssize_t n = ::read(fd.value(), buffer, sizeof(buffer));
    if (n > 0) {
      data.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return data;
}

}  // namespace

int main() {
  // ---- 六场景之"任务异常"：exec 失败为结构化结果且不留僵尸 --------------------
  {
    ProcessSupervisor supervisor;
    const auto result = supervisor.spawn(yori::testing::self_plan({"/nonexistent/binary"}));
    YORI_CHECK(!result);
    YORI_CHECK(result.code == SpawnErrorCode::kExecFailed);
    YORI_CHECK(result.error_number == ENOENT);
    YORI_CHECK(supervisor.phase() == ProcessSupervisor::Phase::kIdle);
  }
  {
    ProcessSupervisor supervisor;
    const auto result = supervisor.spawn(yori::testing::self_plan({"definitely-not-on-path-xyz"}));
    YORI_CHECK(!result);
    YORI_CHECK(result.code == SpawnErrorCode::kExecFailed);
  }
  {
    // PATH 解析成功（true 在 /usr/bin）。
    ProcessSupervisor supervisor;
    const auto result =
        supervisor.spawn(yori::testing::self_plan({"true"}, {{"PATH", "/usr/bin:/bin"}}));
    YORI_CHECK(result);
    if (result) {
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.exited());
      YORI_CHECK(exit.status.is_success());
    }
  }

  // ---- 提交拒绝：无效 plan / 重复 spawn -------------------------------------
  {
    ProcessSupervisor supervisor;
    auto result = supervisor.spawn(yori::testing::self_plan({}));
    YORI_CHECK(!result);
    YORI_CHECK(result.code == SpawnErrorCode::kInvalidPlan);

    result = supervisor.spawn(yori::testing::self_plan({"true"}, {{"BAD NAME", "x"}}));
    YORI_CHECK(result.code == SpawnErrorCode::kInvalidPlan);
  }
  {
    ProcessSupervisor supervisor;
    YORI_CHECK(supervisor.spawn(yori::testing::self_plan({"sleep", "2"})));
    const auto second = supervisor.spawn(yori::testing::self_plan({"true"}));
    YORI_CHECK(second.code == SpawnErrorCode::kAlreadyRunning);
    // 无效取消策略在 spawn 显式拒绝。
    ProcessSupervisor broken(CancelPolicy{std::chrono::milliseconds{1}});
    const auto rejected = broken.spawn(yori::testing::self_plan({"true"}));
    YORI_CHECK(rejected.code == SpawnErrorCode::kInvalidPlan);
    // 清理：击杀并等待回收（析构兜底语义的前置显式化）。
    YORI_CHECK(supervisor.request_cancel().terminating());
    const auto exit = yori::testing::wait_for_exit(supervisor);
    YORI_CHECK(exit.exited());
  }

  // ---- 正常完成：退出码、管道内容、环境一致性 --------------------------------
  {
    ProcessSupervisor supervisor;
    const auto result = supervisor.spawn(
        yori::testing::self_plan({"/bin/sh", "-c", "echo out-line; echo err-line 1>&2; exit 7"},
                                 {{"YORI_TEST_VAR", "hello"}}));
    YORI_CHECK(result);
    if (result) {
      YORI_CHECK(result.identity.valid());
      YORI_CHECK(result.identity.pid > 0);
      YORI_CHECK(result.identity.pgid > 0);
      YORI_CHECK(result.identity.start_ticks > 0);

      const std::string out = read_all(result.stdout_read, std::chrono::milliseconds{3000});
      const std::string err = read_all(result.stderr_read, std::chrono::milliseconds{3000});
      YORI_CHECK(out == "out-line\n");
      YORI_CHECK(err == "err-line\n");

      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.exited());
      YORI_CHECK(exit.status.exit_code == 7);
      // 终态幂等：重复消费同一结果。
      YORI_CHECK(supervisor.consume_exit().exit_code == 7);
      YORI_CHECK(supervisor.consume_exit().exit_code == 7);
      YORI_CHECK(supervisor.poll_exit().exited());
    }
  }

  // ---- 环境一致性：子进程真实环境与 LaunchPlan 一致（含 PATH 查找） -----------
  {
    ProcessSupervisor supervisor;
    const auto result = supervisor.spawn(
        yori::testing::self_plan({"/bin/sh", "-c", "test \"$YORI_TEST_VAR\" = hello && echo ok"},
                                 {{"YORI_TEST_VAR", "hello"}, {"PATH", "/usr/bin:/bin"}}));
    YORI_CHECK(result);
    if (result) {
      const std::string out = read_all(result.stdout_read, std::chrono::milliseconds{3000});
      YORI_CHECK(out == "ok\n");
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.status.is_success());
    }
  }

  // ---- M8 executable 直 exec（DEC-011）：argv[0] 保持用户输入形式 -------------
  {
    ProcessSupervisor supervisor;
    // executable 为绝对路径，argv[0] 为裸名：不做 PATH 搜索，$0 与直接执行一致。
    const auto result = supervisor.spawn(
        yori::testing::self_plan({"sh", "-c", "test \"$0\" = sh && echo direct-exec-ok"},
                                 {{"PATH", "/nonexistent"}}, "", "/bin/sh"));
    YORI_CHECK(result);
    if (result) {
      const std::string out = read_all(result.stdout_read, std::chrono::milliseconds{3000});
      YORI_CHECK(out == "direct-exec-ok\n");
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.status.is_success());
    }
  }
  {
    // executable 不存在/不可执行：结构化 exec 失败（fail-fast 的 daemon 侧防线）。
    ProcessSupervisor supervisor;
    const auto result = supervisor.spawn(
        yori::testing::self_plan({"python", "train.py"}, {}, "", "/nonexistent/python"));
    YORI_CHECK(!result);
    YORI_CHECK(result.code == SpawnErrorCode::kExecFailed);
    YORI_CHECK(result.error_number == ENOENT);
  }

  // ---- cwd 生效 --------------------------------------------------------------
  {
    ProcessSupervisor supervisor;
    const auto result = supervisor.spawn(yori::testing::self_plan({"/bin/pwd"}, {}, "/tmp"));
    YORI_CHECK(result);
    if (result) {
      const std::string out = read_all(result.stdout_read, std::chrono::milliseconds{3000});
      YORI_CHECK(out == "/tmp\n");
      YORI_CHECK(yori::testing::wait_for_exit(supervisor).status.is_success());
    }
  }
  {
    // chdir 失败为结构化结果（任务异常路径）。
    ProcessSupervisor supervisor;
    const auto result =
        supervisor.spawn(yori::testing::self_plan({"true"}, {}, "/nonexistent/dir"));
    YORI_CHECK(!result);
    YORI_CHECK(result.code == SpawnErrorCode::kChdirFailed);
    YORI_CHECK(result.error_number == ENOENT);
  }

  // ---- SIGPIPE 忽略（DEC-008）与信号退出 ------------------------------------
  {
    ProcessSupervisor supervisor;
    // 关闭读端后子进程继续写不致死：读端由 FileDescriptor 立即关闭。
    auto result = supervisor.spawn(yori::testing::self_plan(
        {"/bin/sh", "-c", "for i in 1 2 3 4 5; do echo line-$i; done; echo alive; exit 3"},
        {{"PATH", "/usr/bin:/bin"}}));
    YORI_CHECK(result);
    if (result) {
      // 立即关闭读端，模拟 daemon 崩溃后管道断裂。
      result.stdout_read.reset();
      result.stderr_read.reset();
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.exited());
      YORI_CHECK(exit.status.exit_code == 3);  // 未因 EPIPE/SIGPIPE 死亡。
    }
  }
  {
    // 自信号退出：WIFSIGNALED 语义。
    ProcessSupervisor supervisor;
    const auto result =
        supervisor.spawn(yori::testing::self_plan({"/bin/sh", "-c", "kill -TERM $$"}));
    YORI_CHECK(result);
    if (result) {
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.status.signaled());
      YORI_CHECK(exit.status.signal_number == SIGTERM);
    }
  }

  // ---- 执行中取消：SIGTERM 组信号（DEC-007） ---------------------------------
  {
    ProcessSupervisor supervisor(CancelPolicy{std::chrono::milliseconds{5000}});
    const auto result = supervisor.spawn(yori::testing::self_plan({"sleep", "30"}));
    YORI_CHECK(result);
    if (result) {
      YORI_CHECK(supervisor.phase() == ProcessSupervisor::Phase::kRunning);
      const auto cancel = supervisor.request_cancel();
      YORI_CHECK(cancel.terminating());
      YORI_CHECK(supervisor.phase() == ProcessSupervisor::Phase::kTerminating);
      YORI_CHECK(!supervisor.grace_expired());

      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.status.signaled());
      YORI_CHECK(exit.status.signal_number == SIGTERM);
      // 终态后的取消与升级是显式空操作。
      YORI_CHECK(supervisor.request_cancel().outcome == CancelOutcome::kAlreadyExited);
      YORI_CHECK(supervisor.escalate().outcome == EscalationOutcome::kAlreadyExited);
    }
  }

  // ---- 进程组取消：孙进程一并终止 --------------------------------------------
  {
    ProcessSupervisor supervisor(CancelPolicy{std::chrono::milliseconds{5000}});
    const auto result = supervisor.spawn(yori::testing::self_plan(
        {"/bin/sh", "-c", "sleep 300 & echo $! > /tmp/yori-test-grandchild; wait"}));
    YORI_CHECK(result);
    if (result) {
      // 等待孙进程 PID 写出。
      std::this_thread::sleep_for(std::chrono::milliseconds{300});
      FILE* pid_file = std::fopen("/tmp/yori-test-grandchild", "r");
      YORI_CHECK(pid_file != nullptr);
      long grandchild = 0;
      if (pid_file != nullptr) {
        const int parsed = std::fscanf(pid_file, "%ld", &grandchild);
        std::fclose(pid_file);
        YORI_CHECK(parsed == 1 && grandchild > 0);
        std::remove("/tmp/yori-test-grandchild");
      }
      YORI_CHECK(supervisor.request_cancel().terminating());
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.status.signaled());
      if (grandchild > 0) {
        // 组信号下孙进程同时被终止：kill(pid,0) 返回 ESRCH 前可能有短暂僵尸窗口，
        // 有界重试确认消失。
        bool gone = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
          if (::kill(static_cast<pid_t>(grandchild), 0) < 0 && errno == ESRCH) {
            gone = true;
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        YORI_CHECK(gone);
      }
    }
  }

  // ---- 超时升级：忽略 SIGTERM 的进程组被 SIGKILL（DEC-007） -------------------
  {
    ProcessSupervisor supervisor(CancelPolicy{std::chrono::milliseconds{300}});
    const auto result =
        supervisor.spawn(yori::testing::self_plan({"/bin/sh", "-c", "trap '' TERM; sleep 300"}));
    YORI_CHECK(result);
    if (result) {
      // 等待 shell 安装 TERM 忽略（exec 确认不代表 trap 已执行）。
      std::this_thread::sleep_for(std::chrono::milliseconds{300});
      YORI_CHECK(supervisor.request_cancel().terminating());
      YORI_CHECK(!supervisor.grace_expired());
      // 宽限未到：升级被拒绝。
      YORI_CHECK(supervisor.escalate().outcome == EscalationOutcome::kGraceNotExpired);
      // 等待宽限到期。
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
      while (!supervisor.grace_expired() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
      }
      YORI_CHECK(supervisor.grace_expired());
      const auto escalation = supervisor.escalate();
      YORI_CHECK(escalation.killed());
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.status.signaled());
      YORI_CHECK(exit.status.signal_number == SIGKILL);
    }
  }

  // ---- 身份核验辅助：/proc 启动 ticks ----------------------------------------
  {
    ProcessSupervisor supervisor;
    const auto result = supervisor.spawn(yori::testing::self_plan({"sleep", "1"}));
    YORI_CHECK(result);
    if (result) {
      const auto ticks = read_process_start_ticks(result.identity.pid);
      const auto observed_ticks = ticks.value_or(0);
      YORI_CHECK(ticks.has_value());
      YORI_CHECK(observed_ticks == result.identity.start_ticks);
      YORI_CHECK(!read_process_start_ticks(-1).has_value());
      YORI_CHECK(!read_process_start_ticks(1 << 30).has_value());
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(exit.status.is_success());
    }
  }

  // ---- CancelPolicy 边界 ------------------------------------------------------
  YORI_CHECK(CancelPolicy{}.valid());
  YORI_CHECK(CancelPolicy{std::chrono::milliseconds{99}}.valid() == false);
  YORI_CHECK(CancelPolicy{std::chrono::milliseconds{600001}}.valid() == false);
  YORI_CHECK(CancelPolicy{std::chrono::milliseconds{100}}.valid());
  YORI_CHECK(CancelPolicy{std::chrono::milliseconds{600000}}.valid());

  return yori::testing::failure_count == 0 ? 0 : 1;
}
