#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <yori/store/state_store.hpp>

namespace yori::testing {

struct InMemoryStateStoreConfig final {
  std::size_t max_jobs{1024};
  std::size_t max_leases{128};
};

// 单 owner 的测试 StateStore。生产适配器由 EXEC-08 的 Executor 串行上下文承载；
// 本实现不隐藏线程、锁、队列或重试。
class InMemoryStateStore final : public store::StateStore {
 public:
  explicit InMemoryStateStore(InMemoryStateStoreConfig config = {}) : config_(config) {}

  void fail_with(store::StateStoreErrorCode error) noexcept;
  void fail_next_apply_with(store::StateStoreErrorCode error) noexcept;
  void clear_failure() noexcept;

  [[nodiscard]] store::StateStoreLoadResult load() override;
  [[nodiscard]] store::StateStoreWriteResult apply(const store::StateMutation& mutation) override;

 private:
  InMemoryStateStoreConfig config_;
  std::uint64_t revision_{0};
  std::map<job::JobId, store::StoredJob> jobs_;
  std::map<gpu::GpuUuid, gpu::GpuLease> leases_;
  std::optional<store::StateStoreErrorCode> failure_;
  std::optional<store::StateStoreErrorCode> next_apply_failure_;
};

}  // namespace yori::testing
