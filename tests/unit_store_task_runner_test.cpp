// StoreTaskRunner（EXEC-08 载载）单测（M4-04）：六场景映射——正常完成、任务
// 异常（store 抛异常 / apply 结构化失败）、提交拒绝（BUSY / NOT_ACCEPTING /
// Executor 已关闭）、执行中取消（排队期取消进入 Executor 生命周期视图）、
// 超时（stop 后 join 有界：单 in-flight 任务在 apply 完成后就绪）、shutdown
// 先后顺序（停止生产 -> 消费 -> shutdown 不挂起）。
#include <chrono>
#include <cstdio>
#include <executor/executor.hpp>
#include <memory>
#include <stdexcept>
#include <thread>

#include "runtime/executor_runtime.hpp"
#include "runtime/store_task_runner.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

namespace {

using yori::runtime::ExecutorRuntime;
using yori::runtime::ExecutorRuntimeConfig;
using yori::runtime::StoreTaskCompletionCode;
using yori::runtime::StoreTaskRunner;
using yori::runtime::StoreTaskSubmitCode;
using yori::store::StateMutation;
using yori::store::StateStoreErrorCode;

yori::store::StoredJob queued(std::uint64_t id) {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{id}};
  return {yori::job::JobId{id}, spec, yori::job::JobState::kQueued, 0, {}};
}

StateMutation create_mutation(std::uint64_t id, std::uint64_t expected_revision) {
  StateMutation mutation;
  mutation.expected_revision = expected_revision;
  mutation.create_jobs.push_back(queued(id));
  return mutation;
}

// 任务异常注入：apply 抛出异常的 StateStore。
class ThrowingStore final : public yori::store::StateStore {
 public:
  yori::store::StateStoreLoadResult load() override { return inner_.load(); }
  yori::store::StateStoreWriteResult apply(const StateMutation& mutation) override {
    if (throw_enabled_) {
      throw std::runtime_error("injected store failure");
    }
    return inner_.apply(mutation);
  }
  void set_throw(bool enabled) noexcept { throw_enabled_ = enabled; }

 private:
  yori::testing::InMemoryStateStore inner_;
  bool throw_enabled_{false};
};

}  // namespace

int main() {
  using namespace std::chrono_literals;

  // ---- 正常完成 + 串行 FIFO 语义（前一结果未消费 -> BUSY）--------------------
  {
    ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize(ExecutorRuntimeConfig{}, error));
    yori::testing::InMemoryStateStore store;
    StoreTaskRunner runner(runtime.executor(), store);

    auto first = runner.submit(create_mutation(1, 0));
    YORI_CHECK(first.accepted());
    YORI_CHECK(runner.has_active_task());

    auto busy = runner.submit(create_mutation(2, 1));
    YORI_CHECK(busy.code == StoreTaskSubmitCode::kBusy);

    const auto completion = runner.wait_and_consume();
    YORI_CHECK(completion.code == StoreTaskCompletionCode::kCompleted);
    YORI_CHECK(completion.write_result.has_value());
    YORI_CHECK(completion.write_result->ok());
    YORI_CHECK(completion.write_result->revision == 1);
    YORI_CHECK(!runner.has_active_task());

    // 下一 mutation 由生产者基于新 revision 组合（逐条 FIFO 语义）。
    auto second = runner.submit(create_mutation(2, 1));
    YORI_CHECK(second.accepted());
    const auto second_completion = runner.wait_and_consume();
    YORI_CHECK(second_completion.code == StoreTaskCompletionCode::kCompleted);
    YORI_CHECK(second_completion.write_result->revision == 2);

    runner.stop_accepting();
    const auto rejected = runner.submit(create_mutation(3, 2));
    YORI_CHECK(rejected.code == StoreTaskSubmitCode::kNotAccepting);
    YORI_CHECK(!runner.accepting());

    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // ---- 任务异常：apply 抛异常 -> kFailed 显式结果 -----------------------------
  {
    ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize(ExecutorRuntimeConfig{}, error));
    ThrowingStore store;
    StoreTaskRunner runner(runtime.executor(), store);
    store.set_throw(true);

    YORI_CHECK(runner.submit(create_mutation(1, 0)).accepted());
    const auto completion = runner.wait_and_consume();
    YORI_CHECK(completion.code == StoreTaskCompletionCode::kFailed);
    YORI_CHECK(!completion.message.empty());
    YORI_CHECK(!completion.write_result.has_value());
    YORI_CHECK(!runner.has_active_task());

    // 结构化失败（非异常）保持 kCompleted + 失败写结果，不被吞掉。
    store.set_throw(false);
    auto stale = create_mutation(2, 99);
    YORI_CHECK(runner.submit(std::move(stale)).accepted());
    const auto conflict = runner.wait_and_consume();
    YORI_CHECK(conflict.code == StoreTaskCompletionCode::kCompleted);
    YORI_CHECK(conflict.write_result.has_value());
    YORI_CHECK(conflict.write_result->code == StateStoreErrorCode::kRevisionConflict);

    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // ---- 执行中取消（排队期）： Executor 已被长任务占住 -> 排队 -> 取消 -------
  {
    ExecutorRuntimeConfig config;
    config.min_threads = 1;
    config.max_threads = 1;
    ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize(config, error));

    auto blocker = runtime.executor().submit_auto([] {
      std::this_thread::sleep_for(300ms);
      return 0;
    });
    yori::testing::InMemoryStateStore store;
    StoreTaskRunner runner(runtime.executor(), store);

    YORI_CHECK(runner.submit(create_mutation(1, 0)).accepted());
    // 单 worker 被 blocker 占用，store 任务仍在排队：取消命中排队期。
    const auto cancel = runner.request_cancel();
    YORI_CHECK(cancel == yori::runtime::StoreTaskCancelCode::kRequestedBeforeStart ||
               cancel == yori::runtime::StoreTaskCancelCode::kAlreadyRequested ||
               cancel == yori::runtime::StoreTaskCancelCode::kRequestedRunning);

    const auto completion = runner.wait_and_consume();
    // 无论取消落在排队期还是运行前检查，结果都必须显式且 store 未被触碰。
    YORI_CHECK(completion.consumed());
    if (completion.code == StoreTaskCompletionCode::kCompleted) {
      YORI_CHECK(completion.write_result.has_value());
    } else {
      YORI_CHECK(completion.code == StoreTaskCompletionCode::kCancelled);
    }
    YORI_CHECK(store.load().snapshot.jobs.empty());

    static_cast<void>(blocker.get());
    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  // ---- 提交拒绝：Executor 已 shutdown ---------------------------------------
  {
    ExecutorRuntime runtime;
    std::string error;
    YORI_CHECK(runtime.initialize(ExecutorRuntimeConfig{}, error));
    yori::testing::InMemoryStateStore store;
    StoreTaskRunner runner(runtime.executor(), store);
    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);

    const auto rejected = runner.submit(create_mutation(1, 0));
    YORI_CHECK(rejected.code == StoreTaskSubmitCode::kExecutorRejected);
  }

  // ---- shutdown 顺序：停止生产 -> 消费在飞结果 -> shutdown；析构兜底不挂起 ----
  {
    auto runtime = std::make_unique<ExecutorRuntime>();
    std::string error;
    YORI_CHECK(runtime->initialize(ExecutorRuntimeConfig{}, error));
    auto store = std::make_unique<yori::testing::InMemoryStateStore>();
    auto runner = std::make_unique<StoreTaskRunner>(runtime->executor(), *store);

    YORI_CHECK(runner->submit(create_mutation(1, 0)).accepted());
    runner->stop_accepting();
    const auto completion = runner->wait_and_consume();
    YORI_CHECK(completion.code == StoreTaskCompletionCode::kCompleted);
    YORI_CHECK(completion.write_result->ok());

    runner.reset();  // 先于 Executor 回收（EXEC-08 关闭阶段 ⑦ 语义）
    store.reset();
    YORI_CHECK(runtime->shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
    runtime.reset();
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
