#include "runtime/serial_state_store.hpp"

namespace yori::runtime {

SerialStateStore::SerialStateStore(std::unique_ptr<store::StateStore> inner)
    : inner_(std::move(inner)) {}

store::StateStoreLoadResult SerialStateStore::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++serialized_calls_;
  return inner_->load();
}

store::StateStoreWriteResult SerialStateStore::apply(const store::StateMutation& mutation) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++serialized_calls_;
  return inner_->apply(mutation);
}

std::uint64_t SerialStateStore::serialized_calls() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return serialized_calls_;
}

}  // namespace yori::runtime
