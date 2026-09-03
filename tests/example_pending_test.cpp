#include <cstdio>

// 示例占位（ipc/recovery/fuzz/performance 标签载体）：对应里程碑的真实用例
// 合入前以显式 skip 呈现，不以“无测试可运行”冒充成功（M0-06）。
int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: example_pending_test <skip-reason>\n");
    return 2;
  }
  std::printf("SKIP: %s\n", argv[1]);
  return 77;
}
