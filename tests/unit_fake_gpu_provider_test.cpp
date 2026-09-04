#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "testing/fake_gpu_provider.hpp"
#include "yori_test.hpp"

namespace {

yori::gpu::GpuObservation gpu(std::string uuid, std::uint32_t index,
                              yori::gpu::GpuObservedState state) {
  yori::gpu::GpuObservation observation;
  observation.uuid = yori::gpu::GpuUuid{std::move(uuid)};
  observation.index = index;
  observation.state = state;
  observation.telemetry.utilization_percent = 50;
  observation.telemetry.memory_total_bytes = 16ULL * 1024 * 1024 * 1024;
  observation.telemetry.memory_used_bytes = 4ULL * 1024 * 1024 * 1024;
  return observation;
}

constexpr auto kObservedAt = std::chrono::system_clock::time_point{std::chrono::seconds{1}};

}  // namespace

int main() {
  using yori::gpu::GpuLogicalState;
  using yori::gpu::GpuObservationErrorCode;
  using yori::gpu::GpuObservedState;
  using yori::gpu::GpuProviderErrorCode;
  using yori::testing::FakeGpuProviderUpdateErrorCode;

  yori::testing::FakeGpuProvider provider{2};
  YORI_CHECK(provider.observe().code == GpuProviderErrorCode::kObservationFailed);

  const auto updated =
      provider.replace_observations({gpu("GPU-aaaa", 0, GpuObservedState::kFree),
                                     gpu("GPU-bbbb", 1, GpuObservedState::kExternalBusy)},
                                    kObservedAt);
  YORI_CHECK(updated);

  const auto observed = provider.observe();
  YORI_CHECK(observed);
  YORI_CHECK(observed.snapshot.revision == 1);
  YORI_CHECK(observed.snapshot.devices.size() == 2);
  YORI_CHECK(observed.snapshot.devices[0].uuid.value() == "GPU-aaaa");

  YORI_CHECK(yori::gpu::derive_logical_state(GpuObservedState::kFree, false) ==
             GpuLogicalState::kFree);
  YORI_CHECK(yori::gpu::derive_logical_state(GpuObservedState::kFree, true) ==
             GpuLogicalState::kAllocated);
  YORI_CHECK(yori::gpu::derive_logical_state(GpuObservedState::kExternalBusy, false) ==
             GpuLogicalState::kExternalBusy);
  YORI_CHECK(yori::gpu::derive_logical_state(GpuObservedState::kUnavailable, false) ==
             GpuLogicalState::kUnavailable);
  YORI_CHECK(yori::gpu::derive_logical_state(GpuObservedState::kUnavailable, true) ==
             GpuLogicalState::kAllocated);

  auto duplicate_uuid = provider.replace_observations(
      {gpu("GPU-same", 0, GpuObservedState::kFree), gpu("GPU-same", 1, GpuObservedState::kFree)},
      kObservedAt);
  YORI_CHECK(!duplicate_uuid);
  YORI_CHECK(duplicate_uuid.code == FakeGpuProviderUpdateErrorCode::kInvalidSnapshot);
  YORI_CHECK(duplicate_uuid.validation.code == GpuObservationErrorCode::kDuplicateUuid);
  YORI_CHECK(provider.observe().snapshot.revision == 1);

  auto duplicate_index = provider.replace_observations(
      {gpu("GPU-cccc", 0, GpuObservedState::kFree), gpu("GPU-dddd", 0, GpuObservedState::kFree)},
      kObservedAt);
  YORI_CHECK(!duplicate_index);
  YORI_CHECK(duplicate_index.validation.code == GpuObservationErrorCode::kDuplicateIndex);

  auto invalid_telemetry = gpu("GPU-cccc", 0, GpuObservedState::kFree);
  invalid_telemetry.telemetry.utilization_percent = 101;
  const auto invalid_update = provider.replace_observations({invalid_telemetry}, kObservedAt);
  YORI_CHECK(!invalid_update);
  YORI_CHECK(invalid_update.validation.code == GpuObservationErrorCode::kInvalidUtilization);

  auto invalid_memory = gpu("GPU-cccc", 0, GpuObservedState::kFree);
  invalid_memory.telemetry.memory_used_bytes =
      invalid_memory.telemetry.memory_total_bytes.value_or(0) + 1;
  const auto invalid_memory_update = provider.replace_observations({invalid_memory}, kObservedAt);
  YORI_CHECK(invalid_memory_update.validation.code ==
             GpuObservationErrorCode::kInvalidMemoryTelemetry);

  const auto invalid_uuid =
      provider.replace_observations({gpu("", 0, GpuObservedState::kFree)}, kObservedAt);
  YORI_CHECK(invalid_uuid.validation.code == GpuObservationErrorCode::kInvalidUuid);

  const auto invalid_time =
      provider.replace_observations({gpu("GPU-cccc", 0, GpuObservedState::kFree)}, {});
  YORI_CHECK(invalid_time.validation.code == GpuObservationErrorCode::kInvalidObservationTime);

  yori::testing::FakeGpuProvider capacity_one{1};
  const auto capacity_result = capacity_one.replace_observations(
      {gpu("GPU-aaaa", 0, GpuObservedState::kFree), gpu("GPU-bbbb", 1, GpuObservedState::kFree)},
      kObservedAt);
  YORI_CHECK(capacity_result.code == FakeGpuProviderUpdateErrorCode::kCapacityExceeded);

  provider.fail_with(GpuProviderErrorCode::kPermissionDenied);
  YORI_CHECK(provider.observe().code == GpuProviderErrorCode::kPermissionDenied);
  provider.clear_failure();
  YORI_CHECK(provider.observe());

  constexpr std::array observed_states{GpuObservedState::kFree, GpuObservedState::kExternalBusy,
                                       GpuObservedState::kUnavailable};
  for (const auto state : observed_states) {
    YORI_CHECK(std::string(yori::gpu::to_string(state)) != "UNKNOWN");
  }
  constexpr std::array logical_states{GpuLogicalState::kFree, GpuLogicalState::kAllocated,
                                      GpuLogicalState::kExternalBusy,
                                      GpuLogicalState::kUnavailable};
  for (const auto state : logical_states) {
    YORI_CHECK(std::string(yori::gpu::to_string(state)) != "UNKNOWN");
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
