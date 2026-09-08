#pragma once

#include <memory>
#include <optional>
#include <string>
#include <yori/store/state_store.hpp>

namespace executor {
class Executor;
}

namespace yori::runtime {

enum class StoreTaskSubmitCode {
  kAccepted,
  kBusy,
  kNotAccepting,
  kExecutorRejected,
};

struct StoreTaskSubmitResult final {
  StoreTaskSubmitCode code{StoreTaskSubmitCode::kExecutorRejected};
  std::string message;

  [[nodiscard]] bool accepted() const noexcept { return code == StoreTaskSubmitCode::kAccepted; }
};

enum class StoreTaskCancelCode {
  kNoTask,
  kRequestedBeforeStart,
  kRequestedRunning,
  kAlreadyRequested,
  kAlreadyCompleted,
  kNotFound,
  kShuttingDown,
};

enum class StoreTaskCompletionCode {
  kNoTask,
  kNotReady,
  kCompleted,
  kCancelled,
  kExecutorRejected,
  kFailed,
};

struct StoreTaskCompletion final {
  StoreTaskCompletionCode code{StoreTaskCompletionCode::kNoTask};
  std::optional<store::StateStoreWriteResult> write_result;
  std::string message;

  [[nodiscard]] bool consumed() const noexcept {
    return code == StoreTaskCompletionCode::kCompleted ||
           code == StoreTaskCompletionCode::kCancelled ||
           code == StoreTaskCompletionCode::kExecutorRejected ||
           code == StoreTaskCompletionCode::kFailed;
  }
};

// EXEC-08（SQLite 写入路径）的 Executor 载载：单个在飞 mutation 的
// submit_cancellable 串行单元。mutation 的 expected_revision 由生产者在上一
// 次结果消费后组合（逐条 FIFO 语义），因此前一结果未消费时新提交显式
// kBusy；停止生产后 kNotAccepting；运行中取消只在进入有界 apply 前协作检查，
// 不抢占已开始的原子写入。handle 与 future 全程保留并显式消费，admission
// 拒绝、执行失败与取消均映射为显式结果。
class StoreTaskRunner final {
 public:
  StoreTaskRunner(executor::Executor& executor, store::StateStore& store);
  ~StoreTaskRunner();

  StoreTaskRunner(const StoreTaskRunner&) = delete;
  StoreTaskRunner& operator=(const StoreTaskRunner&) = delete;
  StoreTaskRunner(StoreTaskRunner&&) = delete;
  StoreTaskRunner& operator=(StoreTaskRunner&&) = delete;

  [[nodiscard]] StoreTaskSubmitResult submit(store::StateMutation mutation);
  [[nodiscard]] StoreTaskCancelCode request_cancel() noexcept;
  [[nodiscard]] StoreTaskCompletion try_consume();
  [[nodiscard]] StoreTaskCompletion wait_and_consume();

  void stop_accepting() noexcept;
  [[nodiscard]] bool accepting() const noexcept;
  [[nodiscard]] bool has_active_task() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
