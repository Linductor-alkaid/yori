#include <yori/version.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_protocol.hpp>
#include <yori/ipc/ipc_transport.hpp>
#include <yori/ipc/uds_client.hpp>
#include <yori/job/job.hpp>
#include <yori/launch/launch_adapter.hpp>
#include <yori/observe/log_sink.hpp>
#include <yori/process/process_supervisor.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/scheduler/scheduler.hpp>
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

  const yori::store::StateMutation empty_mutation;
  if (empty_mutation.entry_count() != 0) {
    return 1;
  }

  yori::queue::QueueErrorCode queue_error{};
  auto queue = yori::queue::GlobalJobQueue::create({1}, queue_error);
  if (!queue || queue_error != yori::queue::QueueErrorCode::kNone || !queue->empty()) {
    return 1;
  }

  if (std::string(yori::scheduler::to_string(yori::scheduler::ScheduleResultCode::kQueueEmpty)) !=
      "QUEUE_EMPTY") {
    return 1;
  }

  // M2：LaunchAdapter 构造可 spawn 的 LaunchPlan；取消策略与日志配置边界可见。
  yori::job::JobSpec launch_spec;
  launch_spec.owner_uid = 1000;
  launch_spec.owner_gid = 1000;
  launch_spec.argv = {"python", "train.py"};
  launch_spec.cwd = "/srv/training";
  launch_spec.env = {{"TRAIN_STEP", "1"}};
  launch_spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};

  yori::launch::IdentityInfo identity;
  identity.uid = 1000;
  identity.gid = 1000;
  identity.username = "trainer";
  identity.home = "/home/trainer";
  identity.shell = "/bin/bash";
  identity.supplementary_groups = {1000};

  yori::launch::DefaultLaunchAdapter adapter;
  const auto plan = adapter.prepare(
      launch_spec, yori::launch::GpuAssignment{yori::gpu::GpuUuid{"GPU-consumer"}, 1, 0},
      yori::launch::LaunchProfile{}, identity);
  if (!plan || !yori::launch::validate(plan.plan)) {
    return 1;
  }

  const yori::process::CancelPolicy cancel_policy;
  if (!cancel_policy.valid() ||
      cancel_policy.grace_period != yori::process::CancelPolicyLimits::kDefaultGracePeriod) {
    return 1;
  }

  // M5：IPC 协议 roundtrip 与客户端适配链接边界（无 Executor 依赖）。
  yori::ipc::IpcRequest ipc_request;
  ipc_request.kind = yori::ipc::IpcRequestKind::kSubmit;
  ipc_request.submit.argv = {"python", "train.py"};
  ipc_request.submit.cwd = "/srv/training";
  std::vector<std::uint8_t> ipc_frame;
  if (!yori::ipc::append_request_frame(ipc_request, ipc_frame) || ipc_frame.size() < 6) {
    return 1;
  }
  const auto decoded_request =
      yori::ipc::decode_request_payload(ipc_frame.data() + 4, ipc_frame.size() - 4);
  if (!decoded_request.ok() || decoded_request.value.submit.argv != ipc_request.submit.argv) {
    return 1;
  }
  yori::ipc::UdsIpcClient ipc_client;
  const auto unreachable =
      ipc_client.call("/nonexistent/yori.sock", ipc_request, std::chrono::milliseconds{50});
  if (unreachable.error != yori::ipc::IpcClientError::kConnectFailed) {
    return 1;
  }

  yori::observe::LogSinkConfig sink_config;
  sink_config.directory = "/var/lib/yori/jobs/1";
  if (!sink_config.valid() ||
      sink_config.max_file_bytes != yori::observe::LogSinkLimits::kDefaultFileBytes) {
    return 1;
  }

  std::printf("consumer linked against yori %s\n", yori::version());
  return 0;
}
