#include <yori/version.h>

#include <cstdio>

// 安装后最小 consumer（M0-05）：验证安装的公共头与导出库可用。
int main() {
  std::printf("consumer linked against yori %s\n", yori::version());
  return 0;
}
