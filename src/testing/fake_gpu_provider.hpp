#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>

namespace yori::testing {

enum class FakeGpuProviderUpdateErrorCode {
  kNone,
  kCapacityExceeded,
  kInvalidSnapshot,
};

struct FakeGpuProviderUpdateResult final {
  FakeGpuProviderUpdateErrorCode code{FakeGpuProviderUpdateErrorCode::kNone};
  gpu::GpuObservationValidationResult validation{};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == FakeGpuProviderUpdateErrorCode::kNone;
  }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// 单 owner、进程内可控的测试 Provider。测试在派发观测任务前注入状态；该类不
// 自建锁、线程或队列。
class FakeGpuProvider final : public gpu::GpuProvider {
 public:
  explicit FakeGpuProvider(std::size_t capacity = 16) : capacity_(capacity) {}

  [[nodiscard]] FakeGpuProviderUpdateResult replace_observations(
      std::vector<gpu::GpuObservation> observations,
      std::chrono::system_clock::time_point observed_at);
  void fail_with(gpu::GpuProviderErrorCode error) noexcept;
  void clear_failure() noexcept;

  [[nodiscard]] gpu::GpuProviderResult observe() override;

 private:
  std::size_t capacity_;
  gpu::GpuObservationSnapshot snapshot_;
  std::optional<gpu::GpuProviderErrorCode> failure_;
};

}  // namespace yori::testing
