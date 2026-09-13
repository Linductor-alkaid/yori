#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <yori/ipc/ipc_service.hpp>

#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

namespace {

using namespace yori::ipc;
using yori::job::JobId;
using yori::job::JobState;
using yori::queue::GlobalJobQueue;
using yori::queue::QueueErrorCode;

constexpr std::uint32_t kAliceUid = 1000;
constexpr std::uint32_t kAliceGid = 1000;
constexpr std::uint32_t kBobUid = 1001;
constexpr std::uint32_t kBobGid = 1001;
constexpr std::uint32_t kAdminGid = 3000;

PeerCredentials peer(std::uint32_t uid, std::uint32_t gid) { return PeerCredentials{uid, gid, 1}; }

IpcSubmitRequest valid_submit() {
  IpcSubmitRequest submit;
  submit.argv = {"python", "train.py"};
  submit.cwd = "/srv/training";
  submit.gpu_request = 1;
  return submit;
}

class FakeGpuStatus final : public GpuStatusSource {
 public:
  bool try_get_snapshot(yori::gpu::GpuObservationSnapshot& out) override {
    if (!has_snapshot) {
      return false;
    }
    out = snapshot;
    return true;
  }

  bool has_snapshot{false};
  yori::gpu::GpuObservationSnapshot snapshot{};
};

class FakeLogReader final : public LogSnapshotReader {
 public:
  LogTailResult read_tail(const std::string& path, std::uint32_t max_bytes) override {
    if (fail) {
      return LogTailResult{false, false, {}};
    }
    LogTailResult result;
    result.ok = true;
    result.truncated = content.size() > max_bytes;
    const std::size_t begin = result.truncated ? content.size() - max_bytes : 0;
    const std::size_t end = content.size();
    result.tail.assign(content.begin() + static_cast<std::ptrdiff_t>(begin),
                       content.begin() + static_cast<std::ptrdiff_t>(end));
    static_cast<void>(path);
    return result;
  }

  std::vector<std::uint8_t> content{};
  bool fail{false};
};

class FakeScheduleStatus final : public ScheduleStatusSource {
 public:
  bool try_get_schedule_evaluation(yori::scheduler::ScheduleEvaluation& out) override {
    if (!has_evaluation) {
      return false;
    }
    out = evaluation;
    return true;
  }

  bool has_evaluation{false};
  yori::scheduler::ScheduleEvaluation evaluation{};
};

struct Fixture final {
  Fixture() {
    QueueErrorCode queue_error = QueueErrorCode::kNone;
    queue = GlobalJobQueue::create({}, queue_error);
  }

  yori::testing::InMemoryStateStore store;
  std::unique_ptr<GlobalJobQueue> queue;
  FakeGpuStatus gpu_status;
  FakeLogReader log_reader;
  FakeScheduleStatus schedule_status;
};

// 服务层测试的 JobControl 假实现（M7 起 submit/cancel 委派给守护承载）：执行
// 与 JobManager 相同的 store/queue 变更语义（创建 + 准入 + 回滚 / QUEUED 取消），
// 使本测试聚焦授权、脱敏与响应映射；完整承载语义由 JobManager 测试覆盖。
class FakeJobControl final : public JobControl {
 public:
  explicit FakeJobControl(Fixture& fixture) : fixture_(fixture) {}

  JobSubmitOutcome submit_job(const yori::job::JobSpec& spec) override {
    const auto load = fixture_.store.load();
    if (!load.ok()) {
      return JobSubmitOutcome{JobSubmitOutcome::Code::kStoreFailed, 0,
                              yori::store::to_string(load.code)};
    }
    std::uint64_t max_id = 0;
    for (const auto& record : load.snapshot.jobs) {
      max_id = std::max(max_id, record.id.value());
    }
    yori::store::StoredJob record;
    record.id = JobId{max_id + 1};
    record.spec = spec;
    record.state = JobState::kQueued;
    record.revision = 0;

    yori::store::StateMutation mutation;
    mutation.expected_revision = load.snapshot.revision;
    mutation.create_jobs.push_back(record);
    const auto write = fixture_.store.apply(mutation);
    if (!write.ok()) {
      return JobSubmitOutcome{JobSubmitOutcome::Code::kStoreFailed, 0,
                              yori::store::to_string(write.code)};
    }
    const auto admission = fixture_.queue->admit(record);
    if (!admission.ok()) {
      yori::store::StoredJob cancelled = record;
      cancelled.state = JobState::kCancelled;
      cancelled.revision = 1;
      yori::store::StateMutation rollback;
      rollback.expected_revision = write.revision;
      rollback.update_jobs.push_back(std::move(cancelled));
      const auto rollback_write = fixture_.store.apply(rollback);
      std::string detail = yori::queue::to_string(admission.code);
      if (!rollback_write.ok()) {
        detail += std::string("; rollback failed: ") + yori::store::to_string(rollback_write.code);
      }
      return JobSubmitOutcome{JobSubmitOutcome::Code::kQueueRejected, 0, detail};
    }
    return JobSubmitOutcome{JobSubmitOutcome::Code::kSubmitted, record.id.value(), {}};
  }

  JobCancelOutcome cancel_job(std::uint64_t job_id) override {
    const auto load = fixture_.store.load();
    if (!load.ok()) {
      return JobCancelOutcome{JobCancelOutcome::Code::kStoreFailed, 0,
                              yori::store::to_string(load.code)};
    }
    const yori::store::StoredJob* record = nullptr;
    for (const auto& candidate : load.snapshot.jobs) {
      if (candidate.id.value() == job_id) {
        record = &candidate;
        break;
      }
    }
    if (record == nullptr) {
      return JobCancelOutcome{JobCancelOutcome::Code::kNotFound, 0, "job not found"};
    }
    if (record->state == JobState::kCancelled) {
      return JobCancelOutcome{
          JobCancelOutcome::Code::kCancelled, static_cast<std::uint8_t>(JobState::kCancelled), {}};
    }
    if (record->state == JobState::kQueued) {
      yori::store::StoredJob cancelled = *record;
      cancelled.state = JobState::kCancelled;
      cancelled.revision = record->revision + 1;
      yori::store::StateMutation mutation;
      mutation.expected_revision = load.snapshot.revision;
      mutation.update_jobs.push_back(std::move(cancelled));
      const auto write = fixture_.store.apply(mutation);
      if (!write.ok()) {
        return JobCancelOutcome{JobCancelOutcome::Code::kStoreFailed, 0,
                                yori::store::to_string(write.code)};
      }
      static_cast<void>(fixture_.queue->remove(JobId{job_id}));
      return JobCancelOutcome{
          JobCancelOutcome::Code::kCancelled, static_cast<std::uint8_t>(JobState::kCancelled), {}};
    }
    if (record->state == JobState::kStarting || record->state == JobState::kRunning ||
        record->state == JobState::kStopping) {
      return JobCancelOutcome{
          JobCancelOutcome::Code::kStopping, static_cast<std::uint8_t>(JobState::kStopping), {}};
    }
    return JobCancelOutcome{JobCancelOutcome::Code::kInvalidState,
                            static_cast<std::uint8_t>(record->state),
                            yori::job::to_string(record->state)};
  }

 private:
  Fixture& fixture_;
};

IpcServiceConfig service_config() {
  IpcServiceConfig config;
  config.admin_gids = {kAdminGid};
  return config;
}

IpcService make_service(Fixture& fixture, FakeJobControl& control) {
  return IpcService(service_config(), fixture.store, fixture.gpu_status, fixture.log_reader,
                    control, fixture.schedule_status);
}

IpcResponse submit(IpcService& service, std::uint32_t uid, std::uint32_t gid,
                   IpcSubmitRequest request = valid_submit()) {
  IpcRequest ipc_request;
  ipc_request.kind = IpcRequestKind::kSubmit;
  ipc_request.submit = std::move(request);
  return service.handle(peer(uid, gid), ipc_request);
}

IpcResponse call_ps(IpcService& service, std::uint32_t uid, std::uint32_t gid) {
  IpcRequest request;
  request.kind = IpcRequestKind::kPs;
  return service.handle(peer(uid, gid), request);
}

IpcResponse call_queue(IpcService& service, std::uint32_t uid) {
  IpcRequest request;
  request.kind = IpcRequestKind::kQueue;
  return service.handle(peer(uid, kAliceGid), request);
}

IpcResponse call_cancel(IpcService& service, std::uint32_t uid, std::uint64_t job_id) {
  IpcRequest request;
  request.kind = IpcRequestKind::kCancel;
  request.cancel.job_id = job_id;
  return service.handle(peer(uid, kAliceGid), request);
}

IpcResponse call_gpu(IpcService& service) {
  IpcRequest request;
  request.kind = IpcRequestKind::kGpu;
  return service.handle(peer(kAliceUid, kAliceGid), request);
}

IpcResponse call_logs(IpcService& service, std::uint32_t uid, std::uint64_t job_id,
                      std::uint32_t max_bytes = 4096) {
  IpcRequest request;
  request.kind = IpcRequestKind::kLogs;
  request.logs.job_id = job_id;
  request.logs.max_bytes = max_bytes;
  return service.handle(peer(uid, kAliceGid), request);
}

// 直接在 store 中构造一个非 QUEUED Job（服务测试夹具；revision 从 0 递增）。
void seed_job(yori::testing::InMemoryStateStore& store, JobId id, std::uint32_t owner_uid,
              JobState state, const std::string& log_path = {}) {
  yori::job::JobSpec spec;
  spec.owner_uid = owner_uid;
  spec.owner_gid = owner_uid;
  spec.argv = {"train"};
  spec.cwd = "/srv";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{10}};

  yori::store::StoredJob record;
  record.id = id;
  record.spec = spec;
  record.state = JobState::kQueued;
  record.revision = 0;

  yori::store::StateMutation create;
  create.expected_revision = store.load().snapshot.revision;
  create.create_jobs.push_back(record);
  YORI_CHECK(store.apply(create).ok());

  if (state != JobState::kQueued) {
    yori::store::StoredJob updated = record;
    updated.state = state;
    updated.revision = 1;
    if (!log_path.empty()) {
      updated.execution.log_path = log_path;
    }
    yori::store::StateMutation update;
    update.expected_revision = store.load().snapshot.revision;
    update.update_jobs.push_back(std::move(updated));
    YORI_CHECK(store.apply(update).ok());
  }
}

void test_submit_basics() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  const IpcResponse first = submit(service, kAliceUid, kAliceGid);
  YORI_CHECK(first.error == IpcError::kNone && first.job_id == 1);
  YORI_CHECK(fixture.queue->size() == 1);

  // 身份只来自 peer：Job owner 是提交者，请求无身份字段可伪造。
  const auto snapshot = fixture.store.load().snapshot;
  YORI_CHECK(snapshot.jobs.size() == 1);
  YORI_CHECK(snapshot.jobs[0].spec.owner_uid == kAliceUid);
  YORI_CHECK(snapshot.jobs[0].spec.owner_gid == kAliceGid);

  // 第二个用户提交：JobId 单调（1 -> 2），进入同一队列。
  const IpcResponse second = submit(service, kBobUid, kBobGid);
  YORI_CHECK(second.error == IpcError::kNone && second.job_id == 2);
  YORI_CHECK(fixture.queue->size() == 2);

  // root 提交被拒绝（RULE-09）。
  const IpcResponse root = submit(service, 0, 0);
  YORI_CHECK(root.error == IpcError::kDenied);

  // 非法 JobSpec 映射：空 argv。
  IpcSubmitRequest empty = valid_submit();
  empty.argv.clear();
  const IpcResponse invalid = submit(service, kAliceUid, kAliceGid, std::move(empty));
  YORI_CHECK(invalid.error == IpcError::kInvalidSpec);
  YORI_CHECK(invalid.detail == yori::job::to_string(yori::job::JobSpecErrorCode::kEmptyArguments));

  // 多 GPU（POST-01）拒绝。
  IpcSubmitRequest multi_gpu = valid_submit();
  multi_gpu.gpu_request = 2;
  const IpcResponse multi = submit(service, kAliceUid, kAliceGid, std::move(multi_gpu));
  YORI_CHECK(multi.error == IpcError::kInvalidSpec);
}

void test_queue_capacity_rollback() {
  Fixture fixture;
  QueueErrorCode error = QueueErrorCode::kNone;
  yori::queue::QueueConfig config;
  config.capacity = 1;
  fixture.queue = GlobalJobQueue::create(config, error);
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  const IpcResponse first = submit(service, kAliceUid, kAliceGid);
  YORI_CHECK(first.error == IpcError::kNone && first.job_id == 1);

  const IpcResponse second = submit(service, kBobUid, kBobGid);
  YORI_CHECK(second.error == IpcError::kQueueRejected);
  YORI_CHECK(fixture.queue->size() == 1);

  // 审计事实：被拒 Job 持久化为 CANCELLED（revision 1），id 未占用队列。
  const auto snapshot = fixture.store.load().snapshot;
  YORI_CHECK(snapshot.jobs.size() == 2);
  const auto& rejected = snapshot.jobs[1];
  YORI_CHECK(rejected.id == JobId{2} && rejected.state == JobState::kCancelled &&
             rejected.revision == 1);

  // 终态 id 不复用：腾出队列后，下一个成功提交 id=3。
  YORI_CHECK(call_cancel(service, kAliceUid, 1).error == IpcError::kNone);
  const IpcResponse third = submit(service, kAliceUid, kAliceGid);
  YORI_CHECK(third.error == IpcError::kNone && third.job_id == 3);
}

void test_ps_masking_and_admin() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);
  YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);
  YORI_CHECK(submit(service, kBobUid, kBobGid).error == IpcError::kNone);

  // owner 视角：自有 Job 全量，他人脱敏（仅 JobId/状态/owner/revision）。
  const IpcResponse alice_view = call_ps(service, kAliceUid, kAliceGid);
  YORI_CHECK(alice_view.error == IpcError::kNone && alice_view.jobs.size() == 2);
  YORI_CHECK(!alice_view.jobs[0].masked && alice_view.jobs[0].argv.size() == 2 &&
             alice_view.jobs[0].cwd == "/srv/training");
  YORI_CHECK(alice_view.jobs[1].masked && alice_view.jobs[1].argv.empty() &&
             alice_view.jobs[1].cwd.empty() && !alice_view.jobs[1].tensorboard_logdir);

  // admin（admin_gid 主组匹配）视角：全部全量。
  const IpcResponse admin_view = call_ps(service, kBobUid, kAdminGid);
  YORI_CHECK(admin_view.error == IpcError::kNone && admin_view.jobs.size() == 2);
  YORI_CHECK(!admin_view.jobs[0].masked && !admin_view.jobs[1].masked);

  // 普通他人视角：全部脱敏。
  const IpcResponse bob_view = call_ps(service, kBobUid, kBobGid);
  YORI_CHECK(bob_view.jobs[0].masked && !bob_view.jobs[1].masked);

  // admin_uids（补充组解析结果）同样生效。
  IpcServiceConfig with_admin_uid = service_config();
  with_admin_uid.admin_uids = {kBobUid};
  IpcService uid_admin(with_admin_uid, fixture.store, fixture.gpu_status, fixture.log_reader,
                       control, fixture.schedule_status);
  const IpcResponse uid_admin_view = call_ps(uid_admin, kBobUid, kBobGid);
  YORI_CHECK(!uid_admin_view.jobs[0].masked);
}

void test_ps_limit() {
  Fixture fixture;
  IpcServiceConfig config = service_config();
  config.max_listed_jobs = 2;
  FakeJobControl control(fixture);
  IpcService service(config, fixture.store, fixture.gpu_status, fixture.log_reader, control,
                     fixture.schedule_status);
  for (int i = 0; i < 3; ++i) {
    YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);
  }
  const IpcResponse response = call_ps(service, kAliceUid, kAliceGid);
  YORI_CHECK(response.error == IpcError::kLimit && response.jobs.size() == 2);
}

void test_queue_listing() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);
  YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);
  YORI_CHECK(submit(service, kBobUid, kBobGid).error == IpcError::kNone);

  const IpcResponse response = call_queue(service, kAliceUid);
  YORI_CHECK(response.error == IpcError::kNone && response.queue.size() == 2);
  YORI_CHECK(response.queue[0].job_id == 1 && response.queue[0].owner_uid == kAliceUid);
  YORI_CHECK(response.queue[1].job_id == 2 && response.queue[1].owner_uid == kBobUid);
  YORI_CHECK(static_cast<JobState>(response.queue[0].state) == JobState::kQueued);
  YORI_CHECK(response.queue[0].submit_time_unix_ns > 0);

  // cancel 后队列收缩。
  YORI_CHECK(call_cancel(service, kAliceUid, 1).error == IpcError::kNone);
  const IpcResponse after = call_queue(service, kAliceUid);
  YORI_CHECK(after.queue.size() == 1 && after.queue[0].job_id == 2);
}

void test_gpu_view() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  // 无观测：显式不可用。
  const IpcResponse unavailable = call_gpu(service);
  YORI_CHECK(unavailable.error == IpcError::kNotAvailable);

  // 两台设备：一台 FREE，一台被外部占用；lease 事实优先于 FREE 观测
  // （RULE-05：ALLOCATED）。
  yori::gpu::GpuObservationSnapshot snapshot;
  snapshot.revision = 5;
  snapshot.observed_at = std::chrono::system_clock::now();
  yori::gpu::GpuObservation free_device;
  free_device.uuid = yori::gpu::GpuUuid{"GPU-a"};
  free_device.index = 0;
  free_device.state = yori::gpu::GpuObservedState::kFree;
  free_device.telemetry.utilization_percent = 10;
  free_device.telemetry.memory_used_bytes = 100;
  free_device.telemetry.memory_total_bytes = 1000;
  yori::gpu::GpuObservation busy_device;
  busy_device.uuid = yori::gpu::GpuUuid{"GPU-b"};
  busy_device.index = 1;
  busy_device.state = yori::gpu::GpuObservedState::kExternalBusy;
  snapshot.devices = {free_device, busy_device};
  fixture.gpu_status.snapshot = snapshot;
  fixture.gpu_status.has_snapshot = true;

  // 为 Job 1 建立 GPU-a 的 lease：lease 不变量要求 STARTING/RUNNING 恰好持有
  // 一个 lease，因此状态推进与 lease 获取在同一 mutation。
  seed_job(fixture.store, JobId{1}, kAliceUid, JobState::kQueued);
  yori::store::StoredJob running = fixture.store.load().snapshot.jobs[0];
  running.state = JobState::kStarting;
  running.revision = 1;
  yori::store::StateMutation update;
  update.expected_revision = fixture.store.load().snapshot.revision;
  update.update_jobs.push_back(running);
  update.acquire_leases.push_back(yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-a"}, JobId{1}});
  YORI_CHECK(fixture.store.apply(update).ok());
  running.state = JobState::kRunning;
  running.revision = 2;
  running.execution.identity = yori::process::ProcessIdentity{42, 42, 7};
  yori::store::StateMutation advance;
  advance.expected_revision = fixture.store.load().snapshot.revision;
  advance.update_jobs.push_back(running);
  YORI_CHECK(fixture.store.apply(advance).ok());

  const IpcResponse response = call_gpu(service);
  YORI_CHECK(response.error == IpcError::kNone && response.gpu_revision == 5);
  YORI_CHECK(response.devices.size() == 2);
  YORI_CHECK(response.devices[0].uuid == "GPU-a");
  YORI_CHECK(static_cast<yori::gpu::GpuLogicalState>(response.devices[0].logical_state) ==
             yori::gpu::GpuLogicalState::kAllocated);
  YORI_CHECK(response.devices[0].leased_by_job == std::uint64_t{1});
  YORI_CHECK(response.devices[1].uuid == "GPU-b");
  YORI_CHECK(static_cast<yori::gpu::GpuLogicalState>(response.devices[1].logical_state) ==
             yori::gpu::GpuLogicalState::kExternalBusy);
  YORI_CHECK(!response.devices[1].leased_by_job.has_value());
}

void test_cancel_matrix() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);
  YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);

  // 不存在。
  YORI_CHECK(call_cancel(service, kAliceUid, 99).error == IpcError::kNotFound);

  // 非 owner 非 admin 拒绝（响应携带当前状态上下文）。
  const IpcResponse denied = call_cancel(service, kBobUid, 1);
  YORI_CHECK(denied.error == IpcError::kDenied);
  YORI_CHECK(static_cast<JobState>(denied.state) == JobState::kQueued);

  // admin 可取消他人 Job。
  IpcRequest admin_cancel;
  admin_cancel.kind = IpcRequestKind::kCancel;
  admin_cancel.cancel.job_id = 1;
  const IpcResponse admin_result = service.handle(peer(kBobUid, kAdminGid), admin_cancel);
  YORI_CHECK(admin_result.error == IpcError::kNone);
  YORI_CHECK(static_cast<JobState>(admin_result.state) == JobState::kCancelled);
  YORI_CHECK(fixture.queue->empty());

  // 终态幂等：已 CANCELLED 重复取消成功。
  const IpcResponse repeat = call_cancel(service, kAliceUid, 1);
  YORI_CHECK(repeat.error == IpcError::kNone &&
             static_cast<JobState>(repeat.state) == JobState::kCancelled);

  // 其他终态显式拒绝。
  seed_job(fixture.store, JobId{9}, kAliceUid, JobState::kQueued);
  yori::store::StoredJob cancelled = fixture.store.load().snapshot.jobs.back();
  cancelled.state = JobState::kCancelled;
  cancelled.revision = 1;
  yori::store::StateMutation finish;
  finish.expected_revision = fixture.store.load().snapshot.revision;
  finish.update_jobs.push_back(std::move(cancelled));
  YORI_CHECK(fixture.store.apply(finish).ok());
  YORI_CHECK(call_cancel(service, kAliceUid, 9).error == IpcError::kNone);

  // RUNNING（恢复采纳的 Job）：活动态取消经 JobControl 委派（M7 守护总装），
  // 假实现返回 kStopping；lease 不变量（STARTING 恰好一个 lease）在同一
  // mutation 内满足。
  seed_job(fixture.store, JobId{10}, kAliceUid, JobState::kQueued);
  yori::store::StoredJob starting = fixture.store.load().snapshot.jobs.back();
  starting.state = JobState::kStarting;
  starting.revision = 1;
  yori::store::StateMutation to_starting;
  to_starting.expected_revision = fixture.store.load().snapshot.revision;
  to_starting.update_jobs.push_back(starting);
  to_starting.acquire_leases.push_back(yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-c"}, JobId{10}});
  YORI_CHECK(fixture.store.apply(to_starting).ok());
  const IpcResponse running_cancel = call_cancel(service, kAliceUid, 10);
  YORI_CHECK(running_cancel.error == IpcError::kNone);
  YORI_CHECK(static_cast<JobState>(running_cancel.state) == JobState::kStopping);
}

void test_logs() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);
  // 未启动：QUEUED Job 无 log_path。
  const IpcResponse not_started = call_logs(service, kAliceUid, 1);
  YORI_CHECK(not_started.error == IpcError::kInvalidState);

  // 不存在 / 未授权。
  YORI_CHECK(call_logs(service, kAliceUid, 99).error == IpcError::kNotFound);
  YORI_CHECK(call_logs(service, kBobUid, 1).error == IpcError::kDenied);

  // STARTING 且有 log_path：读取两路尾部（lease 不变量在同一 mutation 满足）。
  seed_job(fixture.store, JobId{5}, kAliceUid, JobState::kQueued);
  yori::store::StoredJob starting = fixture.store.load().snapshot.jobs.back();
  starting.state = JobState::kStarting;
  starting.revision = 1;
  starting.execution.log_path = "/var/lib/yori/jobs/5";
  yori::store::StateMutation update;
  update.expected_revision = fixture.store.load().snapshot.revision;
  update.update_jobs.push_back(std::move(starting));
  update.acquire_leases.push_back(yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-d"}, JobId{5}});
  YORI_CHECK(fixture.store.apply(update).ok());

  std::string big(5000, 'x');
  fixture.log_reader.content.assign(big.begin(), big.end());
  const IpcResponse tail = call_logs(service, kAliceUid, 5, 1024);
  YORI_CHECK(tail.error == IpcError::kNone);
  YORI_CHECK(tail.logs.stdout_tail.size() == 1024 && tail.logs.stdout_truncated);
  YORI_CHECK(tail.logs.stderr_tail.size() == 1024 && tail.logs.stderr_truncated);
  YORI_CHECK(tail.logs.stdout_tail.size() == 1024 && tail.logs.stdout_tail.front() == 'x');

  // max_bytes 超过配置上限时被钳制（请求 64 KiB > 上限默认 256 KiB 不触发；
  // 直接请求超大值验证钳制路径由配置保证，这里验证小值）。
  const IpcResponse small = call_logs(service, kAliceUid, 5, 10);
  YORI_CHECK(small.error == IpcError::kNone && small.logs.stdout_tail.size() == 10);

  // reader 失败：显式不可用。
  fixture.log_reader.fail = true;
  YORI_CHECK(call_logs(service, kAliceUid, 5).error == IpcError::kNotAvailable);

  // admin 可读他人日志。
  fixture.log_reader.fail = false;
  IpcRequest admin_logs;
  admin_logs.kind = IpcRequestKind::kLogs;
  admin_logs.logs.job_id = 5;
  admin_logs.logs.max_bytes = 8;
  const IpcResponse admin_view = service.handle(peer(kBobUid, kAdminGid), admin_logs);
  YORI_CHECK(admin_view.error == IpcError::kNone && admin_view.logs.stdout_tail.size() == 8);
}

void test_file_log_reader() {
  const auto reader = file_log_snapshot_reader();
  const std::string directory = "/tmp/yori-m5-log-reader-test";
  const int prepare = std::system(("rm -rf " + directory + " && mkdir -p " + directory).c_str());
  static_cast<void>(prepare);

  // 写入 100 字节，读取尾部 40 字节。
  const std::string path = directory + "/stdout.log";
  {
    std::ofstream file(path, std::ios::binary);
    for (int i = 0; i < 100; ++i) {
      file.put(static_cast<char>(i));
    }
  }
  LogTailResult tail = reader->read_tail(path, 40);
  YORI_CHECK(tail.ok && tail.truncated && tail.tail.size() == 40);
  YORI_CHECK(tail.tail.front() == static_cast<std::uint8_t>(100 - 40));

  // 全量读取。
  tail = reader->read_tail(path, 128);
  YORI_CHECK(tail.ok && !tail.truncated && tail.tail.size() == 100);

  // 文件缺失：空尾部。
  tail = reader->read_tail(directory + "/missing.log", 10);
  YORI_CHECK(tail.ok && !tail.truncated && tail.tail.empty());

  // 目录而非常规文件：拒绝。
  tail = reader->read_tail(directory, 10);
  YORI_CHECK(!tail.ok);

  const int cleanup = std::system(("rm -rf " + directory).c_str());
  static_cast<void>(cleanup);
}

}  // namespace

// 以 lease 不变量将 Job 推进到 STARTING 并记录 log_path（M6 跟随/tensorboard
// 测试的“已启动”前置；tensorboard_logdir 属于 spec，必须在 create 时写入——
// update 保持 JobSpec 不变）。
void seed_started_job(yori::testing::InMemoryStateStore& store, JobId id, std::uint32_t owner_uid,
                      const std::string& log_path, const std::string& tensorboard_logdir = {}) {
  yori::job::JobSpec spec;
  spec.owner_uid = owner_uid;
  spec.owner_gid = owner_uid;
  spec.argv = {"train"};
  spec.cwd = "/srv";
  if (!tensorboard_logdir.empty()) {
    spec.tensorboard_logdir = tensorboard_logdir;
  }
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{10}};

  yori::store::StoredJob record;
  record.id = id;
  record.spec = spec;
  record.state = JobState::kQueued;
  record.revision = 0;
  yori::store::StateMutation create;
  create.expected_revision = store.load().snapshot.revision;
  create.create_jobs.push_back(record);
  YORI_CHECK(store.apply(create).ok());

  yori::store::StoredJob starting = record;
  starting.state = JobState::kStarting;
  starting.revision = 1;
  starting.execution.log_path = log_path;
  yori::store::StateMutation update;
  update.expected_revision = store.load().snapshot.revision;
  update.update_jobs.push_back(std::move(starting));
  update.acquire_leases.push_back(yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-f"}, id});
  YORI_CHECK(store.apply(update).ok());
}

void test_logs_follow_validation() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  auto follow_request = [](std::uint64_t job_id) {
    IpcRequest request;
    request.kind = IpcRequestKind::kLogsFollow;
    request.logs_follow.job_id = job_id;
    return request;
  };

  // QUEUED Job：未启动显式拒绝。
  YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);
  const IpcResponse not_started =
      service.validate_logs_follow(peer(kAliceUid, kAliceGid), follow_request(1).logs_follow);
  YORI_CHECK(not_started.error == IpcError::kInvalidState);

  // 不存在 / 未授权（owner/admin 矩阵与 logs 快照一致）。
  YORI_CHECK(
      service.validate_logs_follow(peer(kAliceUid, kAliceGid), follow_request(99).logs_follow)
          .error == IpcError::kNotFound);
  YORI_CHECK(
      service.validate_logs_follow(peer(kBobUid, kBobGid), follow_request(1).logs_follow).error ==
      IpcError::kDenied);

  // 已启动：接受并携带当前状态。
  seed_started_job(fixture.store, JobId{5}, kAliceUid, "/var/lib/yori/jobs/5");
  const IpcResponse accepted =
      service.validate_logs_follow(peer(kAliceUid, kAliceGid), follow_request(5).logs_follow);
  YORI_CHECK(accepted.error == IpcError::kNone);
  YORI_CHECK(accepted.logs_follow.job_state == static_cast<std::uint8_t>(JobState::kStarting));

  // admin 可跟随他人 Job；普通 handle 路径（无流式委派）显式 kUnsupported。
  const IpcResponse admin_follow =
      service.validate_logs_follow(peer(kBobUid, kAdminGid), follow_request(5).logs_follow);
  YORI_CHECK(admin_follow.error == IpcError::kNone);
  const IpcResponse plain = service.handle(peer(kAliceUid, kAliceGid), follow_request(5));
  YORI_CHECK(plain.error == IpcError::kUnsupported);

  // store 失败路径。
  fixture.store.fail_with(yori::store::StateStoreErrorCode::kBackendUnavailable);
  YORI_CHECK(service.validate_logs_follow(peer(kAliceUid, kAliceGid), follow_request(5).logs_follow)
                 .error == IpcError::kStoreFailed);
  fixture.store.clear_failure();
}

void test_tensorboard_query() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  auto tensorboard_request = [](std::uint64_t job_id) {
    IpcRequest request;
    request.kind = IpcRequestKind::kTensorboard;
    request.tensorboard.job_id = job_id;
    return request;
  };

  // QUEUED Job（无 logdir 记录）：查询仍可解析 cwd（logdir 解析不依赖启动态）。
  YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);
  const IpcResponse queued = service.handle(peer(kAliceUid, kAliceGid), tensorboard_request(1));
  YORI_CHECK(queued.error == IpcError::kNone);
  YORI_CHECK(!queued.tensorboard.logdir);
  YORI_CHECK(queued.tensorboard.cwd == "/srv/training");

  // 记录了 tensorboard_logdir 的已启动 Job：返回字段原样。
  seed_started_job(fixture.store, JobId{5}, kAliceUid, "/var/lib/yori/jobs/5", "runs/exp9");
  const IpcResponse with_logdir =
      service.handle(peer(kAliceUid, kAliceGid), tensorboard_request(5));
  YORI_CHECK(with_logdir.error == IpcError::kNone);
  YORI_CHECK(with_logdir.tensorboard.logdir == std::string("runs/exp9"));
  YORI_CHECK(with_logdir.tensorboard.cwd == "/srv");

  // 授权矩阵：第三方 DENIED（敏感字段不脱敏、直接拒绝）；admin 放行。
  YORI_CHECK(service.handle(peer(kBobUid, kBobGid), tensorboard_request(5)).error ==
             IpcError::kDenied);
  YORI_CHECK(service.handle(peer(kBobUid, kAdminGid), tensorboard_request(5)).error ==
             IpcError::kNone);
  YORI_CHECK(service.handle(peer(kAliceUid, kAliceGid), tensorboard_request(77)).error ==
             IpcError::kNotFound);
}

// ---------------------------------------------------------------------------
// M8（DEC-011）：v2 SUBMIT 字段映射与 INSPECT 授权/脱敏。
// ---------------------------------------------------------------------------

IpcResponse call_inspect(IpcService& service, std::uint32_t uid, std::uint32_t gid,
                         std::uint64_t job_id) {
  IpcRequest request;
  request.kind = IpcRequestKind::kInspect;
  request.inspect.job_id = job_id;
  return service.handle(peer(uid, gid), request);
}

void test_submit_v2_fields() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  // v2 捕获字段进入 JobSpec（owner 身份仍只来自 peer）。
  IpcSubmitRequest request = valid_submit();
  request.env = {{"PATH", "/opt/conda/bin"},
                 {"HF_TOKEN", "secret-value"},
                 {"LD_LIBRARY_PATH", "/opt/conda/lib"}};
  request.executable = std::string("/opt/conda/bin/python");
  request.env_metadata = IpcEnvMetadata{1, std::string("3.11.5")};
  const IpcResponse response = submit(service, kAliceUid, kAliceGid, std::move(request));
  YORI_CHECK(response.error == IpcError::kNone && response.job_id == 1);

  const auto snapshot = fixture.store.load().snapshot;
  YORI_CHECK(snapshot.jobs.size() == 1);
  const auto& spec = snapshot.jobs[0].spec;
  YORI_CHECK(spec.executable == std::string("/opt/conda/bin/python"));
  YORI_CHECK(spec.env_metadata.has_value());
  if (spec.env_metadata) {
    YORI_CHECK(spec.env_metadata->source == yori::job::EnvSource::kConda);
    YORI_CHECK(spec.env_metadata->python_version == std::string("3.11.5"));
  }
  YORI_CHECK(spec.env.at("LD_LIBRARY_PATH") == "/opt/conda/lib");

  // 非法 executable（相对路径）映射 INVALID_SPEC。
  IpcSubmitRequest bad = valid_submit();
  bad.executable = std::string("relative/python");
  const IpcResponse invalid = submit(service, kAliceUid, kAliceGid, std::move(bad));
  YORI_CHECK(invalid.error == IpcError::kInvalidSpec);
  YORI_CHECK(invalid.detail ==
             yori::job::to_string(yori::job::JobSpecErrorCode::kInvalidExecutable));
}

void test_inspect() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  // 提交一个带捕获的 Job（含敏感名与非敏感名）。
  IpcSubmitRequest request = valid_submit();
  request.env = {{"PATH", "/opt/conda/bin"},
                 {"HF_TOKEN", "secret-value"},
                 {"AWS_SECRET_ACCESS_KEY", "k=value"},
                 {"MY_API_KEY", "abc"},
                 {"PASSWORD", "pw"},
                 {"http_proxy", "http://p:1"}};
  request.executable = std::string("/opt/conda/bin/python");
  request.env_metadata = IpcEnvMetadata{1, std::string("3.11.5")};
  const std::map<std::string, std::string> expected_env = request.env;
  YORI_CHECK(submit(service, kAliceUid, kAliceGid, std::move(request)).error == IpcError::kNone);

  // owner：变量名全可见；敏感名值掩码；非敏感值原样。
  const IpcResponse owner_view = call_inspect(service, kAliceUid, kAliceGid, 1);
  YORI_CHECK(owner_view.error == IpcError::kNone);
  YORI_CHECK(owner_view.inspect.cwd == "/srv/training");
  YORI_CHECK(owner_view.inspect.executable == std::string("/opt/conda/bin/python"));
  YORI_CHECK(owner_view.inspect.argv == valid_submit().argv);
  YORI_CHECK(owner_view.inspect.env_metadata.has_value() &&
             owner_view.inspect.env_metadata->source == 1 &&
             owner_view.inspect.env_metadata->python_version == std::string("3.11.5"));
  YORI_CHECK(owner_view.inspect.env.size() == 6);
  for (const IpcEnvEntry& entry : owner_view.inspect.env) {
    const bool sensitive = entry.name.find("TOKEN") != std::string::npos ||
                           entry.name.find("KEY") != std::string::npos ||
                           entry.name.find("SECRET") != std::string::npos ||
                           entry.name.find("PASSWORD") != std::string::npos;
    YORI_CHECK(entry.masked == sensitive);
    YORI_CHECK(entry.value == (sensitive ? std::string{"***"} : expected_env.at(entry.name)));
  }
  // QUEUED：无分配结果。
  YORI_CHECK(!owner_view.inspect.gpu_uuid.has_value());
  YORI_CHECK(owner_view.inspect.submit_time_unix_ns > 0);

  // 第三方：DENIED（无脱敏视图）；admin：放行且同样掩码。
  YORI_CHECK(call_inspect(service, kBobUid, kBobGid, 1).error == IpcError::kDenied);
  const IpcResponse admin_view = call_inspect(service, kBobUid, kAdminGid, 1);
  YORI_CHECK(admin_view.error == IpcError::kNone);
  YORI_CHECK(admin_view.inspect.env.size() == 6);

  // 不存在的 Job。
  YORI_CHECK(call_inspect(service, kAliceUid, kAliceGid, 77).error == IpcError::kNotFound);

  // 自定义敏感模式（配置级）：默认模式之外的名字按配置判定。
  IpcServiceConfig config = service_config();
  config.sensitive_env_patterns = {"PRIVATE"};
  FakeJobControl control2(fixture);
  IpcService custom(config, fixture.store, fixture.gpu_status, fixture.log_reader, control2,
                    fixture.schedule_status);
  IpcSubmitRequest plain = valid_submit();
  plain.env = {{"HF_TOKEN", "v"}, {"MY_PRIVATE_VAR", "v"}};
  YORI_CHECK(submit(custom, kAliceUid, kAliceGid, std::move(plain)).error == IpcError::kNone);
  const IpcResponse custom_view = call_inspect(custom, kAliceUid, kAliceGid, 2);
  YORI_CHECK(custom_view.error == IpcError::kNone);
  for (const IpcEnvEntry& entry : custom_view.inspect.env) {
    YORI_CHECK(entry.masked == (entry.name == "MY_PRIVATE_VAR"));
  }
}

void test_inspect_allocation_view() {
  Fixture fixture;
  FakeJobControl control(fixture);
  IpcService service = make_service(fixture, control);

  // 已启动（RUNNING）+ lease 的 Job：展示分配结果（lease 事实 + 观测索引）。
  // leaseable 状态与 lease 必须同一 mutation 落盘（lease 矩阵整体校验）。
  seed_job(fixture.store, JobId{1}, kAliceUid, JobState::kQueued);
  const auto seeded = fixture.store.load().snapshot;
  yori::store::StoredJob running;
  for (const auto& record : seeded.jobs) {
    if (record.id == JobId{1}) {
      running = record;
    }
  }
  running.state = JobState::kStarting;  // identity 可选的 leaseable 状态
  running.revision = 1;
  running.execution.log_path = "/var/lib/yori/jobs/1";
  yori::store::StateMutation lease_mutation;
  lease_mutation.update_jobs.push_back(std::move(running));
  lease_mutation.acquire_leases.push_back(
      yori::gpu::GpuLease{yori::gpu::GpuUuid{"GPU-abcdef"}, JobId{1}});
  lease_mutation.expected_revision = seeded.revision;
  YORI_CHECK(fixture.store.apply(lease_mutation).ok());

  fixture.gpu_status.has_snapshot = true;
  yori::gpu::GpuObservation observation;
  observation.uuid = yori::gpu::GpuUuid{"GPU-abcdef"};
  observation.index = 3;
  fixture.gpu_status.snapshot.devices.push_back(observation);

  const IpcResponse view = call_inspect(service, kAliceUid, kAliceGid, 1);
  YORI_CHECK(view.error == IpcError::kNone);
  YORI_CHECK(view.inspect.gpu_uuid == std::string("GPU-abcdef"));
  YORI_CHECK(view.inspect.gpu_index == std::uint32_t{3});
  YORI_CHECK(view.inspect.log_path == std::string("/var/lib/yori/jobs/1"));
  YORI_CHECK(view.inspect.state == static_cast<std::uint8_t>(JobState::kStarting));

  // 无 GPU 观测时仍有 lease uuid，无索引。
  fixture.gpu_status.has_snapshot = false;
  const IpcResponse no_observation = call_inspect(service, kAliceUid, kAliceGid, 1);
  YORI_CHECK(no_observation.error == IpcError::kNone);
  YORI_CHECK(no_observation.inspect.gpu_uuid == std::string("GPU-abcdef"));
  YORI_CHECK(!no_observation.inspect.gpu_index.has_value());
}

// 快照内定位 Job 的 spec（M9 断言辅助）。
const yori::job::JobSpec& require_spec(const yori::store::StateSnapshot& snapshot,
                                       std::uint64_t id) {
  for (const auto& record : snapshot.jobs) {
    if (record.id == JobId{id}) {
      return record.spec;
    }
  }
  std::fprintf(stderr, "required Job %llu spec is missing\n", static_cast<unsigned long long>(id));
  std::exit(1);
}

// ---- M9（DEC-012）：placement 输入解析与 wait_reason 视图 -------------------

void seed_observation(Fixture& fixture) {
  yori::gpu::GpuObservationSnapshot snapshot;
  snapshot.revision = 1;
  snapshot.observed_at = std::chrono::system_clock::now();
  yori::gpu::GpuObservation first;
  first.uuid = yori::gpu::GpuUuid{"GPU-a"};
  first.index = 0;
  first.state = yori::gpu::GpuObservedState::kFree;
  yori::gpu::GpuObservation second;
  second.uuid = yori::gpu::GpuUuid{"GPU-b"};
  second.index = 7;
  second.state = yori::gpu::GpuObservedState::kFree;
  snapshot.devices = {first, second};
  fixture.gpu_status.snapshot = snapshot;
  fixture.gpu_status.has_snapshot = true;
}

yori::store::StoredJob seed_required_job(Fixture& fixture, const char* uuid) {
  yori::job::JobSpec spec;
  spec.owner_uid = kAliceUid;
  spec.owner_gid = kAliceGid;
  spec.argv = {"python", "train.py"};
  spec.cwd = "/srv/training";
  spec.gpu_placement.mode = yori::job::GpuPlacementMode::kRequired;
  spec.gpu_placement.devices.push_back(yori::gpu::GpuUuid{uuid});
  spec.submit_time = std::chrono::system_clock::now();
  yori::store::StoredJob record{JobId{1}, spec, JobState::kQueued, 0};
  yori::store::StateMutation create;
  create.create_jobs = {record};
  YORI_CHECK(fixture.store.apply(create));
  const auto loaded = fixture.store.load();
  YORI_CHECK(fixture.queue->restore(loaded.snapshot));
  return record;
}

void test_m9_placement() {
  // 提交解析：index -> UUID。
  {
    Fixture fixture;
    FakeJobControl control(fixture);
    IpcService service = make_service(fixture, control);
    seed_observation(fixture);

    IpcSubmitRequest request = valid_submit();
    request.gpu_spec = std::string("7");  // GPU-b 的 NVML index
    const IpcResponse accepted = submit(service, kAliceUid, kAliceGid, std::move(request));
    YORI_CHECK(accepted.error == IpcError::kNone);
    const auto loaded = fixture.store.load();
    YORI_CHECK(require_spec(loaded.snapshot, 1).gpu_placement.mode ==
               yori::job::GpuPlacementMode::kRequired);
    YORI_CHECK(require_spec(loaded.snapshot, 1).gpu_placement.devices.front() ==
               yori::gpu::GpuUuid{"GPU-b"});
  }

  // 提交解析：UUID 输入直接命中。
  {
    Fixture fixture;
    FakeJobControl control(fixture);
    IpcService service = make_service(fixture, control);
    seed_observation(fixture);
    IpcSubmitRequest request = valid_submit();
    request.gpu_spec = std::string("GPU-a");
    YORI_CHECK(submit(service, kAliceUid, kAliceGid, std::move(request)).error == IpcError::kNone);
    const auto loaded = fixture.store.load();
    YORI_CHECK(require_spec(loaded.snapshot, 1).gpu_placement.devices.front() ==
               yori::gpu::GpuUuid{"GPU-a"});
  }

  // 无观测：placement 无法解析，显式 kNotAvailable（观测就绪后重试可行）。
  {
    Fixture fixture;
    FakeJobControl control(fixture);
    IpcService service = make_service(fixture, control);
    IpcSubmitRequest request = valid_submit();
    request.gpu_spec = std::string("0");
    const IpcResponse response = submit(service, kAliceUid, kAliceGid, std::move(request));
    YORI_CHECK(response.error == IpcError::kNotAvailable);
  }

  // 解析失败矩阵：未知 index / 未知 UUID / 与 gpu_request 互斥。
  {
    Fixture fixture;
    FakeJobControl control(fixture);
    IpcService service = make_service(fixture, control);
    seed_observation(fixture);

    IpcSubmitRequest bad_index = valid_submit();
    bad_index.gpu_spec = std::string("9");
    IpcResponse response = submit(service, kAliceUid, kAliceGid, std::move(bad_index));
    YORI_CHECK(response.error == IpcError::kInvalidSpec);
    YORI_CHECK(response.detail.find("GPU_INDEX_NOT_FOUND") != std::string::npos);

    IpcSubmitRequest bad_uuid = valid_submit();
    bad_uuid.gpu_spec = std::string("GPU-nope");
    response = submit(service, kAliceUid, kAliceGid, std::move(bad_uuid));
    YORI_CHECK(response.error == IpcError::kInvalidSpec);
    YORI_CHECK(response.detail.find("GPU_UUID_NOT_FOUND") != std::string::npos);

    IpcSubmitRequest conflict = valid_submit();
    conflict.gpu_spec = std::string("0");
    conflict.gpu_request = 2;
    response = submit(service, kAliceUid, kAliceGid, std::move(conflict));
    YORI_CHECK(response.error == IpcError::kInvalidSpec);
    YORI_CHECK(response.detail.find("PLACEMENT_GPU_REQUEST_CONFLICT") != std::string::npos);
  }

  // ps/queue 的 wait_reason：owner 附目标 UUID，脱敏视图只有原因。
  {
    Fixture fixture;
    FakeJobControl control(fixture);
    IpcService service = make_service(fixture, control);
    static_cast<void>(seed_required_job(fixture, "GPU-a"));

    // 尚无评估：wait_reason 为空。
    IpcResponse ps = call_ps(service, kAliceUid, kAliceGid);
    YORI_CHECK(ps.error == IpcError::kNone && ps.jobs[0].wait_reason == 0);
    IpcResponse queue_view = call_queue(service, kAliceUid);
    YORI_CHECK(queue_view.queue[0].wait_reason == 0);

    // 最近一次评估：目标被 lease。
    fixture.schedule_status.has_evaluation = true;
    fixture.schedule_status.evaluation.skipped.push_back(
        {JobId{1}, yori::scheduler::WaitReason::kAffinityGpuAllocated,
         yori::gpu::GpuUuid{"GPU-a"}});

    ps = call_ps(service, kAliceUid, kAliceGid);
    YORI_CHECK(ps.jobs[0].wait_reason ==
               static_cast<std::uint8_t>(yori::scheduler::WaitReason::kAffinityGpuAllocated));
    YORI_CHECK(ps.jobs[0].wait_detail == std::string("GPU-a"));

    // 非 owner 非 admin：原因可见，placement 明细（UUID）不传输。
    ps = call_ps(service, kBobUid, kBobGid);
    YORI_CHECK(ps.jobs[0].masked);
    YORI_CHECK(ps.jobs[0].wait_reason ==
               static_cast<std::uint8_t>(yori::scheduler::WaitReason::kAffinityGpuAllocated));
    YORI_CHECK(!ps.jobs[0].wait_detail.has_value());

    queue_view = call_queue(service, kBobUid);
    YORI_CHECK(queue_view.queue[0].wait_reason ==
               static_cast<std::uint8_t>(yori::scheduler::WaitReason::kAffinityGpuAllocated));
    YORI_CHECK(!queue_view.queue[0].wait_detail.has_value());
    queue_view = call_queue(service, kAliceUid);
    YORI_CHECK(queue_view.queue[0].wait_detail == std::string("GPU-a"));

    // 评估中不存在的 Job（如新提交未评估）：保持空。
    fixture.schedule_status.evaluation.skipped.clear();
    ps = call_ps(service, kAliceUid, kAliceGid);
    YORI_CHECK(ps.jobs[0].wait_reason == 0);
  }

  // inspect placement：owner/admin 视图携带 mode 与目标。
  {
    Fixture fixture;
    FakeJobControl control(fixture);
    IpcService service = make_service(fixture, control);
    static_cast<void>(seed_required_job(fixture, "GPU-b"));

    const IpcResponse owner_view = call_inspect(service, kAliceUid, kAliceGid, 1);
    YORI_CHECK(owner_view.error == IpcError::kNone);
    YORI_CHECK(owner_view.inspect.gpu_placement_mode ==
               static_cast<std::uint8_t>(yori::job::GpuPlacementMode::kRequired));
    YORI_CHECK(owner_view.inspect.gpu_placement_device == std::string("GPU-b"));

    const IpcResponse admin_view = call_inspect(service, kBobUid, kAdminGid, 1);
    YORI_CHECK(admin_view.error == IpcError::kNone);
    YORI_CHECK(admin_view.inspect.gpu_placement_device == std::string("GPU-b"));
  }
}

int main() {
  test_submit_basics();
  test_queue_capacity_rollback();
  test_ps_masking_and_admin();
  test_ps_limit();
  test_queue_listing();
  test_gpu_view();
  test_cancel_matrix();
  test_logs();
  test_file_log_reader();
  test_logs_follow_validation();
  test_tensorboard_query();
  test_submit_v2_fields();
  test_inspect();
  test_inspect_allocation_view();
  test_m9_placement();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "ipc service: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("ipc service: all checks passed\n");
  return 0;
}
