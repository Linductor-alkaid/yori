#include "testing/in_memory_state_store.hpp"

#include <utility>

#include "store/mutation_core.hpp"

namespace yori::testing {

void InMemoryStateStore::fail_with(store::StateStoreErrorCode error) noexcept {
  if (error == store::StateStoreErrorCode::kNone) {
    failure_.reset();
    return;
  }
  failure_ = error;
}

void InMemoryStateStore::fail_next_apply_with(store::StateStoreErrorCode error) noexcept {
  if (error == store::StateStoreErrorCode::kNone) {
    next_apply_failure_.reset();
    return;
  }
  next_apply_failure_ = error;
}

void InMemoryStateStore::clear_failure() noexcept { failure_.reset(); }

store::StateStoreLoadResult InMemoryStateStore::load() {
  if (failure_) {
    return {*failure_, {}};
  }

  store::StateSnapshot snapshot;
  snapshot.revision = revision_;
  snapshot.jobs.reserve(jobs_.size());
  snapshot.leases.reserve(leases_.size());
  for (const auto& [id, record] : jobs_) {
    static_cast<void>(id);
    snapshot.jobs.push_back(record);
  }
  for (const auto& [uuid, lease] : leases_) {
    static_cast<void>(uuid);
    snapshot.leases.push_back(lease);
  }
  return {store::StateStoreErrorCode::kNone, std::move(snapshot)};
}

store::StateStoreWriteResult InMemoryStateStore::apply(const store::StateMutation& mutation) {
  if (failure_) {
    return {*failure_, revision_};
  }
  if (next_apply_failure_) {
    const auto error = *next_apply_failure_;
    next_apply_failure_.reset();
    return {error, revision_};
  }

  // 验证核心与 SQLite adapter 共享（M4-01），保证两个后端同语义；本实现仅
  // 注入故障与提交副本。
  auto outcome = store::mutation_core::apply_mutation(
      jobs_, leases_, revision_, {config_.max_jobs, config_.max_leases}, mutation);
  if (!outcome.ok()) {
    return {outcome.code, revision_};
  }

  jobs_ = std::move(outcome.next_jobs);
  leases_ = std::move(outcome.next_leases);
  revision_ = outcome.next_revision;
  return {store::StateStoreErrorCode::kNone, revision_};
}

}  // namespace yori::testing
