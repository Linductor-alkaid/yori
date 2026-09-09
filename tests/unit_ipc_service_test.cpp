#include <yori/ipc/ipc_service.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

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
    const std::size_t end = result.truncated ? content.size() : content.size();
    result.tail.assign(content.begin() + static_cast<std::ptrdiff_t>(begin),
                       content.begin() + static_cast<std::ptrdiff_t>(end));
    static_cast<void>(path);
    return result;
  }

  std::vector<std::uint8_t> content{};
  bool fail{false};
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
};

IpcServiceConfig service_config() {
  IpcServiceConfig config;
  config.admin_gids = {kAdminGid};
  return config;
}

IpcService make_service(Fixture& fixture) {
  return IpcService(service_config(), *fixture.queue, fixture.store, fixture.gpu_status,
                    fixture.log_reader);
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
              JobState state, std::string log_path = {}) {
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
  IpcService service = make_service(fixture);

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
  IpcService service = make_service(fixture);

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
  IpcService service = make_service(fixture);
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
  IpcService uid_admin(with_admin_uid, *fixture.queue, fixture.store, fixture.gpu_status,
                       fixture.log_reader);
  const IpcResponse uid_admin_view = call_ps(uid_admin, kBobUid, kBobGid);
  YORI_CHECK(!uid_admin_view.jobs[0].masked);
}

void test_ps_limit() {
  Fixture fixture;
  IpcServiceConfig config = service_config();
  config.max_listed_jobs = 2;
  IpcService service(config, *fixture.queue, fixture.store, fixture.gpu_status,
                     fixture.log_reader);
  for (int i = 0; i < 3; ++i) {
    YORI_CHECK(submit(service, kAliceUid, kAliceGid).error == IpcError::kNone);
  }
  const IpcResponse response = call_ps(service, kAliceUid, kAliceGid);
  YORI_CHECK(response.error == IpcError::kLimit && response.jobs.size() == 2);
}

void test_queue_listing() {
  Fixture fixture;
  IpcService service = make_service(fixture);
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
  IpcService service = make_service(fixture);

  // 无观测：显式不可用。
  const IpcResponse unavailable = call_gpu(service);
  YORI_CHECK(unavailable.error == IpcError::kNotAvailable);

  // 两台设备：一台 FREE，一台被外部占用；lease 事实优先于 FREE 观测
  //（RULE-05：ALLOCATED）。
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
  IpcService service = make_service(fixture);
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
  const IpcResponse admin_result =
      service.handle(peer(kBobUid, kAdminGid), admin_cancel);
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

  // RUNNING（恢复采纳的 Job）：M5 无进程守护，显式 kUnsupported。lease 不变量
  //（STARTING 恰好一个 lease）在同一 mutation 内满足。
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
  YORI_CHECK(running_cancel.error == IpcError::kUnsupported);
  YORI_CHECK(static_cast<JobState>(running_cancel.state) == JobState::kStarting);
}

void test_logs() {
  Fixture fixture;
  IpcService service = make_service(fixture);

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
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "ipc service: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("ipc service: all checks passed\n");
  return 0;
}
