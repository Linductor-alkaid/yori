#pragma once

// 内部最小 NVML C API 声明（M3-01）。只声明 NvmlGpuProvider 实际绑定的符号与
// 只读字段：真实驱动的结构体布局以 NVIDIA 公开 NVML 文档的稳定子集为准，
// 适配器不读取任何进程/设备结构体的内部字段，仅消费返回码与计数，避免布局
// 版本差异。该头不安装、不得被公共头包含；tests/stub_nvml 与本适配器共享
// 同一声明以保证配对 ABI 一致。

extern "C" {

using nvmlDevice_t = struct nvmlDevice_st*;

// NVML 返回码子集（数值以驱动导出为准）。
enum nvmlReturn_t {
  NVML_SUCCESS = 0,
  NVML_ERROR_UNINITIALIZED = 1,
  NVML_ERROR_INVALID_ARGUMENT = 2,
  NVML_ERROR_NOT_SUPPORTED = 3,
  NVML_ERROR_NO_PERMISSION = 4,
  NVML_ERROR_NOT_FOUND = 6,
  NVML_ERROR_INSUFFICIENT_SIZE = 7,
  NVML_ERROR_DRIVER_NOT_LOADED = 8,
  NVML_ERROR_TIMEOUT = 9,
  NVML_ERROR_LIBRARY_NOT_FOUND = 11,
  NVML_ERROR_FUNCTION_NOT_FOUND = 12,
  NVML_ERROR_GPU_IS_LOST = 14,
  NVML_ERROR_UNKNOWN = 999,
};

// 遥测结构体按现代驱动布局声明（NVML 5.5 起稳定）；只读取 gpu/total/used。
struct nvmlUtilization_t {
  unsigned int gpu;
  unsigned int memory;
  unsigned int encoder;
  unsigned int decoder;
};

struct nvmlMemory_t {
  unsigned long long total;
  unsigned long long used;
  unsigned long long free;
};

// 进程条目只作为容量探测的占位：适配器以 count=0 + null 缓冲查询所需条数，
// 从不读取条目字段（威胁模型基线 10：不采集外部进程身份）。
struct nvmlProcessInfo_t {
  unsigned int pid;
  unsigned long long usedGpuMemory;
};

nvmlReturn_t nvmlInit_v2(void);
nvmlReturn_t nvmlShutdown(void);
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int* deviceCount);
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t* device);
nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char* uuid, unsigned int length);
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t* utilization);
nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t* memory);
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t device, unsigned int* infoCount,
                                                     nvmlProcessInfo_t* infos);
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v2(nvmlDevice_t device, unsigned int* infoCount,
                                                     nvmlProcessInfo_t* infos);
const char* nvmlErrorString(nvmlReturn_t result);

}  // extern "C"
