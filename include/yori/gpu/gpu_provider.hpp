#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <yori/job/job.hpp>

namespace yori::gpu {

class GpuUuid final {
 public:
  static constexpr std::size_t kMaxBytes = 96;

  GpuUuid() = default;
  explicit GpuUuid(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept {
    return !value_.empty() && value_.size() <= kMaxBytes && value_.find('\0') == std::string::npos;
  }

  auto operator<=>(const GpuUuid&) const = default;

 private:
  std::string value_;
};

// Provider 只能报告资源观测，不得把 Yori lease 伪装成设备事实。
enum class GpuObservedState {
  kFree,
  kExternalBusy,
  kUnavailable,
};

// Core 将观测与 lease 分列后得到的调度视图。
enum class GpuLogicalState {
  kFree,
  kAllocated,
  kExternalBusy,
  kUnavailable,
};

struct GpuTelemetry final {
  std::optional<std::uint32_t> utilization_percent;
  std::optional<std::uint64_t> memory_total_bytes;
  std::optional<std::uint64_t> memory_used_bytes;
};

struct GpuObservation final {
  GpuUuid uuid;
  std::uint32_t index{0};
  GpuObservedState state{GpuObservedState::kUnavailable};
  GpuTelemetry telemetry;
};

struct GpuObservationSnapshot final {
  static constexpr std::size_t kMaxDevices = 128;

  std::uint64_t revision{0};
  std::chrono::system_clock::time_point observed_at{};
  std::vector<GpuObservation> devices;
};

enum class GpuObservationErrorCode {
  kNone,
  kInvalidRevision,
  kInvalidObservationTime,
  kTooManyDevices,
  kInvalidUuid,
  kDuplicateUuid,
  kDuplicateIndex,
  kInvalidState,
  kInvalidUtilization,
  kInvalidMemoryTelemetry,
};

struct GpuObservationValidationResult final {
  GpuObservationErrorCode code{GpuObservationErrorCode::kNone};
  std::optional<std::size_t> item_index;

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == GpuObservationErrorCode::kNone;
  }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

[[nodiscard]] GpuObservationValidationResult validate(
    const GpuObservationSnapshot& snapshot) noexcept;
[[nodiscard]] const char* to_string(GpuObservedState state) noexcept;
[[nodiscard]] const char* to_string(GpuLogicalState state) noexcept;
[[nodiscard]] const char* to_string(GpuObservationErrorCode code) noexcept;

struct GpuLease final {
  GpuUuid gpu_uuid;
  job::JobId job_id;
};

// Lease 是 authoritative scheduler 的事实，优先于 FREE 观测；观测字段仍原样
// 保留，调用方可独立诊断 lease 与 EXTERNAL_BUSY/UNAVAILABLE 的冲突。
[[nodiscard]] constexpr GpuLogicalState derive_logical_state(GpuObservedState observed_state,
                                                             bool has_lease) noexcept {
  if (has_lease) {
    return GpuLogicalState::kAllocated;
  }
  switch (observed_state) {
    case GpuObservedState::kFree:
      return GpuLogicalState::kFree;
    case GpuObservedState::kExternalBusy:
      return GpuLogicalState::kExternalBusy;
    case GpuObservedState::kUnavailable:
      return GpuLogicalState::kUnavailable;
  }
  return GpuLogicalState::kUnavailable;
}

enum class GpuProviderErrorCode {
  kNone,
  kBackendUnavailable,
  kPermissionDenied,
  kObservationFailed,
};

struct GpuProviderResult final {
  GpuProviderErrorCode code{GpuProviderErrorCode::kNone};
  GpuObservationSnapshot snapshot;

  [[nodiscard]] constexpr bool ok() const noexcept { return code == GpuProviderErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

[[nodiscard]] const char* to_string(GpuProviderErrorCode code) noexcept;

class GpuProvider {
 public:
  virtual ~GpuProvider() = default;

  // 同步、有界的一次观测。周期与异步生命周期由调用方通过 Executor 承载，
  // Provider 不得隐藏 worker、timer 或全局运行时。
  [[nodiscard]] virtual GpuProviderResult observe() = 0;
};

}  // namespace yori::gpu
