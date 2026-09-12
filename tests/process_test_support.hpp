#pragma once

// M2 进程守护测试共用 helper：以当前用户身份构造可 spawn 的 LaunchPlan/Identity。
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <yori/launch/launch_adapter.hpp>
#include <yori/process/process_supervisor.hpp>

#include "yori_test.hpp"

namespace yori::testing {

inline launch::IdentityInfo current_identity() {
  std::array<char, 8192> buffer{};
  passwd pwd{};
  passwd* result = nullptr;
  const uid_t uid = ::geteuid();
  YORI_CHECK(::getpwuid_r(uid, &pwd, buffer.data(), buffer.size(), &result) == 0 &&
             result != nullptr);

  launch::IdentityInfo identity;
  identity.uid = static_cast<std::uint32_t>(uid);
  identity.gid = static_cast<std::uint32_t>(::getegid());
  identity.username = pwd.pw_name != nullptr ? pwd.pw_name : "";
  identity.home = pwd.pw_dir != nullptr ? pwd.pw_dir : "";
  identity.shell = pwd.pw_shell != nullptr ? pwd.pw_shell : "";
  return identity;
}

// 以指定 argv 构造"当前有效身份"的 LaunchPlan（uid==euid 时引擎跳过降权）。
// executable 非空时为提交时解析的绝对路径直 exec 语义（DEC-011）。
inline launch::LaunchPlan self_plan(std::vector<std::string> argv,
                                    std::vector<launch::EnvironmentEntry> env = {},
                                    std::string cwd = "", std::string executable = "") {
  launch::LaunchPlan plan;
  plan.argv = std::move(argv);
  plan.env = std::move(env);
  plan.cwd = std::move(cwd);
  plan.executable = std::move(executable);
  plan.uid = static_cast<std::uint32_t>(::geteuid());
  plan.gid = static_cast<std::uint32_t>(::getegid());
  launch::IdentityInfo identity = current_identity();
  plan.username = identity.username;
  plan.supplementary_groups = {identity.gid};
  return plan;
}

// 有界等待 poll_exit 返回退出（默认 10 秒）。
inline process::ExitPollResult wait_for_exit(
    process::ProcessSupervisor& supervisor,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{10000}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = supervisor.poll_exit();
    if (result.outcome == process::ExitPollOutcome::kExited ||
        result.outcome == process::ExitPollOutcome::kWaitFailed) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return supervisor.poll_exit();
}

}  // namespace yori::testing
