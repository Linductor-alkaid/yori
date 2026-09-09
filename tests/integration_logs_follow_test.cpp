#include <sys/wait.h>
#include <unistd.h>
#include <yori/version.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <yori/observe/log_sink.hpp>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "runtime/daemon.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/log_pump.hpp"
#include "testing/fake_gpu_provider.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

// ---------------------------------------------------------------------------
// logs -f 与 tensorboard 的全链路（M6-05）：进程内组装真实 Daemon（Executor
// owner、FakeGpuProvider、InMemoryStateStore、LogStreamer + LogFollowService），
// 真实子进程产日志 -> LogPump 观察者 -> Topic -> 会话 worker -> UDS 流式帧 ->
// 真实 `yori` CLI。tensorboard 以 PATH 注入假二进制验证 CLI 契约（真实
// TensorBoard 不在 CI，补跑条件见 M6 计划风险节）。
// ---------------------------------------------------------------------------

#ifndef YORI_CLI_BIN
#error "integration test requires YORI_CLI_BIN compile definition"
#endif

namespace {

using namespace yori;
using namespace std::chrono_literals;
using namespace yori::runtime;
using observe::LogStreamKind;
using yori::job::JobId;
using yori::job::JobState;

std::string make_directory() {
  char pattern[] = "/tmp/yori-m6-e2e-test-XXXXXX";
  char* directory = ::mkdtemp(pattern);
  YORI_CHECK(directory != nullptr);
  return directory;
}

std::string read_file(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::string data;
  char buffer[4096];
  size_t count = 0;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, count);
  }
  std::fclose(file);
  return data;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// 同步运行 CLI（快照类命令）。
int run_cli(const std::string& directory, const std::string& socket_path,
            const std::string& arguments, const std::string& path_prefix, std::string& stdout_text,
            std::string& stderr_text) {
  const std::string command = path_prefix + " " + std::string(YORI_CLI_BIN) + " --socket " +
                              socket_path + " " + arguments + " > " + directory + "/out.txt 2> " +
                              directory + "/err.txt";
  const int code = std::system(command.c_str());
  stdout_text = read_file(directory + "/out.txt");
  stderr_text = read_file(directory + "/err.txt");
  return code == -1 ? -1 : WEXITSTATUS(code);
}

// 异步启动 CLI（跟随类命令）：fork + exec，输出落文件，返回 pid。
pid_t start_cli(const std::string& directory, const std::string& socket_path,
                const std::string& arguments, const std::string& path_prefix) {
  const pid_t child = ::fork();
  if (child != 0) {
    return child;
  }
  const std::string command = path_prefix + " " + std::string(YORI_CLI_BIN) + " --socket " +
                              socket_path + " " + arguments + " > " + directory +
                              "/follow-out.txt" + " 2> " + directory + "/follow-err.txt";
  const int code = std::system(command.c_str());
  ::_exit(code == -1 ? 125 : WEXITSTATUS(code));
}

int wait_cli(pid_t child, int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited += 20) {
    int status = 0;
    const pid_t done = ::waitpid(child, &status, WNOHANG);
    if (done == child) {
      return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
    }
    std::this_thread::sleep_for(20ms);
  }
  int status = 0;
  static_cast<void>(::waitpid(child, &status, 0));
  return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
}

// 泵观察者 -> daemon 的 LogStreamer 桥（EXEC-04 数据面）。
class StreamerBridge final : public LogChunkObserver {
 public:
  explicit StreamerBridge(LogStreamer& streamer) : streamer_(streamer) {}

  void on_chunk(const yori::job::JobId& job, LogStreamKind stream, std::string_view data,
                std::uint64_t begin_offset, std::uint64_t end_offset) override {
    static_cast<void>(streamer_.publish_chunk(job, stream, data, begin_offset, end_offset));
  }

  void on_drop(const yori::job::JobId& job, LogStreamKind stream, std::uint64_t offset,
               std::uint64_t dropped_bytes) override {
    static_cast<void>(streamer_.publish_drop_marker(job, stream, offset, dropped_bytes));
  }

 private:
  LogStreamer& streamer_;
};

// 在 store 内造一个已启动 Job（STARTING + lease + log_path；可选 logdir 属于
// spec，须在 create 时写入）。须在 daemon 启动前调用（InMemoryStateStore 单
// owner；启动后恢复将其收敛为 LOST，log_path 保留——观察面不依赖状态机推进）。
void seed_started_job(yori::testing::InMemoryStateStore& store, std::uint64_t id,
                      const std::string& log_path, const std::string& tensorboard_logdir = {}) {
  yori::job::JobSpec spec;
  spec.owner_uid = static_cast<std::uint32_t>(::geteuid());
  spec.owner_gid = static_cast<std::uint32_t>(::getegid());
  spec.argv = {"train"};
  spec.cwd = "/srv";
  if (!tensorboard_logdir.empty()) {
    spec.tensorboard_logdir = tensorboard_logdir;
  }
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{10}};

  yori::store::StoredJob record;
  record.id = JobId{id};
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
  // lease 一对一：每个 Job 使用不同设备。
  update.acquire_leases.push_back(yori::gpu::GpuLease{
      yori::gpu::GpuUuid{std::string("GPU-e2e-") + std::to_string(id)}, JobId{id}});
  YORI_CHECK(store.apply(update).ok());
}

}  // namespace

int main() {
  const std::string directory = make_directory();
  const std::string socket_path = directory + "/yori.sock";
  const std::string jobs_root = directory + "/jobs";
  const std::string job_dir = jobs_root + "/1";
  YORI_CHECK(std::system(("mkdir -p " + job_dir).c_str()) == 0);

  yori::runtime::ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(yori::runtime::ExecutorRuntimeConfig{}, error));

  yori::testing::FakeGpuProvider provider;
  {
    yori::gpu::GpuObservationSnapshot snapshot;
    snapshot.observed_at = std::chrono::system_clock::now();
    yori::gpu::GpuObservation device;
    device.uuid = yori::gpu::GpuUuid{"GPU-e2e"};
    device.index = 0;
    device.state = yori::gpu::GpuObservedState::kFree;
    snapshot.devices = {device};
    YORI_CHECK(provider.replace_observations(snapshot.devices, snapshot.observed_at).ok());
  }
  yori::testing::InMemoryStateStore store;

  // 播种在 daemon 启动前（store 单 owner）：Job 1 带日志目录与 tensorboard
  // logdir；Job 2 无流式注册（kNotAvailable 路径）。恢复将两者收敛为 LOST，
  // log_path 保留。
  seed_started_job(store, 1, job_dir, "tb-run");
  seed_started_job(store, 2, jobs_root + "/2");

  DaemonConfig config;
  config.ipc.socket_path = socket_path;
  config.ipc.socket_mode = 0600;
  config.ipc.request_deadline = 2000ms;

  Daemon daemon(runtime.executor(), provider, store, config);
  const auto started = daemon.start();
  YORI_CHECK(started.ok());

  // Job 1 注册日志源。
  std::string streamer_error;
  YORI_CHECK(daemon.log_streamer().register_job(JobId{1}, streamer_error) ==
             LogRegisterCode::kRegistered);

  std::string out;
  std::string err;
  int code = 0;

  // ---- logs 快照仍工作（M5 语义不回归） --------------------------------------
  {
    LogPump pump(runtime.executor());
    YORI_CHECK(pump.start().ok());

    observe::LogSinkConfig sink_config;
    sink_config.directory = job_dir;
    observe::LogSinkOpenOptions sink_options;
    sink_options.owner_uid = static_cast<std::uint32_t>(::geteuid());
    sink_options.owner_gid = static_cast<std::uint32_t>(::getegid());
    observe::LogSink sink;
    std::string open_error;
    YORI_CHECK(sink.open(sink_config, sink_options, open_error) ==
               observe::LogSinkErrorCode::kNone);

    process::ProcessSupervisor supervisor;
    auto spawned = supervisor.spawn(yori::testing::self_plan(
        {"/bin/sh", "-c", "sleep 0.3; echo follow-line-1; sleep 0.3; echo follow-line-2"}));
    YORI_CHECK(spawned);
    if (spawned) {
      LogPumpJobInput input;
      input.job = JobId{1};
      input.stdout_read = std::move(spawned.stdout_read);
      input.stderr_read = std::move(spawned.stderr_read);
      input.sink = std::move(sink);
      input.observer = std::make_shared<StreamerBridge>(daemon.log_streamer());
      YORI_CHECK(pump.attach(std::move(input)).ok());

      // CLI 在子进程输出前完成订阅（子进程先睡 0.3s）。
      const pid_t cli = start_cli(directory, socket_path, "logs -f 1", "");
      YORI_CHECK(cli > 0);

      LogPumpDone done;
      YORI_CHECK(pump.receive_done_for(done, 10s));
      YORI_CHECK(done.completed);
      YORI_CHECK(yori::testing::wait_for_exit(supervisor).status.is_success());

      // 终态：EOF 帧 -> CLI 退出码与 FINISHED 对齐（0）。
      YORI_CHECK(daemon.log_streamer().finish_job(
                     JobId{1}, static_cast<std::uint8_t>(JobState::kFinished),
                     yori::ipc::IpcExitStatus{true, 0}) == LogFinishCode::kFinished);
      YORI_CHECK(wait_cli(cli, 10000) == 0);

      out = read_file(directory + "/follow-out.txt");
      err = read_file(directory + "/follow-err.txt");
      YORI_CHECK(contains(out, "follow-line-1"));
      YORI_CHECK(contains(out, "follow-line-2"));

      // 断线重连：--since-stdout 0 从头回放（含已落盘并发布的块）。
      code = run_cli(directory, socket_path, "logs -f --since-stdout 0 1", "", out, err);
      YORI_CHECK(code == 0);
      YORI_CHECK(contains(out, "follow-line-1"));
      YORI_CHECK(contains(out, "follow-line-2"));
    }
    pump.stop();
  }

  // ---- 落盘核验：管道字节同时落盘（观察不影响落盘主路径） ----------------------
  {
    const std::string stdout_log = read_file(job_dir + "/stdout.log");
    YORI_CHECK(contains(stdout_log, "follow-line-1"));
    YORI_CHECK(contains(stdout_log, "follow-line-2"));
  }

  // ---- tensorboard：PATH 注入假二进制，验证解析优先级与参数 -------------------
  {
    const std::string bin_dir = directory + "/fakebin";
    YORI_CHECK(std::system(("mkdir -p " + bin_dir + " && printf '#!/bin/sh\\necho \"$@\" > " +
                            directory + "/tb-args.txt\\n' > " + bin_dir +
                            "/tensorboard && chmod +x " + bin_dir + "/tensorboard")
                               .c_str()) == 0);
    const std::string path_prefix = "PATH=" + bin_dir + ":$PATH";

    // spec.tensorboard_logdir = "tb-run"（相对 cwd /srv）-> --logdir /srv/tb-run。
    code = run_cli(directory, socket_path, "tensorboard 1 --port 6123", path_prefix, out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(out, "http://127.0.0.1:6123/"));
    const std::string tb_args = read_file(directory + "/tb-args.txt");
    YORI_CHECK(contains(tb_args, "--logdir /srv/tb-run"));
    YORI_CHECK(contains(tb_args, "--port 6123"));
    YORI_CHECK(contains(tb_args, "--host 127.0.0.1"));

    // --logdir 参数优先（相对 Job cwd 解析）。
    code =
        run_cli(directory, socket_path, "tensorboard 1 --logdir custom-run", path_prefix, out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(read_file(directory + "/tb-args.txt"), "--logdir /srv/custom-run"));

    // 端口 0：URL 由 TensorBoard 自行打印（CLI 只说明）。
    code = run_cli(directory, socket_path, "tensorboard 1", path_prefix, out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(out, "port auto-assigned"));

    // 假二进制不存在（PATH 干净）：CLI 显式失败。
    code = run_cli(directory, socket_path, "tensorboard 1 --port 6124",
                   "PATH=/nonexistent-yori-bin", out, err);
    YORI_CHECK(code == 1);
    YORI_CHECK(contains(err, "tensorboard"));
  }

  // ---- 无日志源 Job：logs -f 显式 kNotAvailable（不伪造流） -------------------
  {
    code = run_cli(directory, socket_path, "logs -f 2", "", out, err);
    YORI_CHECK(code == 1);
    YORI_CHECK(contains(err, "no live log source"));
  }

  YORI_CHECK(daemon.stop() == DaemonStopCode::kStopped);
  YORI_CHECK(runtime.shutdown() == ExecutorRuntimeShutdownResult::kCompleted);

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "m6 e2e: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("m6 e2e: all checks passed\n");
  return 0;
}
