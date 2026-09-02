#include <unistd.h>

#include "yori_test.hpp"

// 示例（标签 platform;gpu）：演示依赖真实 NVIDIA GPU 的用例在无 GPU 环境下
// 显式 skip 并给出补跑条件（M0-06）。M3 接入 NVML 后由真实发现/遥测用例替换。
int main() {
  const bool device_present =
      access("/dev/nvidia0", F_OK) == 0 || access("/dev/nvidiactl", F_OK) == 0;
  if (!device_present) {
    YORI_SKIP("需要真实 NVIDIA GPU（NVML）；补跑条件：在带 GPU 的 Linux 主机运行 ctest -L gpu");
  }
  YORI_CHECK(device_present);
  return yori::testing::failure_count == 0 ? 0 : 1;
}
