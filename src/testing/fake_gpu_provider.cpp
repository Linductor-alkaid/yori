#include "testing/fake_gpu_provider.hpp"

#include <utility>

namespace yori::testing {

FakeGpuProviderUpdateResult FakeGpuProvider::replace_observations(
    std::vector<gpu::GpuObservation> observations,
    std::chrono::system_clock::time_point observed_at) {
  if (observations.size() > capacity_) {
    return {FakeGpuProviderUpdateErrorCode::kCapacityExceeded, {}};
  }

  gpu::GpuObservationSnapshot candidate;
  candidate.revision = snapshot_.revision + 1;
  candidate.observed_at = observed_at;
  candidate.devices = std::move(observations);
  const auto validation = gpu::validate(candidate);
  if (!validation) {
    return {FakeGpuProviderUpdateErrorCode::kInvalidSnapshot, validation};
  }

  snapshot_ = std::move(candidate);
  return {};
}

void FakeGpuProvider::fail_with(gpu::GpuProviderErrorCode error) noexcept {
  if (error == gpu::GpuProviderErrorCode::kNone) {
    failure_.reset();
    return;
  }
  failure_ = error;
}

void FakeGpuProvider::clear_failure() noexcept { failure_.reset(); }

gpu::GpuProviderResult FakeGpuProvider::observe() {
  if (failure_) {
    return {*failure_, {}};
  }
  if (!gpu::validate(snapshot_)) {
    return {gpu::GpuProviderErrorCode::kObservationFailed, {}};
  }
  return {gpu::GpuProviderErrorCode::kNone, snapshot_};
}

}  // namespace yori::testing
