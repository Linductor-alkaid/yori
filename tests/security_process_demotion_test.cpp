#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>
#include <yori/launch/launch_adapter.hpp>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "yori_test.hpp"

// DEC-004 的 M2 验证：root daemon exec 前降权。需要 root 与一个非 root 目标用户；
// 非 root 环境显式 skip（退出码 77），补跑条件：以 root 运行并设置
// YORI_DEMOTION_TEST_UID / YORI_DEMOTION_TEST_GID 指向真实非特权用户。

namespace {

using namespace yori;
using namespace yori::process;

[[maybe_unused]] std::string read_all(const FileDescriptor& fd, std::chrono::milliseconds timeout) {
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
  if (::geteuid() != 0) {
    YORI_SKIP(
        "非 root 环境无法验证降权；补跑条件：root 下运行并设置 "
        "YORI_DEMOTION_TEST_UID/YORI_DEMOTION_TEST_GID（security;multi-user 标签）");
  }
  const char* uid_text = std::getenv("YORI_DEMOTION_TEST_UID");
  const char* gid_text = std::getenv("YORI_DEMOTION_TEST_GID");
  if (uid_text == nullptr || gid_text == nullptr) {
    YORI_SKIP(
        "未设置 YORI_DEMOTION_TEST_UID/YORI_DEMOTION_TEST_GID；补跑条件：root 下指定"
        " 一个真实非特权用户的 uid/gid 后运行 ctest -L multi-user");
  }

  const std::uint32_t target_uid = static_cast<std::uint32_t>(std::strtoul(uid_text, nullptr, 10));
  const std::uint32_t target_gid = static_cast<std::uint32_t>(std::strtoul(gid_text, nullptr, 10));
  YORI_CHECK(target_uid != 0 && target_gid != 0);

  launch::PosixIdentityResolver resolver;
  const auto resolved = resolver.resolve(target_uid);
  YORI_CHECK(resolved.ok());

  // 降权成功 + 身份断言：子进程 euid/egid/groups 与目标一致，产出文件 owner 正确。
  ProcessSupervisor supervisor;
  launch::LaunchPlan plan;
  plan.argv = {"/bin/sh", "-c", "id -u; id -g; touch demotion-proof.txt; echo done"};
  plan.cwd = "/tmp";
  plan.uid = target_uid;
  plan.gid = target_gid;
  plan.username = resolved.ok() ? resolved.identity.username : "nobody";
  plan.supplementary_groups = resolved.ok() ? resolved.identity.supplementary_groups
                                            : std::vector<std::uint32_t>{target_gid};
  plan.env = {{"PATH", "/usr/bin:/bin"}};

  auto spawned = supervisor.spawn(plan);
  YORI_CHECK(spawned);
  if (spawned) {
    const std::string out = read_all(spawned.stdout_read, std::chrono::milliseconds{5000});
    YORI_CHECK(out.find(std::to_string(target_uid) + "\n") != std::string::npos);
    YORI_CHECK(out.find(std::to_string(target_gid) + "\n") != std::string::npos);
    YORI_CHECK(out.find("done\n") != std::string::npos);

    const auto exit = yori::testing::wait_for_exit(supervisor);
    YORI_CHECK(exit.status.is_success());

    const std::string proof = "/tmp/demotion-proof.txt";
    struct stat st {};
    YORI_CHECK(::stat(proof.c_str(), &st) == 0);
    YORI_CHECK(static_cast<std::uint32_t>(st.st_uid) == target_uid);
    YORI_CHECK(static_cast<std::uint32_t>(st.st_gid) == target_gid);
    static_cast<void>(::unlink(proof.c_str()));
  }

  // 降权失败路径：不存在的 uid 结构化失败，绝不以 root 执行。
  launch::LaunchPlan broken = plan;
  broken.uid = 60000;
  broken.gid = 60000;
  broken.username = "no-such-user-xyz";
  broken.supplementary_groups = {60000};
  const auto rejected = supervisor.spawn(broken);
  YORI_CHECK(!rejected);
  YORI_CHECK(rejected.code == SpawnErrorCode::kIdentitySwitchFailed);

  return yori::testing::failure_count == 0 ? 0 : 1;
}
