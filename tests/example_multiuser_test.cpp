#include <pwd.h>
#include <unistd.h>

#include "yori_test.hpp"

// 示例（标签 security;multi-user）：演示依赖 root 与第二个 Linux 用户的用例的
// skip 行为（M0-06）。真实多用户隔离用例（exec 前降权、SO_PEERCRED 鉴权）在
// M2/M5 落地。
int main() {
  if (geteuid() != 0) {
    YORI_SKIP("需要以 root 运行才能验证多用户路径；补跑条件：sudo ctest -L multi-user");
  }
  const bool has_second_user = getpwnam("nobody") != nullptr;
  if (!has_second_user) {
    YORI_SKIP(
        "系统缺少第二个可用 Linux 用户（nobody）；补跑条件：在含 nobody 账户的环境运行 ctest -L "
        "multi-user");
  }
  YORI_CHECK(has_second_user);
  return yori::testing::failure_count == 0 ? 0 : 1;
}
