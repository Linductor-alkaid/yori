#include "runtime/executor_runtime.hpp"

#include <executor/executor.hpp>
#include <stdexcept>

namespace yori::runtime {

class ExecutorRuntime::Impl final {
 public:
  executor::Executor executor;
  bool initialized{false};
};

ExecutorRuntime::ExecutorRuntime() : impl_(std::make_unique<Impl>()) {}

ExecutorRuntime::~ExecutorRuntime() { static_cast<void>(shutdown()); }

bool ExecutorRuntime::initialize(const ExecutorRuntimeConfig& config, std::string& error_message) {
  if (impl_->initialized) {
    error_message = "Executor runtime 已初始化";
    return false;
  }

  executor::ExecutorConfig executor_config;
  executor_config.min_threads = config.min_threads;
  executor_config.max_threads = config.max_threads;
  executor_config.queue_capacity = config.queue_capacity;
  executor_config.max_in_flight_tasks = config.max_in_flight_tasks;

  const auto result = impl_->executor.initialize_ex(executor_config);
  if (!result) {
    error_message = result.message;
    return false;
  }

  impl_->initialized = true;
  error_message.clear();
  return true;
}

bool ExecutorRuntime::is_initialized() const noexcept { return impl_->initialized; }

executor::Executor& ExecutorRuntime::executor() {
  if (!impl_->initialized) {
    throw std::logic_error("Executor runtime 尚未初始化");
  }
  return impl_->executor;
}

ExecutorRuntimeShutdownResult ExecutorRuntime::shutdown() {
  if (!impl_->initialized) {
    return ExecutorRuntimeShutdownResult::kNotInitialized;
  }

  const auto result = impl_->executor.shutdown(true);
  if (result == executor::ShutdownResult::RequestedFromWorker) {
    return ExecutorRuntimeShutdownResult::kRequestedFromWorker;
  }

  impl_->initialized = false;
  return ExecutorRuntimeShutdownResult::kCompleted;
}

}  // namespace yori::runtime
