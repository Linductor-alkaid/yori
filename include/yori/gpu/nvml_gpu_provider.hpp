#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <yori/gpu/gpu_provider.hpp>

namespace yori::gpu {

// NVML 适配配置。library_path 只能来自管理员配置（默认系统 NVML 库名）或
// 测试注入的显式 stub 路径，不得接受用户输入。
struct NvmlGpuProviderConfig final {
  std::string library_path{"libnvidia-ml.so.1"};
  std::size_t max_devices{GpuObservationSnapshot::kMaxDevices};
};

enum class NvmlProviderLoadCode {
  kLoaded,
  kAlreadyLoaded,
  kInvalidConfig,
  kLibraryOpenFailed,
  kSymbolMissing,
  kBackendUnavailable,
  kPermissionDenied,
  kInitFailed,
};

[[nodiscard]] const char* to_string(NvmlProviderLoadCode code) noexcept;

struct NvmlProviderLoadResult final {
  NvmlProviderLoadCode code{NvmlProviderLoadCode::kLoaded};
  std::string detail;

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == NvmlProviderLoadCode::kLoaded || code == NvmlProviderLoadCode::kAlreadyLoaded;
  }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// GpuProvider 的 NVML 适配（Adapter 层）。实现 M1 冻结的同步 observe() SPI：
// 设备发现、UUID 身份、utilization/显存遥测（失败仅省略对应字段）与外部计算
// 进程占用检测（EXTERNAL_BUSY，只依据进程计数，不读取进程身份）。Provider
// 不隐藏线程、timer 或队列；周期采样由 GpuManager 经 Executor 承载。
//
// 生命周期纪律：load() 由 owner 线程调用一次；observe() 线程安全（NVML 自身
// 线程安全，revision 原子递增）；析构前 owner 必须保证没有并发 observe()
// （GpuManager 的停止顺序先行取消周期任务）。
class NvmlGpuProvider final : public GpuProvider {
 public:
  explicit NvmlGpuProvider(NvmlGpuProviderConfig config = {});
  ~NvmlGpuProvider() override;

  NvmlGpuProvider(const NvmlGpuProvider&) = delete;
  NvmlGpuProvider& operator=(const NvmlGpuProvider&) = delete;
  NvmlGpuProvider(NvmlGpuProvider&&) = delete;
  NvmlGpuProvider& operator=(NvmlGpuProvider&&) = delete;

  // 显式初始化：dlopen + dlsym + nvmlInit_v2。失败时保持未加载状态，detail
  // 携带 nvmlErrorString 或 dlerror 摘要；未 load 的 observe() 返回
  // kBackendUnavailable。
  [[nodiscard]] NvmlProviderLoadResult load();

  [[nodiscard]] bool loaded() const noexcept;

  [[nodiscard]] GpuProviderResult observe() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::gpu
