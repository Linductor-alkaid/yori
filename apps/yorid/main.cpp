#include <yori/version.h>

#include <cstdio>
#include <string_view>

// M0 骨架：仅支持 --version；daemon 主生命周期（Executor owner、恢复与调度）
// 自 M1 起实现。
int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::printf("yorid %s\n", yori::version());
    return 0;
  }
  std::fprintf(stderr, "usage: yorid --version\n");
  return 2;
}
