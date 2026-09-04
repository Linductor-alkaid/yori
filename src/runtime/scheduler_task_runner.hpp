#pragma once

#include <memory>
#include <optional>
#include <string>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/scheduler/scheduler.hpp>

namespace executor {
class Executor;
}

namespace yori::runtime {

enum class SchedulerTaskSubmitCode {
  kAccepted,
  kBusy,
  kNotAccepting,
  kExecutorRejected,
};

struct SchedulerTaskSubmitResult final {
  SchedulerTaskSubmitCode code{SchedulerTaskSubmitCode::kExecutorRejected};
  std::string message;

  [[nodiscard]] bool accepted() const noexcept {
    return code == SchedulerTaskSubmitCode::kAccepted;
  }
};

enum class SchedulerTaskCancelCode {
  kNoTask,
  kRequestedBeforeStart,
  kRequestedRunning,
  kAlreadyRequested,
  kAlreadyCompleted,
  kNotFound,
  kShuttingDown,
};

enum class SchedulerTaskCompletionCode {
  kNoTask,
  kNotReady,
  kCompleted,
  kCancelled,
  kExecutorRejected,
  kFailed,
};

struct SchedulerTaskCompletion final {
  SchedulerTaskCompletionCode code{SchedulerTaskCompletionCode::kNoTask};
  std::optional<scheduler::ScheduleResult> schedule_result;
  std::string message;
};

// 进程私有的 Executor 适配边界。由非 worker 的外部 owner 创建/销毁并单 owner
// 调用；同一时刻最多保留一个调度任务，且 handle/future 直到显式消费前都不会
// 丢失。
class SchedulerTaskRunner final {
 public:
  SchedulerTaskRunner(executor::Executor& executor, scheduler::FifoScheduler& scheduler);
  ~SchedulerTaskRunner();

  SchedulerTaskRunner(const SchedulerTaskRunner&) = delete;
  SchedulerTaskRunner& operator=(const SchedulerTaskRunner&) = delete;
  SchedulerTaskRunner(SchedulerTaskRunner&&) = delete;
  SchedulerTaskRunner& operator=(SchedulerTaskRunner&&) = delete;

  [[nodiscard]] SchedulerTaskSubmitResult trigger(scheduler::SchedulerTrigger trigger,
                                                  gpu::GpuObservationSnapshot snapshot);
  [[nodiscard]] SchedulerTaskCancelCode request_cancel() noexcept;
  [[nodiscard]] SchedulerTaskCompletion try_consume();
  [[nodiscard]] SchedulerTaskCompletion wait_and_consume();

  void stop_accepting() noexcept;
  [[nodiscard]] bool accepting() const noexcept;
  [[nodiscard]] bool has_active_task() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
