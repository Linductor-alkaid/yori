#include <unistd.h>

#include <cstdio>

#include "yori/gpu/nvml_gpu_provider.hpp"
#include "yori_test.hpp"

// platform;gpu（M3-03）：经 NvmlGpuProvider 探测真实 NVIDIA GPU（发现、UUID、
// 遥测、外部占用）。无 GPU 环境显式 skip 并给出补跑条件；设备存在但 NVML
// 装载或观测失败视为失败（不冒充通过）。
int main() {
  const bool device_present =
      access("/dev/nvidia0", F_OK) == 0 || access("/dev/nvidiactl", F_OK) == 0;
  if (!device_present) {
    YORI_SKIP("需要真实 NVIDIA GPU（NVML）；补跑条件：在带 GPU 的 Linux 主机运行 ctest -L gpu");
  }

  yori::gpu::NvmlGpuProvider provider;
  const auto load = provider.load();
  if (!load.ok()) {
    std::fprintf(stderr, "nvml load failed: %s (%s)\n", yori::gpu::to_string(load.code),
                 load.detail.c_str());
    YORI_CHECK(false);
    return yori::testing::failure_count == 0 ? 0 : 1;
  }

  const auto result = provider.observe();
  YORI_CHECK(result.ok());
  if (result.ok()) {
    const auto validation = yori::gpu::validate(result.snapshot);
    YORI_CHECK(validation.ok());
    YORI_CHECK(!result.snapshot.devices.empty());
    std::printf("nvml observed %zu device(s), revision %llu\n", result.snapshot.devices.size(),
                static_cast<unsigned long long>(result.snapshot.revision));
    for (const auto& device : result.snapshot.devices) {
      const auto utilization = device.telemetry.utilization_percent.has_value()
                                   ? std::to_string(*device.telemetry.utilization_percent) + "%"
                                   : std::string{"n/a"};
      std::printf("  [%u] %s state=%s util=%s\n", device.index, device.uuid.value().c_str(),
                  yori::gpu::to_string(device.state), utilization.c_str());
    }
  }
  return yori::testing::failure_count == 0 ? 0 : 1;
}
