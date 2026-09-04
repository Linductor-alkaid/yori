#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace executor {
class Executor;
}

namespace yori::runtime {

struct ExecutorRuntimeConfig {
  std::size_t min_threads{1};
  std::size_t max_threads{1};
  std::size_t queue_capacity{256};
  std::size_t max_in_flight_tasks{256};
};

enum class ExecutorRuntimeShutdownResult {
  kNotInitialized,
  kCompleted,
  kRequestedFromWorker,
};

// 由进程主生命周期持有的唯一 Executor owner。调用方须先停止任务生产者，再从
// 非 worker 线程调用 shutdown()；析构仅作为异常路径的排空兜底。
class ExecutorRuntime final {
 public:
  ExecutorRuntime();
  ~ExecutorRuntime();

  ExecutorRuntime(const ExecutorRuntime&) = delete;
  ExecutorRuntime& operator=(const ExecutorRuntime&) = delete;
  ExecutorRuntime(ExecutorRuntime&&) = delete;
  ExecutorRuntime& operator=(ExecutorRuntime&&) = delete;

  [[nodiscard]] bool initialize(const ExecutorRuntimeConfig& config, std::string& error_message);
  [[nodiscard]] bool is_initialized() const noexcept;
  executor::Executor& executor();
  [[nodiscard]] ExecutorRuntimeShutdownResult shutdown();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
