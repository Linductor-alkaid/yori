#include <yori/version.h>

#include <cstdio>
#include <string_view>

// M0 骨架：仅支持 --version；submit/ps/queue/gpu/cancel/logs 在 M5/M6 经 IPC 接入。
int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::printf("yori %s\n", yori::version());
    return 0;
  }
  std::fprintf(stderr, "usage: yori --version\n");
  return 2;
}
