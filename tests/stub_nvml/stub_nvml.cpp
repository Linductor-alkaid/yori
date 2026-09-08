// 可控 stub NVML 共享库（M3-03）：导出 NvmlGpuProvider 绑定的 NVML 符号子集与
// `stub_nvml_*` 控制接口。测试进程对同一 .so 的重复 dlopen 返回同一对象，
// 因此测试可经 dlsym 取控制符号注入设备表、遥测与逐调用错误。
//
// 线律：本库为单线程测试夹具，只服务于适配器单测（observe() 由测试主线程直接
// 调用）；跨线程的采样路径测试使用 yori::testing::FakeGpuProvider/原子注入，
// 不经本库。每个用例先 stub_nvml_reset()。

#include <cstdint>
#include <cstring>

#include "gpu/nvml_api.hpp"

namespace {

constexpr unsigned int kStubMaxDevices = 16;
constexpr unsigned int kStubUuidSize = 96;

struct StubDevice {
  bool present{false};
  char uuid[kStubUuidSize] = {};
  int utilization{-1};  // -1 = 查询失败
  unsigned long long memory_total{0};
  unsigned long long memory_used{0};
  int memory_error{0};       // 0 = 成功；否则返回该 nvmlReturn_t
  int compute_processes{0};  // <0 = 查询返回 -compute_processes 的错误码
  int uuid_error{0};
};

struct StubState {
  bool initialized{false};
  int init_error{0};
  int count_error{0};
  unsigned int device_count{0};
  StubDevice devices[kStubMaxDevices];
  int init_calls{0};
  int shutdown_calls{0};
  int uuid_v2_calls{0};  // nvmlDeviceGetComputeRunningProcesses_v2 调用计数
  int v3_missing{0};     // 1 = v3 符号表现为 FUNCTION_NOT_FOUND（回退路径）
};

StubState g_state;

// 句柄编码为 index+1（0 保留为 null，避免索引 0 的句柄与 nullptr 混淆）。
StubDevice* device_by_handle(nvmlDevice_t device) {
  const auto encoded = reinterpret_cast<std::uintptr_t>(device);
  if (device == nullptr || encoded == 0 || encoded > kStubMaxDevices ||
      !g_state.devices[encoded - 1].present) {
    return nullptr;
  }
  return &g_state.devices[encoded - 1];
}

}  // namespace

extern "C" {

// ---- 控制接口 -------------------------------------------------------------

void stub_nvml_reset(void) { g_state = StubState{}; }

void stub_nvml_set_init_error(int error) { g_state.init_error = error; }
void stub_nvml_set_count_error(int error) { g_state.count_error = error; }
void stub_nvml_set_v3_missing(int missing) { g_state.v3_missing = missing; }
int stub_nvml_init_calls(void) { return g_state.init_calls; }
int stub_nvml_shutdown_calls(void) { return g_state.shutdown_calls; }
int stub_nvml_process_v2_calls(void) { return g_state.uuid_v2_calls; }

void stub_nvml_set_device_count(unsigned int count) { g_state.device_count = count; }

// index 超界设备静默置为不存在的设备（适配器观察 GetHandle 失败路径用 -1 呈现）。
void stub_nvml_set_device(unsigned int index, const char* uuid) {
  if (index >= kStubMaxDevices) {
    return;
  }
  auto& device = g_state.devices[index];
  device.present = true;
  std::memset(device.uuid, 0, sizeof(device.uuid));
  if (uuid != nullptr) {
    std::strncpy(device.uuid, uuid, sizeof(device.uuid) - 1);
  }
}

void stub_nvml_remove_device(unsigned int index) {
  if (index < kStubMaxDevices) {
    g_state.devices[index].present = false;
  }
}

void stub_nvml_set_uuid_error(unsigned int index, int error) {
  if (index < kStubMaxDevices) {
    g_state.devices[index].uuid_error = error;
  }
}

void stub_nvml_set_utilization(unsigned int index, int percent) {
  if (index < kStubMaxDevices) {
    g_state.devices[index].utilization = percent;
  }
}

void stub_nvml_set_memory(unsigned int index, unsigned long long total, unsigned long long used,
                          int error) {
  if (index < kStubMaxDevices) {
    g_state.devices[index].memory_total = total;
    g_state.devices[index].memory_used = used;
    g_state.devices[index].memory_error = error;
  }
}

// compute_processes：>0 映射 EXTERNAL_BUSY；0 映射 FREE；<0 时占用查询返回
// 该值的相反数错误码（-3 = NOT_SUPPORTED，-14 = GPU_IS_LOST，-4 = NO_PERMISSION…）。
void stub_nvml_set_compute_processes(unsigned int index, int processes) {
  if (index < kStubMaxDevices) {
    g_state.devices[index].compute_processes = processes;
  }
}

// ---- NVML API -------------------------------------------------------------

nvmlReturn_t nvmlInit_v2(void) {
  ++g_state.init_calls;
  if (g_state.init_error != NVML_SUCCESS) {
    return static_cast<nvmlReturn_t>(g_state.init_error);
  }
  g_state.initialized = true;
  return NVML_SUCCESS;
}

nvmlReturn_t nvmlShutdown(void) {
  ++g_state.shutdown_calls;
  g_state.initialized = false;
  return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int* device_count) {
  if (device_count == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  if (!g_state.initialized) {
    return NVML_ERROR_UNINITIALIZED;
  }
  if (g_state.count_error != NVML_SUCCESS) {
    return static_cast<nvmlReturn_t>(g_state.count_error);
  }
  *device_count = g_state.device_count;
  return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t* device) {
  if (device == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  if (!g_state.initialized) {
    return NVML_ERROR_UNINITIALIZED;
  }
  if (index >= kStubMaxDevices || !g_state.devices[index].present) {
    return NVML_ERROR_NOT_FOUND;
  }
  *device = reinterpret_cast<nvmlDevice_t>(static_cast<std::uintptr_t>(index + 1));
  return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char* uuid, unsigned int length) {
  if (uuid == nullptr || length == 0) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  const StubDevice* target = device_by_handle(device);
  if (target == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  if (target->uuid_error != NVML_SUCCESS) {
    return static_cast<nvmlReturn_t>(target->uuid_error);
  }
  const auto source_length = static_cast<unsigned int>(std::strlen(target->uuid));
  if (source_length >= length) {
    return NVML_ERROR_INSUFFICIENT_SIZE;
  }
  std::memcpy(uuid, target->uuid, source_length + 1);
  return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t* utilization) {
  if (utilization == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  const StubDevice* target = device_by_handle(device);
  if (target == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  if (target->utilization < 0) {
    return NVML_ERROR_NOT_SUPPORTED;
  }
  utilization->gpu = static_cast<unsigned int>(target->utilization);
  utilization->memory = 0;
  utilization->encoder = 0;
  utilization->decoder = 0;
  return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t* memory) {
  if (memory == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  const StubDevice* target = device_by_handle(device);
  if (target == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  if (target->memory_error != NVML_SUCCESS) {
    return static_cast<nvmlReturn_t>(target->memory_error);
  }
  memory->total = target->memory_total;
  memory->used = target->memory_used;
  memory->free = target->memory_total - target->memory_used;
  return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t device, unsigned int* info_count,
                                                     nvmlProcessInfo_t* infos) {
  if (g_state.v3_missing != 0) {
    return NVML_ERROR_FUNCTION_NOT_FOUND;
  }
  // v3 与 v2 共用同一注入状态（计数语义一致）。
  return nvmlDeviceGetComputeRunningProcesses_v2(device, info_count, infos);
}

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v2(nvmlDevice_t device, unsigned int* info_count,
                                                     nvmlProcessInfo_t* infos) {
  ++g_state.uuid_v2_calls;
  if (info_count == nullptr || infos != nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  const StubDevice* target = device_by_handle(device);
  if (target == nullptr) {
    return NVML_ERROR_INVALID_ARGUMENT;
  }
  if (target->compute_processes < 0) {
    return static_cast<nvmlReturn_t>(-target->compute_processes);
  }
  if (target->compute_processes > 0) {
    // 适配器只以 count=0 + null 缓冲探测：有进程即 INSUFFICIENT_SIZE。
    *info_count = static_cast<unsigned int>(target->compute_processes);
    return NVML_ERROR_INSUFFICIENT_SIZE;
  }
  *info_count = 0;
  return NVML_SUCCESS;
}

const char* nvmlErrorString(nvmlReturn_t result) {
  switch (result) {
    case NVML_SUCCESS:
      return "success";
    case NVML_ERROR_UNINITIALIZED:
      return "uninitialized";
    case NVML_ERROR_INVALID_ARGUMENT:
      return "invalid argument";
    case NVML_ERROR_NOT_SUPPORTED:
      return "not supported";
    case NVML_ERROR_NO_PERMISSION:
      return "no permission";
    case NVML_ERROR_INSUFFICIENT_SIZE:
      return "insufficient size";
    case NVML_ERROR_DRIVER_NOT_LOADED:
      return "driver not loaded";
    case NVML_ERROR_LIBRARY_NOT_FOUND:
      return "library not found";
    case NVML_ERROR_FUNCTION_NOT_FOUND:
      return "function not found";
    case NVML_ERROR_GPU_IS_LOST:
      return "gpu is lost";
    default:
      return "unknown";
  }
}

}  // extern "C"
