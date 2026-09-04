#include <yori/version.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/job/job.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/store/state_store.hpp>

// 安装后最小 consumer（M0-05）：验证安装的公共头与导出库可用。
int main() {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};

  yori::job::JobCreationError error;
  auto job = yori::job::Job::create(yori::job::JobId{1}, std::move(spec), error);
  if (!job) {
    std::fprintf(stderr, "consumer failed to create Job (%d)\n", static_cast<int>(error.code));
    return 1;
  }

  const auto logical_state =
      yori::gpu::derive_logical_state(yori::gpu::GpuObservedState::kFree, false);
  if (logical_state != yori::gpu::GpuLogicalState::kFree ||
      std::string(yori::gpu::to_string(logical_state)) != "FREE") {
    return 1;
  }

  yori::store::StateMutation empty_mutation;
  if (empty_mutation.entry_count() != 0) {
    return 1;
  }

  yori::queue::QueueErrorCode queue_error{};
  auto queue = yori::queue::GlobalJobQueue::create({1}, queue_error);
  if (!queue || queue_error != yori::queue::QueueErrorCode::kNone || !queue->empty()) {
    return 1;
  }

  std::printf("consumer linked against yori %s\n", yori::version());
  return 0;
}
