#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "runtime/daemon.hpp"
#include "runtime/executor_runtime.hpp"
#include "testing/fake_gpu_provider.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

// ---------------------------------------------------------------------------
// CLI -> UDS -> daemon 全链路（M5-05）：进程内组装真实 Daemon（Executor owner、
// FakeGpuProvider、InMemoryStateStore、恢复、GPU 观察、IPC 服务），以真实
// `yori` CLI 二进制作为客户端驱动。跨用户授权矩阵无法在单用户 CI 覆盖，
// 由 unit_ipc_service_test 注入 PeerCredentials 承接（多用户补跑条件见 M5 计
// 划风险节）。
// ---------------------------------------------------------------------------

#ifndef YORI_CLI_BIN
#error "integration test requires YORI_CLI_BIN compile definition"
#endif

namespace {

using namespace std::chrono_literals;

std::string make_directory() {
  char pattern[] = "/tmp/yori-ipc-e2e-test-XXXXXX";
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

// 运行真实 CLI；返回退出码，stdout/stderr 落入目录内文件。
int run_cli(const std::string& directory, const std::string& socket_path,
            const std::string& arguments, std::string& stdout_text, std::string& stderr_text) {
  const std::string command = std::string(YORI_CLI_BIN) + " --socket " + socket_path + " " +
                              arguments + " > " + directory + "/out.txt 2> " + directory +
                              "/err.txt";
  const int code = std::system(command.c_str());
  stdout_text = read_file(directory + "/out.txt");
  stderr_text = read_file(directory + "/err.txt");
  return code == -1 ? -1 : WEXITSTATUS(code);
}

}  // namespace

int main() {
  const std::string directory = make_directory();
  const std::string socket_path = directory + "/yori.sock";

  yori::runtime::ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(yori::runtime::ExecutorRuntimeConfig{}, error));

  yori::testing::FakeGpuProvider provider;
  {
    yori::gpu::GpuObservationSnapshot snapshot;
    snapshot.observed_at = std::chrono::system_clock::now();
    yori::gpu::GpuObservation first;
    first.uuid = yori::gpu::GpuUuid{"GPU-e2e-a"};
    first.index = 0;
    first.state = yori::gpu::GpuObservedState::kFree;
    first.telemetry.utilization_percent = 0;
    first.telemetry.memory_used_bytes = 1 << 20;
    first.telemetry.memory_total_bytes = 8 << 20;
    yori::gpu::GpuObservation second;
    second.uuid = yori::gpu::GpuUuid{"GPU-e2e-b"};
    second.index = 1;
    second.state = yori::gpu::GpuObservedState::kExternalBusy;
    snapshot.devices = {first, second};
    YORI_CHECK(provider.replace_observations(snapshot.devices, snapshot.observed_at).ok());
  }

  yori::testing::InMemoryStateStore store;

  yori::runtime::DaemonConfig config;
  config.ipc.socket_path = socket_path;
  config.ipc.socket_mode = 0600;
  config.ipc.request_deadline = 2000ms;

  yori::runtime::Daemon daemon(runtime.executor(), provider, store, config);
  const auto started = daemon.start();
  YORI_CHECK(started.ok());

  std::string out;
  std::string err;
  int code = 0;

  // submit：成功返回 JobId（设计 13.1）。
  code = run_cli(directory, socket_path,
                 "submit --tensorboard-logdir runs/exp -- python train.py --epochs 3", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "Submitted job 1"));

  // queue / ps：QUEUED Job 可见，自有 Job 明细完整。
  code = run_cli(directory, socket_path, "queue", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "JOB") && contains(out, "1") && contains(out, "QUEUED"));

  code = run_cli(directory, socket_path, "ps", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "python train.py --epochs 3"));
  YORI_CHECK(contains(out, "QUEUED"));

  // gpu：观测 + 逻辑状态视图（一台 FREE、一台 EXTERNAL_BUSY）。
  code = run_cli(directory, socket_path, "gpu", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "GPU-e2e-a") && contains(out, "FREE"));
  YORI_CHECK(contains(out, "GPU-e2e-b") && contains(out, "EXTERNAL_BUSY"));

  // logs：未启动的 Job 显式失败（M5 无守护；M6 起有真实日志路径语义）。
  code = run_cli(directory, socket_path, "logs 1", out, err);
  YORI_CHECK(code == 1);
  YORI_CHECK(contains(err, "invalid state"));

  // logs -f：M6 才提供，显式用法错误。
  code = run_cli(directory, socket_path, "logs -f 1", out, err);
  YORI_CHECK(code == 2);

  // cancel：排队期取消成功；重复取消幂等成功；不存在显式失败。
  code = run_cli(directory, socket_path, "cancel 1", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "Cancelled job 1"));

  code = run_cli(directory, socket_path, "cancel 1", out, err);
  // 终态幂等（RULE-04）：协议层对已 CANCELLED 的重复取消返回成功。
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "Cancelled job 1"));

  code = run_cli(directory, socket_path, "cancel 99", out, err);
  YORI_CHECK(code == 1);
  YORI_CHECK(contains(err, "not found"));

  // 取消后队列与 ps 收缩到无条目。
  code = run_cli(directory, socket_path, "queue", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(!contains(out, "QUEUED"));

  // 用法错误：--gpus 2（POST-01）、未知命令、缺失 --。
  code = run_cli(directory, socket_path, "submit --gpus 2 -- python x.py", out, err);
  YORI_CHECK(code == 2);
  code = run_cli(directory, socket_path, "bogus", out, err);
  YORI_CHECK(code == 2);
  code = run_cli(directory, socket_path, "submit python x.py", out, err);
  YORI_CHECK(code == 2);

  // 传输失败：无 daemon 端点 -> 退出码 3。
  code = run_cli(directory, directory + "/missing.sock", "ps", out, err);
  YORI_CHECK(code == 3);

  // daemon 停止后：端点清理，再连失败。
  YORI_CHECK(daemon.stop() == yori::runtime::DaemonStopCode::kStopped);
  {
    struct stat status {};
    YORI_CHECK(::lstat(socket_path.c_str(), &status) != 0);
  }
  code = run_cli(directory, socket_path, "ps", out, err);
  YORI_CHECK(code == 3);

  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "ipc e2e: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("ipc e2e: all checks passed\n");
  return 0;
}
