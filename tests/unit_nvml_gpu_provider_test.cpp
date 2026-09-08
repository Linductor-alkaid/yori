// NvmlGpuProvider 适配器单测（M3-01/M3-03）：经可控 stub NVML 共享库验证
// dlopen 绑定、init/观测错误映射、发现与 UUID 身份、遥测省略、外部占用判定、
// v3→v2 回退与 revision 单调。全部在测试主线程同步调用 observe()（stub 的
// 单线程线律）。

#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gpu/nvml_api.hpp"
#include "yori/gpu/nvml_gpu_provider.hpp"
#include "yori_test.hpp"

namespace {

#ifndef YORI_STUB_NVML_PATH
#error "YORI_STUB_NVML_PATH must be defined to the stub library path"
#endif

constexpr const char* kStubPath = YORI_STUB_NVML_PATH;

// stub 控制接口句柄：进程内首次使用时 dlopen（与 Provider 的 dlopen 返回同一
// 对象），符号经 dlsym 解析。
struct StubControl {
  void (*reset)(){};
  void (*set_init_error)(int){};
  void (*set_count_error)(int){};
  void (*set_v3_missing)(int){};
  int (*init_calls)(){};
  int (*shutdown_calls)(){};
  int (*process_v2_calls)(){};
  void (*set_device_count)(unsigned int){};
  void (*set_device)(unsigned int, const char*){};
  void (*remove_device)(unsigned int){};
  void (*set_uuid_error)(unsigned int, int){};
  void (*set_utilization)(unsigned int, int){};
  void (*set_memory)(unsigned int, unsigned long long, unsigned long long, int){};
  void (*set_compute_processes)(unsigned int, int){};

  bool valid() const noexcept { return reset != nullptr; }
};

StubControl load_stub_control() {
  StubControl control{};
  void* handle = ::dlopen(kStubPath, RTLD_NOW);
  if (handle == nullptr) {
    std::fprintf(stderr, "failed to open stub nvml: %s\n", ::dlerror());
    return control;
  }
  control.reset = reinterpret_cast<void (*)()>(::dlsym(handle, "stub_nvml_reset"));
  control.set_init_error =
      reinterpret_cast<void (*)(int)>(::dlsym(handle, "stub_nvml_set_init_error"));
  control.set_count_error =
      reinterpret_cast<void (*)(int)>(::dlsym(handle, "stub_nvml_set_count_error"));
  control.set_v3_missing =
      reinterpret_cast<void (*)(int)>(::dlsym(handle, "stub_nvml_set_v3_missing"));
  control.init_calls = reinterpret_cast<int (*)()>(::dlsym(handle, "stub_nvml_init_calls"));
  control.shutdown_calls = reinterpret_cast<int (*)()>(::dlsym(handle, "stub_nvml_shutdown_calls"));
  control.process_v2_calls =
      reinterpret_cast<int (*)()>(::dlsym(handle, "stub_nvml_process_v2_calls"));
  control.set_device_count =
      reinterpret_cast<void (*)(unsigned int)>(::dlsym(handle, "stub_nvml_set_device_count"));
  control.set_device = reinterpret_cast<void (*)(unsigned int, const char*)>(
      ::dlsym(handle, "stub_nvml_set_device"));
  control.remove_device =
      reinterpret_cast<void (*)(unsigned int)>(::dlsym(handle, "stub_nvml_remove_device"));
  control.set_uuid_error =
      reinterpret_cast<void (*)(unsigned int, int)>(::dlsym(handle, "stub_nvml_set_uuid_error"));
  control.set_utilization =
      reinterpret_cast<void (*)(unsigned int, int)>(::dlsym(handle, "stub_nvml_set_utilization"));
  control.set_memory =
      reinterpret_cast<void (*)(unsigned int, unsigned long long, unsigned long long, int)>(
          ::dlsym(handle, "stub_nvml_set_memory"));
  control.set_compute_processes = reinterpret_cast<void (*)(unsigned int, int)>(
      ::dlsym(handle, "stub_nvml_set_compute_processes"));
  return control;
}

StubControl stub() {
  static StubControl control = load_stub_control();
  return control;
}

yori::gpu::NvmlGpuProvider make_provider() {
  yori::gpu::NvmlGpuProviderConfig config;
  config.library_path = kStubPath;
  return yori::gpu::NvmlGpuProvider{std::move(config)};
}

void seed_two_devices() {
  const auto control = stub();
  control.set_device_count(2);
  control.set_device(0, "GPU-stub-a");
  control.set_device(1, "GPU-stub-b");
  control.set_utilization(0, 7);
  control.set_utilization(1, 0);
  control.set_memory(0, 24ULL * 1024 * 1024 * 1024, 1024, 0);
  control.set_memory(1, 24ULL * 1024 * 1024 * 1024, 0, 0);
  control.set_compute_processes(0, 0);
  control.set_compute_processes(1, 0);
}

void test_load_and_observe() {
  const auto control = stub();
  control.reset();
  seed_two_devices();

  auto provider = make_provider();
  YORI_CHECK(!provider.loaded());
  const auto before_load = provider.observe();
  YORI_CHECK(before_load.code == yori::gpu::GpuProviderErrorCode::kBackendUnavailable);

  const auto load_result = provider.load();
  YORI_CHECK(load_result.ok());
  YORI_CHECK(load_result.code == yori::gpu::NvmlProviderLoadCode::kLoaded);
  YORI_CHECK(provider.loaded());
  YORI_CHECK(control.init_calls() == 1);

  const auto again = provider.load();
  YORI_CHECK(again.code == yori::gpu::NvmlProviderLoadCode::kAlreadyLoaded);

  const auto result = provider.observe();
  YORI_CHECK(result.ok());
  YORI_CHECK(result.snapshot.revision == 1);
  YORI_CHECK(result.snapshot.observed_at > std::chrono::system_clock::time_point{});
  YORI_CHECK(result.snapshot.devices.size() == 2);
  YORI_CHECK(result.snapshot.devices[0].uuid == yori::gpu::GpuUuid{"GPU-stub-a"});
  YORI_CHECK(result.snapshot.devices[0].index == 0);
  YORI_CHECK(result.snapshot.devices[0].state == yori::gpu::GpuObservedState::kFree);
  YORI_CHECK(result.snapshot.devices[0].telemetry.utilization_percent == std::uint32_t{7});
  YORI_CHECK(result.snapshot.devices[0].telemetry.memory_total_bytes == 24ULL * 1024 * 1024 * 1024);
  YORI_CHECK(result.snapshot.devices[0].telemetry.memory_used_bytes == 1024);
  YORI_CHECK(result.snapshot.devices[1].state == yori::gpu::GpuObservedState::kFree);

  const auto second = provider.observe();
  YORI_CHECK(second.ok());
  YORI_CHECK(second.snapshot.revision == 2);
  YORI_CHECK(second.snapshot.devices.size() == 2);
}

void test_load_error_mapping() {
  const auto control = stub();

  // dlopen 失败 → kLibraryOpenFailed。
  {
    yori::gpu::NvmlGpuProviderConfig config;
    config.library_path = "/nonexistent/libstub-nvml.so";
    yori::gpu::NvmlGpuProvider provider{std::move(config)};
    const auto result = provider.load();
    YORI_CHECK(result.code == yori::gpu::NvmlProviderLoadCode::kLibraryOpenFailed);
    YORI_CHECK(!result.detail.empty());
    YORI_CHECK(!provider.loaded());
  }

  // 驱动未装载 → kBackendUnavailable。
  {
    control.reset();
    control.set_init_error(NVML_ERROR_DRIVER_NOT_LOADED);
    auto provider = make_provider();
    const auto result = provider.load();
    YORI_CHECK(result.code == yori::gpu::NvmlProviderLoadCode::kBackendUnavailable);
    YORI_CHECK(!provider.loaded());
  }

  // 无权限 → kPermissionDenied。
  {
    control.reset();
    control.set_init_error(NVML_ERROR_NO_PERMISSION);
    auto provider = make_provider();
    const auto result = provider.load();
    YORI_CHECK(result.code == yori::gpu::NvmlProviderLoadCode::kPermissionDenied);
  }

  // 其余 init 失败 → kInitFailed。
  {
    control.reset();
    control.set_init_error(NVML_ERROR_UNKNOWN);
    auto provider = make_provider();
    const auto result = provider.load();
    YORI_CHECK(result.code == yori::gpu::NvmlProviderLoadCode::kInitFailed);
  }

  // 非法配置 → kInvalidConfig。
  {
    yori::gpu::NvmlGpuProviderConfig config;
    config.library_path = "";
    yori::gpu::NvmlGpuProvider provider{std::move(config)};
    const auto result = provider.load();
    YORI_CHECK(result.code == yori::gpu::NvmlProviderLoadCode::kInvalidConfig);
  }
}

void test_observe_error_mapping() {
  const auto control = stub();

  // 计数错误：驱动未装载类 → 观测级 backend unavailable。
  {
    control.reset();
    control.set_init_error(0);
    seed_two_devices();
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    control.set_count_error(NVML_ERROR_UNINITIALIZED);
    const auto result = provider.observe();
    YORI_CHECK(result.code == yori::gpu::GpuProviderErrorCode::kBackendUnavailable);
    control.set_count_error(0);
  }

  // 计数无权限 → kPermissionDenied。
  {
    control.reset();
    seed_two_devices();
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    control.set_count_error(NVML_ERROR_NO_PERMISSION);
    const auto result = provider.observe();
    YORI_CHECK(result.code == yori::gpu::GpuProviderErrorCode::kPermissionDenied);
  }

  // 设备句柄失败（计数含不存在设备）→ 整次观测失败。
  {
    control.reset();
    control.set_device_count(3);
    control.set_device(0, "GPU-stub-a");
    control.set_device(1, "GPU-stub-b");
    control.set_compute_processes(0, 0);
    control.set_compute_processes(1, 0);
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    const auto result = provider.observe();
    YORI_CHECK(result.code == yori::gpu::GpuProviderErrorCode::kObservationFailed);
    YORI_CHECK(result.snapshot.devices.empty());
  }

  // UUID 不可读（GPU_IS_LOST）→ 整次观测失败，不产出部分快照。
  {
    control.reset();
    seed_two_devices();
    control.set_uuid_error(1, NVML_ERROR_GPU_IS_LOST);
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    const auto result = provider.observe();
    YORI_CHECK(result.code == yori::gpu::GpuProviderErrorCode::kObservationFailed);
    YORI_CHECK(result.snapshot.devices.empty());
  }
}

void test_telemetry_omission() {
  const auto control = stub();
  control.reset();
  seed_two_devices();
  // utilization 查询失败（-1）→ 字段省略；memory 返回错误 → 字段省略。
  control.set_utilization(0, -1);
  control.set_memory(0, 0, 0, NVML_ERROR_NOT_SUPPORTED);

  auto provider = make_provider();
  YORI_CHECK(provider.load().ok());
  const auto result = provider.observe();
  YORI_CHECK(result.ok());
  YORI_CHECK(!result.snapshot.devices[0].telemetry.utilization_percent.has_value());
  YORI_CHECK(!result.snapshot.devices[0].telemetry.memory_total_bytes.has_value());
  YORI_CHECK(!result.snapshot.devices[0].telemetry.memory_used_bytes.has_value());
  // 设备 1 的遥测正常。
  YORI_CHECK(result.snapshot.devices[1].telemetry.utilization_percent == std::uint32_t{0});
}

void test_external_occupancy() {
  const auto control = stub();

  // 有计算进程 → EXTERNAL_BUSY（v3 路径）。
  {
    control.reset();
    seed_two_devices();
    control.set_compute_processes(1, 4);
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    const auto result = provider.observe();
    YORI_CHECK(result.ok());
    YORI_CHECK(result.snapshot.devices[0].state == yori::gpu::GpuObservedState::kFree);
    YORI_CHECK(result.snapshot.devices[1].state == yori::gpu::GpuObservedState::kExternalBusy);
  }

  // v3 符号缺失（旧驱动）→ 回退 v2，语义不变。
  {
    control.reset();
    seed_two_devices();
    control.set_compute_processes(1, 2);
    control.set_v3_missing(1);
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    const auto result = provider.observe();
    YORI_CHECK(result.ok());
    YORI_CHECK(result.snapshot.devices[1].state == yori::gpu::GpuObservedState::kExternalBusy);
    YORI_CHECK(control.process_v2_calls() >= 1);
  }

  // 占用查询 NOT_SUPPORTED → 设备 UNAVAILABLE（按策略不可调度）。
  {
    control.reset();
    seed_two_devices();
    control.set_compute_processes(1, -NVML_ERROR_NOT_SUPPORTED);
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    const auto result = provider.observe();
    YORI_CHECK(result.ok());
    YORI_CHECK(result.snapshot.devices[1].state == yori::gpu::GpuObservedState::kUnavailable);
  }

  // 占用查询 GPU_IS_LOST → 设备 UNAVAILABLE。
  {
    control.reset();
    seed_two_devices();
    control.set_compute_processes(1, -NVML_ERROR_GPU_IS_LOST);
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    const auto result = provider.observe();
    YORI_CHECK(result.ok());
    YORI_CHECK(result.snapshot.devices[1].state == yori::gpu::GpuObservedState::kUnavailable);
  }

  // 占用查询无权限 → 整次观测 kPermissionDenied。
  {
    control.reset();
    seed_two_devices();
    control.set_compute_processes(1, -NVML_ERROR_NO_PERMISSION);
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    const auto result = provider.observe();
    YORI_CHECK(result.code == yori::gpu::GpuProviderErrorCode::kPermissionDenied);
  }
}

void test_device_count_overflow() {
  const auto control = stub();
  control.reset();
  // 超出快照设备上限：整次失败而非截断。
  control.set_device_count(1000);
  auto provider = make_provider();
  YORI_CHECK(provider.load().ok());
  const auto result = provider.observe();
  YORI_CHECK(result.code == yori::gpu::GpuProviderErrorCode::kObservationFailed);
}

void test_shutdown_on_destruction() {
  const auto control = stub();
  control.reset();
  seed_two_devices();
  {
    auto provider = make_provider();
    YORI_CHECK(provider.load().ok());
    YORI_CHECK(control.shutdown_calls() == 0);
  }
  YORI_CHECK(control.shutdown_calls() == 1);
}

}  // namespace

int main() {
  const auto control = stub();
  if (!control.valid()) {
    std::fprintf(stderr, "stub nvml control interface unavailable\n");
    return 1;
  }
  test_load_and_observe();
  test_load_error_mapping();
  test_observe_error_mapping();
  test_telemetry_omission();
  test_external_occupancy();
  test_device_count_overflow();
  test_shutdown_on_destruction();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "nvml gpu provider test failures: %d\n", yori::testing::failure_count);
  }
  return yori::testing::failure_count == 0 ? 0 : 1;
}
