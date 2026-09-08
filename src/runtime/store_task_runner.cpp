#include "runtime/store_task_runner.hpp"

#include <chrono>
#include <exception>
#include <executor/executor.hpp>
#include <future>
#include <utility>

namespace yori::runtime {
namespace {

StoreTaskCancelCode map_cancel(executor::TaskCancellationResult result) noexcept {
  switch (result) {
    case executor::TaskCancellationResult::RequestedBeforeStart:
      return StoreTaskCancelCode::kRequestedBeforeStart;
    case executor::TaskCancellationResult::RequestedRunning:
      return StoreTaskCancelCode::kRequestedRunning;
    case executor::TaskCancellationResult::AlreadyRequested:
      return StoreTaskCancelCode::kAlreadyRequested;
    case executor::TaskCancellationResult::AlreadyCompleted:
      return StoreTaskCancelCode::kAlreadyCompleted;
    case executor::TaskCancellationResult::NotFound:
      return StoreTaskCancelCode::kNotFound;
    case executor::TaskCancellationResult::ShuttingDown:
      return StoreTaskCancelCode::kShuttingDown;
  }
  return StoreTaskCancelCode::kNotFound;
}

}  // namespace

class StoreTaskRunner::Impl final {
 public:
  Impl(executor::Executor& executor_ref, store::StateStore& store_ref)
      : executor(executor_ref), store(store_ref) {}

  StoreTaskCompletion consume_ready() {
    try {
      auto write_result = future.get();
      handle = {};
      return {StoreTaskCompletionCode::kCompleted, write_result, {}};
    } catch (const executor::TaskCancelled& error) {
      handle = {};
      return {StoreTaskCompletionCode::kCancelled, std::nullopt, error.what()};
    } catch (const executor::CapacityExhaustedException& error) {
      handle = {};
      return {StoreTaskCompletionCode::kExecutorRejected, std::nullopt, error.what()};
    } catch (const executor::ExecutorStopping& error) {
      handle = {};
      return {StoreTaskCompletionCode::kExecutorRejected, std::nullopt, error.what()};
    } catch (const std::exception& error) {
      handle = {};
      return {StoreTaskCompletionCode::kFailed, std::nullopt, error.what()};
    } catch (...) {
      handle = {};
      return {StoreTaskCompletionCode::kFailed, std::nullopt,
              "store task failed with a non-standard exception"};
    }
  }

  executor::Executor& executor;
  store::StateStore& store;
  executor::TaskHandle handle;
  std::future<store::StateStoreWriteResult> future;
  bool accepting{true};
};

StoreTaskRunner::StoreTaskRunner(executor::Executor& executor, store::StateStore& store)
    : impl_(std::make_unique<Impl>(executor, store)) {}

StoreTaskRunner::~StoreTaskRunner() {
  stop_accepting();
  if (has_active_task()) {
    static_cast<void>(request_cancel());
    static_cast<void>(wait_and_consume());
  }
}

StoreTaskSubmitResult StoreTaskRunner::submit(store::StateMutation mutation) {
  if (!impl_->accepting) {
    return {StoreTaskSubmitCode::kNotAccepting, "store task producer is stopped"};
  }
  if (impl_->future.valid()) {
    return {StoreTaskSubmitCode::kBusy, "previous store task is not consumed"};
  }

  try {
    auto submission =
        impl_->executor.submit_cancellable([store = &impl_->store, mutation = std::move(mutation)](
                                               const executor::StopToken& token) mutable {
          if (token.stop_requested()) {
            // 排队期取消：未触碰存储。以规范的取消终态异常就绪 future
            // （task_cancellation.hpp：取消不属于 failure 体系）。
            throw executor::TaskCancelled(executor::TaskCancellationReason::Explicit,
                                          "store mutation cancelled before apply");
          }
          return store->apply(mutation);
        });
    impl_->handle = std::move(submission.handle);
    impl_->future = std::move(submission.future);
    return {StoreTaskSubmitCode::kAccepted, {}};
  } catch (const std::exception& error) {
    return {StoreTaskSubmitCode::kExecutorRejected, error.what()};
  } catch (...) {
    return {StoreTaskSubmitCode::kExecutorRejected,
            "executor rejected store task with a non-standard exception"};
  }
}

StoreTaskCancelCode StoreTaskRunner::request_cancel() noexcept {
  if (!impl_->future.valid()) {
    return StoreTaskCancelCode::kNoTask;
  }
  return map_cancel(impl_->executor.request_task_cancel(impl_->handle).result);
}

StoreTaskCompletion StoreTaskRunner::try_consume() {
  if (!impl_->future.valid()) {
    return {};
  }
  if (impl_->future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
    return {StoreTaskCompletionCode::kNotReady, std::nullopt, {}};
  }
  return impl_->consume_ready();
}

StoreTaskCompletion StoreTaskRunner::wait_and_consume() {
  if (!impl_->future.valid()) {
    return {};
  }
  impl_->future.wait();
  return impl_->consume_ready();
}

void StoreTaskRunner::stop_accepting() noexcept { impl_->accepting = false; }

bool StoreTaskRunner::accepting() const noexcept { return impl_->accepting; }

bool StoreTaskRunner::has_active_task() const noexcept { return impl_->future.valid(); }

}  // namespace yori::runtime
