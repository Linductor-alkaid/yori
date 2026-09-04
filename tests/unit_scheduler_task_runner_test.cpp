#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <executor/comm/phase_gate.hpp>
#include <executor/executor.hpp>
#include <memory>
#include <string>
#include <utility>

#include "runtime/executor_runtime.hpp"
#include "runtime/scheduler_task_runner.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

namespace {

yori::store::StoredJob queued(std::uint64_t id) {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{id}};
  return {yori::job::JobId{id}, std::move(spec), yori::job::JobState::kQueued, 0};
}

yori::gpu::GpuObservationSnapshot gpu_snapshot() {
  yori::gpu::GpuObservationSnapshot snapshot;
  snapshot.revision = 1;
  snapshot.observed_at = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
  snapshot.devices.push_back(
      {yori::gpu::GpuUuid{"GPU-runtime"}, 0, yori::gpu::GpuObservedState::kFree, {}});
  return snapshot;
}

std::unique_ptr<yori::queue::GlobalJobQueue> make_queue(std::size_t capacity) {
  yori::queue::QueueErrorCode error{};
  auto queue = yori::queue::GlobalJobQueue::create({capacity}, error);
  if (!queue || error != yori::queue::QueueErrorCode::kNone) {
    std::fprintf(stderr, "failed to create queue\n");
    std::exit(1);
  }
  return queue;
}

void seed(yori::testing::InMemoryStateStore& store, yori::queue::GlobalJobQueue& queue,
          std::uint64_t id) {
  yori::store::StateMutation create;
  create.create_jobs.push_back(queued(id));
  if (!store.apply(create)) {
    std::fprintf(stderr, "failed to seed store\n");
    std::exit(1);
  }
  const auto state = store.load();
  if (!state || !queue.restore(state.snapshot)) {
    std::fprintf(stderr, "failed to restore queue\n");
    std::exit(1);
  }
}

yori::runtime::SchedulerTaskCompletion consume(yori::runtime::SchedulerTaskRunner& runner) {
  auto completion = runner.try_consume();
  if (completion.code == yori::runtime::SchedulerTaskCompletionCode::kNotReady) {
    completion = runner.wait_and_consume();
  }
  return completion;
}

const yori::scheduler::ScheduleResult& require_schedule_result(
    const yori::runtime::SchedulerTaskCompletion& completion) {
  if (!completion.schedule_result) {
    std::fprintf(stderr, "required scheduler result is missing\n");
    std::exit(1);
  }
  return *completion.schedule_result;
}

}  // namespace

int main() {
  using yori::runtime::ExecutorRuntimeShutdownResult;
  using yori::runtime::SchedulerTaskCancelCode;
  using yori::runtime::SchedulerTaskCompletionCode;
  using yori::runtime::SchedulerTaskSubmitCode;
  using yori::scheduler::SchedulerTrigger;

  yori::runtime::ExecutorRuntime runtime;
  std::string error_message;
  YORI_CHECK(runtime.initialize({1, 1, 8, 4}, error_message));

  auto queue = make_queue(1);
  yori::testing::InMemoryStateStore store{{1, 1}};
  seed(store, *queue, 1);
  yori::scheduler::FifoScheduler scheduler{*queue, store};
  yori::runtime::SchedulerTaskRunner runner{runtime.executor(), scheduler};

  auto submitted = runner.trigger(SchedulerTrigger::kJobSubmitted, gpu_snapshot());
  YORI_CHECK(submitted.accepted());
  YORI_CHECK(runner.has_active_task());
  const auto busy = runner.trigger(SchedulerTrigger::kGpuStateChanged, gpu_snapshot());
  YORI_CHECK(busy.code == SchedulerTaskSubmitCode::kBusy);

  auto completion = consume(runner);
  YORI_CHECK(completion.code == SchedulerTaskCompletionCode::kCompleted);
  YORI_CHECK(completion.schedule_result);
  YORI_CHECK(require_schedule_result(completion).scheduled());
  YORI_CHECK(!runner.has_active_task());
  YORI_CHECK(runner.request_cancel() == SchedulerTaskCancelCode::kNoTask);
  YORI_CHECK(runner.try_consume().code == SchedulerTaskCompletionCode::kNoTask);

  runner.stop_accepting();
  YORI_CHECK(!runner.accepting());
  submitted = runner.trigger(SchedulerTrigger::kJobExited, gpu_snapshot());
  YORI_CHECK(submitted.code == SchedulerTaskSubmitCode::kNotAccepting);

  auto cancel_queue = make_queue(1);
  yori::testing::InMemoryStateStore cancel_store{{1, 1}};
  seed(cancel_store, *cancel_queue, 2);
  yori::scheduler::FifoScheduler cancel_scheduler{*cancel_queue, cancel_store};
  yori::runtime::SchedulerTaskRunner cancel_runner{runtime.executor(), cancel_scheduler};

  executor::comm::PhaseGate gate{"queued-cancel"};
  auto blocker = runtime.executor().submit_auto([&gate] {
    if (!gate.advance_to(1)) {
      return false;
    }
    return static_cast<bool>(gate.wait_for(2, std::chrono::seconds{5}));
  });
  YORI_CHECK(gate.wait_for(1, std::chrono::seconds{5}));

  submitted = cancel_runner.trigger(SchedulerTrigger::kJobSubmitted, gpu_snapshot());
  YORI_CHECK(submitted.accepted());
  YORI_CHECK(cancel_runner.request_cancel() == SchedulerTaskCancelCode::kRequestedBeforeStart);
  YORI_CHECK(gate.advance_to(2));
  YORI_CHECK(blocker.get());
  completion = cancel_runner.wait_and_consume();
  YORI_CHECK(completion.code == SchedulerTaskCompletionCode::kCancelled);
  YORI_CHECK(!completion.schedule_result);
  YORI_CHECK(cancel_queue->size() == 1);
  YORI_CHECK(cancel_store.load().snapshot.leases.empty());
  YORI_CHECK(runtime.executor().get_cancellation_status().queued_cancelled_count >= 1);
  cancel_runner.stop_accepting();

  YORI_CHECK(runtime.shutdown() == ExecutorRuntimeShutdownResult::kCompleted);

  yori::runtime::ExecutorRuntime capacity_runtime;
  YORI_CHECK(capacity_runtime.initialize({1, 1, 8, 1}, error_message));
  auto capacity_queue = make_queue(1);
  yori::testing::InMemoryStateStore capacity_store{{1, 1}};
  seed(capacity_store, *capacity_queue, 3);
  yori::scheduler::FifoScheduler capacity_scheduler{*capacity_queue, capacity_store};
  yori::runtime::SchedulerTaskRunner capacity_runner{capacity_runtime.executor(),
                                                     capacity_scheduler};

  executor::comm::PhaseGate capacity_gate{"capacity-rejection"};
  auto capacity_blocker = capacity_runtime.executor().submit_auto([&capacity_gate] {
    if (!capacity_gate.advance_to(1)) {
      return false;
    }
    return static_cast<bool>(capacity_gate.wait_for(2, std::chrono::seconds{5}));
  });
  YORI_CHECK(capacity_gate.wait_for(1, std::chrono::seconds{5}));
  submitted = capacity_runner.trigger(SchedulerTrigger::kJobSubmitted, gpu_snapshot());
  if (submitted.accepted()) {
    completion = capacity_runner.try_consume();
    YORI_CHECK(completion.code == SchedulerTaskCompletionCode::kExecutorRejected);
  } else {
    YORI_CHECK(submitted.code == SchedulerTaskSubmitCode::kExecutorRejected);
  }
  YORI_CHECK(capacity_gate.advance_to(2));
  YORI_CHECK(capacity_blocker.get());
  YORI_CHECK(capacity_queue->size() == 1);
  YORI_CHECK(capacity_runtime.executor().get_failure_status().capacity_exhausted_count >= 1);

  capacity_runner.stop_accepting();
  YORI_CHECK(capacity_runtime.shutdown() == ExecutorRuntimeShutdownResult::kCompleted);

  return yori::testing::failure_count == 0 ? 0 : 1;
}
