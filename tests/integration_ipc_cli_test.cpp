#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
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

// 以指定环境前缀运行真实 CLI（env -i 起步，模拟"已激活环境"的提交终端）。
int run_cli_env(const std::string& directory, const std::string& socket_path,
                const std::string& env_prefix, const std::string& arguments,
                std::string& stdout_text, std::string& stderr_text) {
  const std::string command = env_prefix + " " + YORI_CLI_BIN + " --socket " + socket_path + " " +
                              arguments + " > " + directory + "/out.txt 2> " + directory +
                              "/err.txt";
  const int code = std::system(command.c_str());
  stdout_text = read_file(directory + "/out.txt");
  stderr_text = read_file(directory + "/err.txt");
  return code == -1 ? -1 : WEXITSTATUS(code);
}

// 构造假 conda 环境（M8 语义一致性用）：bin/fake-python 打印执行上下文标记
// 后短暂驻留，供运行中 inspect 与日志断言。
std::string write_fake_conda_env(const std::string& directory) {
  const std::string root = directory + "/fakeenv";
  const std::string bin = root + "/bin";
  YORI_CHECK(::mkdir(root.c_str(), 0755) == 0 || errno == EEXIST);
  YORI_CHECK(::mkdir(bin.c_str(), 0755) == 0 || errno == EEXIST);
  const std::string script = bin + "/fake-python";
  FILE* file = std::fopen(script.c_str(), "w");
  YORI_CHECK(file != nullptr);
  static_cast<void>(
      std::fputs("#!/bin/sh\n"
                 "echo \"argv0=$0\"\n"
                 "echo \"conda=$CONDA_PREFIX\"\n"
                 "echo \"ldp=$LD_LIBRARY_PATH\"\n"
                 "echo \"job=$YORI_JOB_ID\"\n"
                 "echo \"gpu=$YORI_GPU_UUID\"\n"
                 "echo \"token=$M8_TOKEN\"\n"
                 "sleep 3\n",
                 file));
  static_cast<void>(std::fclose(file));
  YORI_CHECK(::chmod(script.c_str(), 0755) == 0);
  return root;
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

  // 所有权串行化包装（M7）：IPC 读与 JobManager 写（StoreTaskRunner）对单
  // owner 后端的并发访问经 SerialStateStore 互斥串行化。
  auto store = std::make_unique<yori::runtime::SerialStateStore>(
      std::make_unique<yori::testing::InMemoryStateStore>());

  yori::runtime::DaemonConfig config;
  config.ipc.socket_path = socket_path;
  config.ipc.socket_mode = 0600;
  config.ipc.request_deadline = 5000ms;
  config.gpu.sample_period = 200ms;
  config.job_manager.log_root = directory + "/jobs";
  config.job_manager.cancel_grace = 500ms;

  yori::runtime::Daemon daemon(runtime.executor(), provider, std::move(store), config);
  const auto started = daemon.start();
  YORI_CHECK(started.ok());

  std::string out;
  std::string err;
  int code = 0;

  // 守护总装（M7）下的全链路：submit -> FIFO 调度 -> 真实 spawn -> 日志 ->
  // 取消升级 -> lease 释放 -> 队首推进。
  // ps 行级状态等待：按"行首 JobId + 状态列"匹配，避免他行 FINISHED 造成
  // 子串误报（M8 场景中多 Job 并存时必须精确）。
  const auto ps_state_of = [&](std::uint64_t job_id, std::string& line_out) {
    code = run_cli(directory, socket_path, "ps", out, err);
    YORI_CHECK(code == 0);
    const std::string prefix = std::to_string(job_id) + " ";
    std::size_t begin = 0;
    while (begin < out.size()) {
      const std::size_t end = out.find('\n', begin);
      const std::size_t length = (end == std::string::npos ? out.size() : end) - begin;
      const std::string line = out.substr(begin, length);
      if (line.rfind(prefix, 0) == 0) {
        line_out = line;
        return true;
      }
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
    }
    return false;
  };

  const auto wait_for_state = [&](std::uint64_t job_id, const char* state) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
      std::string line;
      if (ps_state_of(job_id, line) && contains(line, state)) {
        return true;
      }
      std::this_thread::sleep_for(100ms);
    }
    return false;
  };

  // job 1：长驻训练占住唯一 FREE GPU。
  code = run_cli(directory, socket_path, "submit -- /bin/sh -c 'sleep 30'", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "Submitted job 1"));
  YORI_CHECK(wait_for_state(1, "RUNNING"));

  // job 2：无空闲 GPU，保持 QUEUED（FIFO 头部阻塞，不绕过）。
  code = run_cli(directory, socket_path,
                 "submit -- /bin/sh -c 'echo e2e-stdout; echo e2e-stderr 1>&2'", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "Submitted job 2"));
  std::this_thread::sleep_for(300ms);

  code = run_cli(directory, socket_path, "queue", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "2") && contains(out, "QUEUED"));

  code = run_cli(directory, socket_path, "ps", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "RUNNING"));
  YORI_CHECK(contains(out, "QUEUED"));

  // gpu：观测 + 逻辑状态视图（唯一 FREE GPU 已被 lease -> ALLOCATED；
  // 另一台 EXTERNAL_BUSY）。
  code = run_cli(directory, socket_path, "gpu", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "GPU-e2e-a") && contains(out, "ALLOCATED"));
  YORI_CHECK(contains(out, "GPU-e2e-b") && contains(out, "EXTERNAL_BUSY"));

  // 运行中 Job 的取消（SIGTERM -> 宽限 -> SIGKILL 路径）：响应 STOPPING，
  // 随后终态 CANCELLED 并释放 lease -> job 2 自动调度（队首推进）。
  code = run_cli(directory, socket_path, "cancel 1", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "Cancelled job 1"));
  YORI_CHECK(wait_for_state(1, "CANCELLED"));

  // FIFO 链式启动：前一个释放 GPU 后，后一个自动 RUNNING -> FINISHED。
  YORI_CHECK(wait_for_state(2, "FINISHED"));

  // 日志快照：job 2 的两路输出落盘可见（stdout 走 CLI stdout、stderr 走
  // CLI stderr）。
  code = run_cli(directory, socket_path, "logs 2", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "e2e-stdout"));
  YORI_CHECK(contains(err, "e2e-stderr"));

  // logs -f：对已终态 Job 以 since=0 回放全部窗口后 EOF，退出码与终态对齐
  // （FINISHED -> 0）；无 since 时按设计从订阅时刻的流末开始（空回放）。
  code = run_cli(directory, socket_path, "logs -f 2 --since-stdout 0 --since-stderr 0", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "e2e-stdout"));
  YORI_CHECK(contains(err, "e2e-stderr"));

  // ---- M8（DEC-011）：执行上下文捕获的端到端语义一致性 -----------------------
  {
    const std::string fake_root = write_fake_conda_env(directory);
    const std::string env_prefix = "env -i PATH=" + fake_root +
                                   "/bin:/usr/bin:/bin CONDA_PREFIX=" + fake_root +
                                   " LD_LIBRARY_PATH=" + fake_root + "/lib VIRTUAL_ENV=/old-venv";

    // 已激活 conda 环境提交：白名单捕获 + executable 按捕获 PATH 解析。
    code = run_cli_env(directory, socket_path, env_prefix,
                       "submit --env M8_TOKEN=secret-e2e -- fake-python", out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(out, "Submitted job 3"));
    YORI_CHECK(wait_for_state(3, "RUNNING"));

    // 运行中 inspect（owner）：来源 conda、executable 为解析出的绝对路径、
    // 敏感值掩码（原值不出现在输出）、分配结果为 lease 的 GPU。
    code = run_cli(directory, socket_path, "inspect 3", out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(out, "environment: conda"));
    YORI_CHECK(contains(out, (fake_root + "/bin/fake-python").c_str()));
    YORI_CHECK(contains(out, "assigned:    GPU-e2e-a"));
    YORI_CHECK(contains(out, "M8_TOKEN=***"));
    YORI_CHECK(!contains(out, "secret-e2e"));

    // 快照语义 + 四层合并 + 资源块注入：训练进程读到捕获值与 Yori 资源键。
    YORI_CHECK(wait_for_state(3, "FINISHED"));
    code = run_cli(directory, socket_path, "logs 3", out, err);
    YORI_CHECK(code == 0);
    // argv[0] 保持用户输入形式；#! 脚本经内核解释器承接时 $0 显示脚本路径
    // （ELF 直 exec 的 argv[0] 语义由 unit_process_supervisor 锁定）。
    YORI_CHECK(contains(out, "argv0=") && contains(out, "fake-python"));
    YORI_CHECK(contains(out, ("conda=" + fake_root).c_str()));
    YORI_CHECK(contains(out, ("ldp=" + fake_root + "/lib").c_str()));
    YORI_CHECK(contains(out, "job=3"));
    YORI_CHECK(contains(out, "gpu=GPU-e2e-a"));
    YORI_CHECK(contains(out, "token=secret-e2e"));
    // venv 与 conda 同时存在时判定 conda（unit 已覆盖；此处确认 env 元数据
    // 不影响运行时环境的内容）。
    code = run_cli(directory, socket_path, "inspect 3", out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(out, "VIRTUAL_ENV=/old-venv"));
    YORI_CHECK(contains(out, "LD_LIBRARY_PATH=" + fake_root + "/lib"));

    // executable 解析失败：提交即拒（fail-fast，本地用法错误 2）。
    code = run_cli_env(directory, socket_path, "env -i PATH=/nonexistent-dir",
                       "submit -- no-such-cmd", out, err);
    YORI_CHECK(code == 2);
    YORI_CHECK(contains(err, "cannot resolve command"));

    code = run_cli(directory, socket_path, "submit -- /nonexistent/binary", out, err);
    YORI_CHECK(code == 2);
    YORI_CHECK(contains(err, "cannot resolve command"));

    // 显式 --env 保留键：本地拒绝（YORI_* 前缀与 GPU 管理键）。
    code = run_cli(directory, socket_path, "submit --env YORI_JOB_ID=9 -- /bin/true", out, err);
    YORI_CHECK(code == 2);
    YORI_CHECK(contains(err, "reserved key"));
    code = run_cli(directory, socket_path, "submit --env CUDA_VISIBLE_DEVICES=0 -- /bin/true", out,
                   err);
    YORI_CHECK(code == 2);
    YORI_CHECK(contains(err, "reserved key"));
    code =
        run_cli(directory, socket_path, "submit --env LD_PRELOAD=/tmp/x.so -- /bin/true", out, err);
    YORI_CHECK(code == 2);
    YORI_CHECK(contains(err, "reserved key"));

    // --inherit-env 超限显式失败：单值超出 JobSpec 上限（本地捕获拒绝）。
    {
      const std::string big = directory + "/big-var.txt";
      FILE* file = std::fopen(big.c_str(), "w");
      YORI_CHECK(file != nullptr);
      for (int i = 0; i < 33 * 1024; ++i) {
        static_cast<void>(std::fputc('x', file));
      }
      static_cast<void>(std::fclose(file));
      code =
          run_cli_env(directory, socket_path, "env -i PATH=/usr/bin:/bin BIGVAR=$(cat " + big + ")",
                      "submit --inherit-env -- /bin/true", out, err);
      YORI_CHECK(code == 2);
      YORI_CHECK(contains(err, "captured environment rejected"));
      YORI_CHECK(contains(err, "BIGVAR"));
    }

    // --capture-env 扩展键：非白名单变量经扩展捕获并进入训练环境。
    code =
        run_cli_env(directory, socket_path, "env -i PATH=/usr/bin:/bin M8_EXTRA=via-capture",
                    "submit --capture-env M8_EXTRA -- /bin/sh -c 'echo extra=$M8_EXTRA'", out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(out, "Submitted job 4"));
    YORI_CHECK(wait_for_state(4, "FINISHED"));
    code = run_cli(directory, socket_path, "logs 4", out, err);
    YORI_CHECK(code == 0);
    YORI_CHECK(contains(out, "extra=via-capture"));
  }

  // 终态幂等（RULE-04）：重复取消已 CANCELLED 的 Job 成功；不存在显式失败。
  code = run_cli(directory, socket_path, "cancel 1", out, err);
  YORI_CHECK(code == 0);
  YORI_CHECK(contains(out, "Cancelled job 1"));

  code = run_cli(directory, socket_path, "cancel 99", out, err);
  YORI_CHECK(code == 1);
  YORI_CHECK(contains(err, "not found"));

  // 队列收缩到无条目。
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
