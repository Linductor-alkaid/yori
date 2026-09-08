#include "yori/gpu/nvml_gpu_provider.hpp"

#include <dlfcn.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "gpu/nvml_api.hpp"

namespace yori::gpu {
namespace {

// load() 完成后只读的符号表；observe() 只消费返回码与计数。
struct NvmlSymbols final {
  nvmlReturn_t (*init)();
  nvmlReturn_t (*shutdown)();
  nvmlReturn_t (*device_get_count)(unsigned int*);
  nvmlReturn_t (*device_get_handle)(unsigned int, nvmlDevice_t*);
  nvmlReturn_t (*device_get_uuid)(nvmlDevice_t, char*, unsigned int);
  nvmlReturn_t (*device_get_utilization)(nvmlDevice_t, nvmlUtilization_t*);
  nvmlReturn_t (*device_get_memory)(nvmlDevice_t, nvmlMemory_t*);
  nvmlReturn_t (*device_get_compute_processes_v3)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*);
  nvmlReturn_t (*device_get_compute_processes_v2)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*);
  const char* (*error_string)(nvmlReturn_t);
};

// detail 截断上限：dlerror/nvmlErrorString 只做诊断摘要，不进入结构化错误。
constexpr std::size_t kDetailLimit = 200;

std::string truncate_detail(const char* raw) {
  if (raw == nullptr) {
    return {};
  }
  std::string detail(raw);
  if (detail.size() > kDetailLimit) {
    detail.resize(kDetailLimit);
  }
  return detail;
}

GpuProviderErrorCode map_init_error(nvmlReturn_t status) {
  switch (status) {
    case NVML_ERROR_DRIVER_NOT_LOADED:
    case NVML_ERROR_LIBRARY_NOT_FOUND:
    case NVML_ERROR_UNINITIALIZED:
      return GpuProviderErrorCode::kBackendUnavailable;
    case NVML_ERROR_NO_PERMISSION:
      return GpuProviderErrorCode::kPermissionDenied;
    default:
      return GpuProviderErrorCode::kObservationFailed;
  }
}

GpuProviderErrorCode map_observe_error(nvmlReturn_t status) {
  switch (status) {
    case NVML_ERROR_UNINITIALIZED:
    case NVML_ERROR_DRIVER_NOT_LOADED:
    case NVML_ERROR_LIBRARY_NOT_FOUND:
      return GpuProviderErrorCode::kBackendUnavailable;
    case NVML_ERROR_NO_PERMISSION:
      return GpuProviderErrorCode::kPermissionDenied;
    default:
      return GpuProviderErrorCode::kObservationFailed;
  }
}

// 外部占用探测结果。
enum class OccupancyCode : std::uint8_t {
  kFree,
  kExternalBusy,
  kUnavailable,
  kPermissionDenied,
  kFailed,
};

// 以 count=0 + null 缓冲查询所需进程条数：不读取任何进程条目字段，因此不依赖
// nvmlProcessInfo_t 的真实布局版本。INSUFFICIENT_SIZE 表示至少存在 1 个进程。
OccupancyCode query_occupancy(const NvmlSymbols& symbols, nvmlDevice_t device) {
  unsigned int count = 0;
  nvmlReturn_t status = NVML_ERROR_FUNCTION_NOT_FOUND;
  if (symbols.device_get_compute_processes_v3 != nullptr) {
    status = symbols.device_get_compute_processes_v3(device, &count, nullptr);
  }
  if (status == NVML_ERROR_FUNCTION_NOT_FOUND &&
      symbols.device_get_compute_processes_v2 != nullptr) {
    status = symbols.device_get_compute_processes_v2(device, &count, nullptr);
  }
  switch (status) {
    case NVML_SUCCESS:
      return count > 0 ? OccupancyCode::kExternalBusy : OccupancyCode::kFree;
    case NVML_ERROR_INSUFFICIENT_SIZE:
      return OccupancyCode::kExternalBusy;
    case NVML_ERROR_NOT_SUPPORTED:
    case NVML_ERROR_GPU_IS_LOST:
    case NVML_ERROR_TIMEOUT:
    case NVML_ERROR_INVALID_ARGUMENT:
      // 无法判定占用（虚拟化平台不支持、设备丢失或查询语义不满足）：按策略
      // 报告 UNAVAILABLE，宁可不可调度也不误报 FREE。
      return OccupancyCode::kUnavailable;
    case NVML_ERROR_NO_PERMISSION:
      return OccupancyCode::kPermissionDenied;
    default:
      return OccupancyCode::kFailed;
  }
}

// 从零填缓冲中提取 NUL 结尾且长度合法的 UUID；越界或非法返回 false。
bool extract_uuid(const char (&buffer)[128], GpuUuid& out) {
  if (std::memchr(buffer, '\0', sizeof(buffer)) == nullptr) {
    return false;
  }
  out = GpuUuid{std::string(buffer)};
  return out.valid();
}

std::string describe(const NvmlSymbols& symbols, nvmlReturn_t status) {
  if (symbols.error_string != nullptr) {
    return truncate_detail(symbols.error_string(status));
  }
  return "nvml error " + std::to_string(static_cast<int>(status));
}

}  // namespace

class NvmlGpuProvider::Impl final {
 public:
  explicit Impl(NvmlGpuProviderConfig config_value) : config(std::move(config_value)) {}

  // 单线程初始化（owner 线程）；成功后符号表与库句柄只读。
  NvmlProviderLoadResult load() {
    if (loaded) {
      return {NvmlProviderLoadCode::kAlreadyLoaded, {}};
    }
    if (config.library_path.empty() || config.max_devices == 0 ||
        config.max_devices > GpuObservationSnapshot::kMaxDevices) {
      return {NvmlProviderLoadCode::kInvalidConfig,
              "library_path must be non-empty and max_devices within 1..128"};
    }

    void* library = ::dlopen(config.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
      return {NvmlProviderLoadCode::kLibraryOpenFailed, truncate_detail(::dlerror())};
    }

    NvmlSymbols resolved{};
    const auto bind = [library](void** target, const char* name) -> bool {
      *target = ::dlsym(library, name);
      return *target != nullptr;
    };
    if (!bind(reinterpret_cast<void**>(&resolved.init), "nvmlInit_v2") ||
        !bind(reinterpret_cast<void**>(&resolved.shutdown), "nvmlShutdown") ||
        !bind(reinterpret_cast<void**>(&resolved.device_get_count), "nvmlDeviceGetCount_v2") ||
        !bind(reinterpret_cast<void**>(&resolved.device_get_handle),
              "nvmlDeviceGetHandleByIndex_v2") ||
        !bind(reinterpret_cast<void**>(&resolved.device_get_uuid), "nvmlDeviceGetUUID") ||
        !bind(reinterpret_cast<void**>(&resolved.device_get_utilization),
              "nvmlDeviceGetUtilizationRates") ||
        !bind(reinterpret_cast<void**>(&resolved.device_get_memory), "nvmlDeviceGetMemoryInfo") ||
        !bind(reinterpret_cast<void**>(&resolved.device_get_compute_processes_v2),
              "nvmlDeviceGetComputeRunningProcesses_v2") ||
        !bind(reinterpret_cast<void**>(&resolved.error_string), "nvmlErrorString")) {
      const std::string detail = truncate_detail(::dlerror());
      static_cast<void>(::dlclose(library));
      return {NvmlProviderLoadCode::kSymbolMissing, detail};
    }
    // v3 计算进程查询是可选符号：旧驱动回退 v2。
    resolved.device_get_compute_processes_v3 =
        reinterpret_cast<decltype(resolved.device_get_compute_processes_v3)>(
            ::dlsym(library, "nvmlDeviceGetComputeRunningProcesses_v3"));

    const nvmlReturn_t status = resolved.init();
    if (status != NVML_SUCCESS) {
      const std::string detail = describe(resolved, status);
      static_cast<void>(::dlclose(library));
      const auto mapped = map_init_error(status);
      if (mapped == GpuProviderErrorCode::kBackendUnavailable) {
        return {NvmlProviderLoadCode::kBackendUnavailable, detail};
      }
      if (mapped == GpuProviderErrorCode::kPermissionDenied) {
        return {NvmlProviderLoadCode::kPermissionDenied, detail};
      }
      return {NvmlProviderLoadCode::kInitFailed, detail};
    }

    symbols = resolved;
    library_handle = library;
    loaded = true;
    return {NvmlProviderLoadCode::kLoaded, {}};
  }

  GpuProviderResult observe() {
    GpuProviderResult result;
    if (!loaded) {
      result.code = GpuProviderErrorCode::kBackendUnavailable;
      return result;
    }

    unsigned int device_count = 0;
    nvmlReturn_t status = symbols.device_get_count(&device_count);
    if (status != NVML_SUCCESS) {
      result.code = map_observe_error(status);
      return result;
    }
    if (static_cast<std::size_t>(device_count) > config.max_devices) {
      // 设备群超出快照上限：整体失败而非截断，防止静默缩编导致 lease 悬空。
      result.code = GpuProviderErrorCode::kObservationFailed;
      return result;
    }

    // 本地构建，仅成功时移入结果：失败路径不携带部分设备数据。
    gpu::GpuObservationSnapshot snapshot;
    snapshot.devices.reserve(device_count);
    for (unsigned int index = 0; index < device_count; ++index) {
      nvmlDevice_t device = nullptr;
      status = symbols.device_get_handle(index, &device);
      if (status != NVML_SUCCESS || device == nullptr) {
        result.code = map_observe_error(status == NVML_SUCCESS ? NVML_ERROR_UNKNOWN : status);
        return result;
      }

      char uuid_buffer[128] = {};
      status = symbols.device_get_uuid(device, uuid_buffer,
                                       static_cast<unsigned int>(sizeof(uuid_buffer)));
      GpuObservation observation;
      // UUID 不可读：设备群身份不完整，整次观测失败（不产出部分设备快照）。
      if (status != NVML_SUCCESS || !extract_uuid(uuid_buffer, observation.uuid)) {
        result.code = status != NVML_SUCCESS ? map_observe_error(status)
                                             : GpuProviderErrorCode::kObservationFailed;
        return result;
      }
      observation.index = index;

      nvmlUtilization_t utilization = {};
      status = symbols.device_get_utilization(device, &utilization);
      // 遥测失败或越界只省略字段；驱动契约 0..100，越界视为不可信。
      if (status == NVML_SUCCESS && utilization.gpu <= 100) {
        observation.telemetry.utilization_percent = utilization.gpu;
      }

      nvmlMemory_t memory = {};
      status = symbols.device_get_memory(device, &memory);
      if (status == NVML_SUCCESS) {
        // used 必须 <= total 才能通过快照校验；不一致时省略而非上报非法值。
        if (memory.used <= memory.total) {
          observation.telemetry.memory_total_bytes = memory.total;
          observation.telemetry.memory_used_bytes = memory.used;
        }
      }

      switch (query_occupancy(symbols, device)) {
        case OccupancyCode::kFree:
          observation.state = GpuObservedState::kFree;
          break;
        case OccupancyCode::kExternalBusy:
          observation.state = GpuObservedState::kExternalBusy;
          break;
        case OccupancyCode::kUnavailable:
          observation.state = GpuObservedState::kUnavailable;
          break;
        case OccupancyCode::kPermissionDenied:
          result.code = GpuProviderErrorCode::kPermissionDenied;
          return result;
        case OccupancyCode::kFailed:
          result.code = GpuProviderErrorCode::kObservationFailed;
          return result;
      }
      snapshot.devices.push_back(std::move(observation));
    }

    snapshot.revision = next_revision.fetch_add(1, std::memory_order_relaxed) + 1;
    snapshot.observed_at = std::chrono::system_clock::now();
    result.snapshot = std::move(snapshot);
    result.code = GpuProviderErrorCode::kNone;
    return result;
  }

  void teardown() {
    if (!loaded) {
      return;
    }
    static_cast<void>(symbols.shutdown());
    static_cast<void>(::dlclose(library_handle));
    library_handle = nullptr;
    loaded = false;
  }

  NvmlGpuProviderConfig config;
  NvmlSymbols symbols{};
  void* library_handle{nullptr};
  bool loaded{false};
  std::atomic<std::uint64_t> next_revision{0};
};

NvmlGpuProvider::NvmlGpuProvider(NvmlGpuProviderConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

NvmlGpuProvider::~NvmlGpuProvider() {
  // owner 纪律保证此刻没有并发 observe()（头文件注释）。
  impl_->teardown();
}

NvmlProviderLoadResult NvmlGpuProvider::load() { return impl_->load(); }

bool NvmlGpuProvider::loaded() const noexcept { return impl_->loaded; }

GpuProviderResult NvmlGpuProvider::observe() { return impl_->observe(); }

const char* to_string(NvmlProviderLoadCode code) noexcept {
  switch (code) {
    case NvmlProviderLoadCode::kLoaded:
      return "loaded";
    case NvmlProviderLoadCode::kAlreadyLoaded:
      return "already loaded";
    case NvmlProviderLoadCode::kInvalidConfig:
      return "invalid config";
    case NvmlProviderLoadCode::kLibraryOpenFailed:
      return "library open failed";
    case NvmlProviderLoadCode::kSymbolMissing:
      return "symbol missing";
    case NvmlProviderLoadCode::kBackendUnavailable:
      return "backend unavailable";
    case NvmlProviderLoadCode::kPermissionDenied:
      return "permission denied";
    case NvmlProviderLoadCode::kInitFailed:
      return "init failed";
  }
  return "unknown";
}

}  // namespace yori::gpu
