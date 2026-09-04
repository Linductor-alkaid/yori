#include "runtime/scheduler_task_runner.hpp"

#include <chrono>
#include <exception>
#include <executor/executor.hpp>
#include <future>
#include <utility>

namespace yori::runtime {
namespace {

SchedulerTaskCancelCode map_cancel(executor::TaskCancellationResult result) noexcept {
  switch (result) {
    case executor::TaskCancellationResult::RequestedBeforeStart:
      return SchedulerTaskCancelCode::kRequestedBeforeStart;
    case executor::TaskCancellationResult::RequestedRunning:
      return SchedulerTaskCancelCode::kRequestedRunning;
    case executor::TaskCancellationResult::AlreadyRequested:
      return SchedulerTaskCancelCode::kAlreadyRequested;
    case executor::TaskCancellationResult::AlreadyCompleted:
      return SchedulerTaskCancelCode::kAlreadyCompleted;
    case executor::TaskCancellationResult::NotFound:
      return SchedulerTaskCancelCode::kNotFound;
    case executor::TaskCancellationResult::ShuttingDown:
      return SchedulerTaskCancelCode::kShuttingDown;
  }
  return SchedulerTaskCancelCode::kNotFound;
}

}  // namespace

class SchedulerTaskRunner::Impl final {
 public:
  Impl(executor::Executor& executor_ref, scheduler::FifoScheduler& scheduler_ref)
      : executor(executor_ref), scheduler(scheduler_ref) {}

  SchedulerTaskCompletion consume_ready() {
    try {
      auto schedule_result = future.get();
      handle = {};
      return {SchedulerTaskCompletionCode::kCompleted, std::move(schedule_result), {}};
    } catch (const executor::TaskCancelled& error) {
      handle = {};
      return {SchedulerTaskCompletionCode::kCancelled, std::nullopt, error.what()};
    } catch (const executor::CapacityExhaustedException& error) {
      handle = {};
      return {SchedulerTaskCompletionCode::kExecutorRejected, std::nullopt, error.what()};
    } catch (const executor::ExecutorStopping& error) {
      handle = {};
      return {SchedulerTaskCompletionCode::kExecutorRejected, std::nullopt, error.what()};
    } catch (const std::exception& error) {
      handle = {};
      return {SchedulerTaskCompletionCode::kFailed, std::nullopt, error.what()};
    } catch (...) {
      handle = {};
      return {SchedulerTaskCompletionCode::kFailed, std::nullopt,
              "scheduler task failed with a non-standard exception"};
    }
  }

  executor::Executor& executor;
  scheduler::FifoScheduler& scheduler;
  executor::TaskHandle handle;
  std::future<scheduler::ScheduleResult> future;
  bool accepting{true};
};

SchedulerTaskRunner::SchedulerTaskRunner(executor::Executor& executor,
                                         scheduler::FifoScheduler& scheduler)
    : impl_(std::make_unique<Impl>(executor, scheduler)) {}

SchedulerTaskRunner::~SchedulerTaskRunner() {
  stop_accepting();
  if (has_active_task()) {
    static_cast<void>(request_cancel());
    static_cast<void>(wait_and_consume());
  }
}

SchedulerTaskSubmitResult SchedulerTaskRunner::trigger(scheduler::SchedulerTrigger trigger,
                                                       gpu::GpuObservationSnapshot snapshot) {
  if (!impl_->accepting) {
    return {SchedulerTaskSubmitCode::kNotAccepting, "scheduler task producer is stopped"};
  }
  if (impl_->future.valid()) {
    return {SchedulerTaskSubmitCode::kBusy, "previous scheduler task is not consumed"};
  }

  try {
    auto submission = impl_->executor.submit_cancellable(
        [scheduler = &impl_->scheduler, trigger,
         snapshot = std::move(snapshot)](const executor::StopToken& token) mutable {
          if (token.stop_requested()) {
            return scheduler::FifoScheduler::cancelled(trigger);
          }
          return scheduler->run_once(trigger, snapshot);
        });
    impl_->handle = std::move(submission.handle);
    impl_->future = std::move(submission.future);
    return {SchedulerTaskSubmitCode::kAccepted, {}};
  } catch (const std::exception& error) {
    return {SchedulerTaskSubmitCode::kExecutorRejected, error.what()};
  } catch (...) {
    return {SchedulerTaskSubmitCode::kExecutorRejected,
            "executor rejected scheduler task with a non-standard exception"};
  }
}

SchedulerTaskCancelCode SchedulerTaskRunner::request_cancel() noexcept {
  if (!impl_->future.valid()) {
    return SchedulerTaskCancelCode::kNoTask;
  }
  return map_cancel(impl_->executor.request_task_cancel(impl_->handle).result);
}

SchedulerTaskCompletion SchedulerTaskRunner::try_consume() {
  if (!impl_->future.valid()) {
    return {};
  }
  if (impl_->future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
    return {SchedulerTaskCompletionCode::kNotReady, std::nullopt, {}};
  }
  return impl_->consume_ready();
}

SchedulerTaskCompletion SchedulerTaskRunner::wait_and_consume() {
  if (!impl_->future.valid()) {
    return {};
  }
  impl_->future.wait();
  return impl_->consume_ready();
}

void SchedulerTaskRunner::stop_accepting() noexcept { impl_->accepting = false; }

bool SchedulerTaskRunner::accepting() const noexcept { return impl_->accepting; }

bool SchedulerTaskRunner::has_active_task() const noexcept { return impl_->future.valid(); }

}  // namespace yori::runtime
