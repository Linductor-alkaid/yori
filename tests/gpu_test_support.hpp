#pragma once

// GPU 采样路径的跨线程测试支撑（M3-04）。GpuManager 的采样 tick 运行在
// Executor 定时器线程，测试在主线程注入状态：可变状态全部为 atomic（跨线程
// happens-before 由 acquire/release 建立），UUID 等不可变字段只在构造/启动前
// 写入（经任务提交建立顺序）。yori::testing::FakeGpuProvider 的普通成员只用于
// 单线程路径，不直接跨线程使用。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>

namespace yori::testing {

class AtomicGpuProvider final : public gpu::GpuProvider {
 public:
  static constexpr std::size_t kSlots = 4;

  // 仅在 GpuManager 启动前调用（不可变身份）。
  void set_uuid(std::size_t slot, std::string uuid) {
    if (slot < kSlots) {
      uuids_[slot] = std::move(uuid);
    }
  }

  // 运行期可变（atomic）。
  void set_present(std::size_t slot, bool present) {
    if (slot < kSlots) {
      present_[slot].store(present, std::memory_order_release);
    }
  }
  void set_state(std::size_t slot, gpu::GpuObservedState state) {
    if (slot < kSlots) {
      state_[slot].store(static_cast<int>(state), std::memory_order_release);
    }
  }
  // percent < 0 省略遥测字段；> 100 产出非法快照（供校验失败路径）。
  void set_utilization(std::size_t slot, int percent) {
    if (slot < kSlots) {
      utilization_[slot].store(percent, std::memory_order_release);
    }
  }
  void set_memory(std::size_t slot, std::uint64_t total, std::uint64_t used) {
    if (slot < kSlots) {
      memory_total_[slot].store(total, std::memory_order_release);
      memory_used_[slot].store(used, std::memory_order_release);
      memory_valid_[slot].store(true, std::memory_order_release);
    }
  }
  void set_failure(std::optional<gpu::GpuProviderErrorCode> error) {
    failure_.store(error.has_value() ? static_cast<int>(*error) : kNoFailure,
                   std::memory_order_release);
  }
  void set_throwing(bool enabled) { throwing_.store(enabled, std::memory_order_release); }

  [[nodiscard]] gpu::GpuProviderResult observe() override {
    gpu::GpuProviderResult result;
    const auto failure = failure_.load(std::memory_order_acquire);
    if (failure != kNoFailure) {
      result.code = static_cast<gpu::GpuProviderErrorCode>(failure);
      return result;
    }
    if (throwing_.load(std::memory_order_acquire)) {
      throw std::runtime_error("injected provider failure");
    }
    result.snapshot.revision = revision_.fetch_add(1, std::memory_order_relaxed) + 1;
    result.snapshot.observed_at = std::chrono::system_clock::now();
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
      if (!present_[slot].load(std::memory_order_acquire)) {
        continue;
      }
      gpu::GpuObservation observation;
      observation.uuid = gpu::GpuUuid{uuids_[slot]};
      observation.index = static_cast<std::uint32_t>(slot);
      observation.state =
          static_cast<gpu::GpuObservedState>(state_[slot].load(std::memory_order_acquire));
      const auto utilization = utilization_[slot].load(std::memory_order_acquire);
      if (utilization >= 0) {
        observation.telemetry.utilization_percent = static_cast<std::uint32_t>(utilization);
      }
      observation.telemetry.memory_total_bytes =
          memory_valid_[slot].load(std::memory_order_acquire)
              ? std::optional<std::uint64_t>{memory_total_[slot].load(std::memory_order_acquire)}
              : std::nullopt;
      observation.telemetry.memory_used_bytes =
          memory_valid_[slot].load(std::memory_order_acquire)
              ? std::optional<std::uint64_t>{memory_used_[slot].load(std::memory_order_acquire)}
              : std::nullopt;
      result.snapshot.devices.push_back(std::move(observation));
    }
    result.code = gpu::GpuProviderErrorCode::kNone;
    return result;
  }

 private:
  static constexpr int kNoFailure = -1;

  std::string uuids_[kSlots];
  std::atomic<bool> present_[kSlots]{};
  std::atomic<int> state_[kSlots]{};
  std::atomic<int> utilization_[kSlots]{-1, -1, -1, -1};
  std::atomic<bool> memory_valid_[kSlots]{};
  std::atomic<std::uint64_t> memory_total_[kSlots]{};
  std::atomic<std::uint64_t> memory_used_[kSlots]{};
  std::atomic<int> failure_{kNoFailure};
  std::atomic<bool> throwing_{false};
  std::atomic<std::uint64_t> revision_{0};
};

// 阻塞注入：observe() 在 hold 置位期间自旋等待（有界），用于构造在途 tick。
class HoldingGpuProvider final : public gpu::GpuProvider {
 public:
  explicit HoldingGpuProvider(gpu::GpuProvider& inner) : inner_(inner) {}

  void hold_next_observe() { hold_.store(true, std::memory_order_release); }
  void release() { hold_.store(false, std::memory_order_release); }

  [[nodiscard]] gpu::GpuProviderResult observe() override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (hold_.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() > deadline) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return inner_.observe();
  }

 private:
  gpu::GpuProvider& inner_;
  std::atomic<bool> hold_{false};
};

// 有界轮询等待谓词成立（测试侧便利，不进入产品路径）。
template <typename Predicate>
bool wait_for(Predicate&& predicate, std::chrono::milliseconds timeout,
              std::chrono::milliseconds step = std::chrono::milliseconds{5}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(step);
  }
  return true;
}

}  // namespace yori::testing
