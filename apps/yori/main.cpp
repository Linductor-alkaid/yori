#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yori/version.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_protocol.hpp>
#include <yori/ipc/ipc_transport.hpp>
#include <yori/ipc/uds_client.hpp>
#include <yori/job/job.hpp>

namespace {

constexpr const char* kDefaultSocketPath = "/run/yori/yori.sock";
// CLI 侧对 daemon 端请求的总预算；连接失败/慢 daemon 时有界退出。
constexpr int kCallTimeoutMs = 10000;
// logs 快照默认每流字节数（daemon 上限内）。
constexpr std::uint32_t kDefaultLogBytes = 64 * 1024;
// logs -f 初始握手（连接 + 请求 + ack）预算；流式阶段无限等待（安静流）。
constexpr int kFollowSetupTimeoutMs = 10000;

// 退出码契约（M5 计划；M6 扩展 4）：0 成功；1 请求失败（daemon 显式错误）；
// 2 用法错误；3 传输失败（连接/超时/协议）；4 logs -f 以 BACKPRESSURE 结束
// （可按提示以 --since-* 续传重连）。
constexpr int kExitOk = 0;
constexpr int kExitRequestFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitTransport = 3;
constexpr int kExitBackpressure = 4;

const char* job_state_name(std::uint8_t state) {
  switch (static_cast<yori::job::JobState>(state)) {
    case yori::job::JobState::kQueued:
      return "QUEUED";
    case yori::job::JobState::kStarting:
      return "STARTING";
    case yori::job::JobState::kRunning:
      return "RUNNING";
    case yori::job::JobState::kStopping:
      return "STOPPING";
    case yori::job::JobState::kFinished:
      return "FINISHED";
    case yori::job::JobState::kFailed:
      return "FAILED";
    case yori::job::JobState::kCancelled:
      return "CANCELLED";
    case yori::job::JobState::kLost:
      return "LOST";
  }
  return "UNKNOWN";
}

const char* gpu_logical_name(std::uint8_t state) {
  switch (static_cast<yori::gpu::GpuLogicalState>(state)) {
    case yori::gpu::GpuLogicalState::kFree:
      return "FREE";
    case yori::gpu::GpuLogicalState::kAllocated:
      return "ALLOCATED";
    case yori::gpu::GpuLogicalState::kExternalBusy:
      return "EXTERNAL_BUSY";
    case yori::gpu::GpuLogicalState::kUnavailable:
      return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

const char* gpu_observed_name(std::uint8_t state) {
  switch (static_cast<yori::gpu::GpuObservedState>(state)) {
    case yori::gpu::GpuObservedState::kFree:
      return "FREE";
    case yori::gpu::GpuObservedState::kExternalBusy:
      return "EXTERNAL_BUSY";
    case yori::gpu::GpuObservedState::kUnavailable:
      return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

void print_usage() {
  std::fprintf(stderr,
               "usage: yori [--socket PATH] <command> [args]\n"
               "commands:\n"
               "  submit [--gpus N] [--tensorboard-logdir DIR] [--cwd DIR]\n"
               "         [--env K=V]... -- CMD [ARG]...\n"
               "  ps                          list jobs (own jobs in full, others masked)\n"
               "  queue                       list queued jobs in FIFO order\n"
               "  gpu                         list GPUs with logical state\n"
               "  cancel <job-id>             cancel a queued job\n"
               "  logs [--bytes N] <job-id>   print current log tail\n"
               "  logs -f [--since-stdout N] [--since-stderr N] <job-id>\n"
               "                              follow logs (GAP/EOF/BACKPRESSURE aware)\n"
               "  tensorboard [--logdir DIR] [--port N] [--host H] <job-id>\n"
               "                              run TensorBoard for a job in this session\n"
               "options:\n"
               "  --socket PATH               daemon endpoint (default %s or $YORI_SOCKET)\n",
               kDefaultSocketPath);
}

bool parse_u64(const char* text, std::uint64_t& out) {
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

class CommandLine final {
 public:
  explicit CommandLine(std::vector<std::string> arguments) : arguments_(std::move(arguments)) {}

  bool take_flag(const std::string& name, std::string& value) {
    for (std::size_t i = 0; i + 1 < arguments_.size(); ++i) {
      if (arguments_[i] == name) {
        value = arguments_[i + 1];
        arguments_.erase(arguments_.begin() + static_cast<std::ptrdiff_t>(i),
                         arguments_.begin() + static_cast<std::ptrdiff_t>(i + 2));
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] const std::vector<std::string>& remaining() const noexcept { return arguments_; }

 private:
  std::vector<std::string> arguments_;
};

// 一次请求/响应交互。请求级失败打印 daemon 的错误并返回 kExitRequestFailed；
// 传输失败返回 kExitTransport。成功时 response 交给调用方。
int call_daemon(const std::string& socket_path, const yori::ipc::IpcRequest& request,
                yori::ipc::IpcResponse& response) {
  yori::ipc::UdsIpcClient client;
  const yori::ipc::IpcClientResult result =
      client.call(socket_path, request, std::chrono::milliseconds{kCallTimeoutMs});
  if (!result.ok()) {
    std::fprintf(stderr, "yori: cannot reach daemon at %s: %s\n", socket_path.c_str(),
                 yori::ipc::to_string(result.error));
    return kExitTransport;
  }
  if (result.response.error != yori::ipc::IpcError::kNone) {
    response = result.response;
    return kExitRequestFailed;
  }
  response = result.response;
  return kExitOk;
}

int fail_request(const std::string& command, const yori::ipc::IpcResponse& response) {
  std::fprintf(stderr, "yori: %s failed: %s%s%s\n", command.c_str(),
               yori::ipc::to_string(response.error), response.detail.empty() ? "" : ": ",
               response.detail.c_str());
  return kExitRequestFailed;
}

int command_submit(const std::string& socket_path, std::vector<std::string> arguments) {
  yori::ipc::IpcSubmitRequest submit;
  std::uint32_t gpus = 1;

  CommandLine parser(std::move(arguments));
  std::string value;
  while (parser.take_flag("--gpus", value)) {
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
      std::fprintf(stderr, "yori: --gpus expects a number\n");
      return kExitUsage;
    }
    gpus = static_cast<std::uint32_t>(parsed);
  }
  if (gpus != 1) {
    // MVP 单 GPU Job；多 GPU 为 POST-01（设计 16.2）。
    std::fprintf(stderr, "yori: only 1 GPU per job is supported in MVP\n");
    return kExitUsage;
  }
  while (parser.take_flag("--tensorboard-logdir", value)) {
    submit.tensorboard_logdir = value;
  }
  while (parser.take_flag("--cwd", value)) {
    submit.cwd = value;
  }
  std::map<std::string, std::string> env;
  while (parser.take_flag("--env", value)) {
    const std::size_t separator = value.find('=');
    if (separator == std::string::npos || separator == 0) {
      std::fprintf(stderr, "yori: --env expects K=V\n");
      return kExitUsage;
    }
    env.emplace(value.substr(0, separator), value.substr(separator + 1));
  }

  const std::vector<std::string>& rest = parser.remaining();
  if (rest.empty() || rest.front() != "--") {
    std::fprintf(stderr, "yori: submit requires '--' before the command\n");
    return kExitUsage;
  }
  submit.argv.assign(rest.begin() + 1, rest.end());
  if (submit.argv.empty()) {
    std::fprintf(stderr, "yori: submit requires a command after '--'\n");
    return kExitUsage;
  }
  if (submit.cwd.empty()) {
    std::array<char, 4096> cwd{};
    if (::getcwd(cwd.data(), cwd.size()) == nullptr) {
      std::fprintf(stderr, "yori: cannot determine working directory\n");
      return kExitUsage;
    }
    submit.cwd = cwd.data();
  }
  submit.env = std::move(env);
  submit.gpu_request = 1;

  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kSubmit;
  request.submit = std::move(submit);

  yori::ipc::IpcResponse response;
  const int status = call_daemon(socket_path, request, response);
  if (status != kExitOk) {
    if (status == kExitRequestFailed) {
      return fail_request("submit", response);
    }
    return status;
  }
  std::printf("Submitted job %llu\n", static_cast<unsigned long long>(response.job_id));
  return kExitOk;
}

void print_job_row(std::uint64_t job_id, const char* state, std::uint32_t owner,
                   const std::string& command) {
  std::printf("%-10llu %-10s %-8u %s\n", static_cast<unsigned long long>(job_id), state, owner,
              command.c_str());
}

int command_ps(const std::string& socket_path) {
  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kPs;

  yori::ipc::IpcResponse response;
  const int status = call_daemon(socket_path, request, response);
  if (status != kExitOk) {
    if (status == kExitRequestFailed) {
      return fail_request("ps", response);
    }
    return status;
  }
  if (response.error == yori::ipc::IpcError::kLimit) {
    std::fprintf(stderr, "yori: %s\n", response.detail.c_str());
  }
  std::printf("%-10s %-10s %-8s %s\n", "JOB", "STATE", "OWNER", "COMMAND");
  for (const auto& summary : response.jobs) {
    std::string command;
    if (summary.masked) {
      command = "(masked)";
    } else {
      for (std::size_t i = 0; i < summary.argv.size(); ++i) {
        if (i > 0) {
          command += " ";
        }
        command += summary.argv[i];
      }
    }
    print_job_row(summary.job_id, job_state_name(summary.state), summary.owner_uid, command);
  }
  return kExitOk;
}

int command_queue(const std::string& socket_path) {
  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kQueue;

  yori::ipc::IpcResponse response;
  const int status = call_daemon(socket_path, request, response);
  if (status != kExitOk) {
    if (status == kExitRequestFailed) {
      return fail_request("queue", response);
    }
    return status;
  }
  if (response.error == yori::ipc::IpcError::kLimit) {
    std::fprintf(stderr, "yori: %s\n", response.detail.c_str());
  }
  std::printf("%-8s %-10s %-8s %-20s %s\n", "POSITION", "JOB", "OWNER", "SUBMITTED", "STATE");
  std::size_t position = 1;
  for (const auto& entry : response.queue) {
    const auto seconds = static_cast<std::time_t>(entry.submit_time_unix_ns / 1000000000);
    std::array<char, 32> formatted{};
    const auto* broken = std::localtime(&seconds);
    if (broken != nullptr) {
      static_cast<void>(
          std::strftime(formatted.data(), formatted.size(), "%Y-%m-%d %H:%M:%S", broken));
    }
    std::printf("%-8zu %-10llu %-8u %-20s %s\n", position,
                static_cast<unsigned long long>(entry.job_id), entry.owner_uid, formatted.data(),
                job_state_name(entry.state));
    ++position;
  }
  return kExitOk;
}

int command_gpu(const std::string& socket_path) {
  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kGpu;

  yori::ipc::IpcResponse response;
  const int status = call_daemon(socket_path, request, response);
  if (status != kExitOk) {
    if (status == kExitRequestFailed) {
      return fail_request("gpu", response);
    }
    return status;
  }
  std::printf("%-6s %-10s %-14s %-14s %-7s %-16s %s\n", "INDEX", "UUID", "LOGICAL", "OBSERVED",
              "UTIL%", "MEMORY", "LEASE");
  for (const auto& device : response.devices) {
    char util[16];
    if (device.utilization_percent) {
      static_cast<void>(std::snprintf(util, sizeof(util), "%u", *device.utilization_percent));
    } else {
      static_cast<void>(std::snprintf(util, sizeof(util), "-"));
    }
    char memory[64];
    if (device.memory_used_bytes && device.memory_total_bytes) {
      static_cast<void>(
          std::snprintf(memory, sizeof(memory), "%llu/%llu MiB",
                        static_cast<unsigned long long>(*device.memory_used_bytes) >> 20,
                        static_cast<unsigned long long>(*device.memory_total_bytes) >> 20));
    } else {
      static_cast<void>(std::snprintf(memory, sizeof(memory), "-"));
    }
    char lease[24];
    if (device.leased_by_job) {
      static_cast<void>(std::snprintf(lease, sizeof(lease), "job %llu",
                                      static_cast<unsigned long long>(*device.leased_by_job)));
    } else {
      static_cast<void>(std::snprintf(lease, sizeof(lease), "-"));
    }
    std::printf("%-6u %-10s %-14s %-14s %-7s %-16s %s\n", device.index, device.uuid.c_str(),
                gpu_logical_name(device.logical_state), gpu_observed_name(device.observed_state),
                util, memory, lease);
  }
  return kExitOk;
}

int command_cancel(const std::string& socket_path, const std::string& job_text) {
  std::uint64_t job_id = 0;
  if (!parse_u64(job_text.c_str(), job_id) || job_id == 0) {
    std::fprintf(stderr, "yori: cancel expects a numeric job id\n");
    return kExitUsage;
  }
  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kCancel;
  request.cancel.job_id = job_id;

  yori::ipc::IpcResponse response;
  const int status = call_daemon(socket_path, request, response);
  if (status == kExitOk) {
    std::printf("Cancelled job %llu\n", static_cast<unsigned long long>(job_id));
    return kExitOk;
  }
  if (status == kExitRequestFailed) {
    // 终态幂等：已取消的重复 cancel 对用户是成功。
    if (response.error == yori::ipc::IpcError::kInvalidState &&
        static_cast<yori::job::JobState>(response.state) == yori::job::JobState::kCancelled) {
      std::printf("Job %llu already cancelled\n", static_cast<unsigned long long>(job_id));
      return kExitOk;
    }
    return fail_request("cancel", response);
  }
  return status;
}

// logs -f 的流式帧呈现：数据按流写出、GAP 提示、BACKPRESSURE 记录并停止、
// EOF 记录终态。
class FollowPrinter final : public yori::ipc::IpcStreamFrameHandler {
 public:
  bool on_frame(const yori::ipc::IpcStreamFrame& frame) override {
    switch (frame.kind) {
      case yori::ipc::IpcStreamFrameKind::kLogData: {
        std::FILE* target = frame.stream == 0 ? stdout : stderr;
        if (!frame.data.empty()) {
          static_cast<void>(std::fwrite(frame.data.data(), 1, frame.data.size(), target));
          static_cast<void>(std::fflush(target));
        }
        return true;
      }
      case yori::ipc::IpcStreamFrameKind::kLogGap:
        std::fprintf(stderr, "yori: log gap: bytes [%llu, %llu) on %s not retained; resumed\n",
                     static_cast<unsigned long long>(frame.begin_offset),
                     static_cast<unsigned long long>(frame.end_offset),
                     frame.stream == 0 ? "stdout" : "stderr");
        return true;
      case yori::ipc::IpcStreamFrameKind::kLogBackpressure:
        std::fprintf(stderr,
                     "yori: following too far behind on %s at offset %llu; disconnected\n"
                     "yori: reconnect with: yori logs -f --since-%s %llu\n",
                     frame.stream == 0 ? "stdout" : "stderr",
                     static_cast<unsigned long long>(frame.begin_offset),
                     frame.stream == 0 ? "stdout" : "stderr",
                     static_cast<unsigned long long>(frame.begin_offset));
        backpressure_ = true;
        return true;
      case yori::ipc::IpcStreamFrameKind::kLogEof:
        eof_state_ = frame.job_state;
        return true;
    }
    return true;
  }

  [[nodiscard]] bool backpressure() const noexcept { return backpressure_; }
  [[nodiscard]] bool eof_seen() const noexcept { return eof_state_ >= 0; }
  [[nodiscard]] std::uint8_t eof_state() const noexcept {
    return static_cast<std::uint8_t>(eof_state_);
  }

 private:
  bool backpressure_{false};
  int eof_state_{-1};
};

int command_logs_follow(const std::string& socket_path, std::uint64_t job_id,
                        std::optional<std::uint64_t> since_stdout,
                        std::optional<std::uint64_t> since_stderr) {
  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kLogsFollow;
  request.logs_follow.job_id = job_id;
  request.logs_follow.since_stdout = since_stdout;
  request.logs_follow.since_stderr = since_stderr;

  FollowPrinter printer;
  yori::ipc::UdsIpcClient client;
  const yori::ipc::IpcFollowResult result =
      client.follow(socket_path, request, std::chrono::milliseconds{kFollowSetupTimeoutMs},
                    std::chrono::milliseconds::zero(), printer);
  if (!result.ok()) {
    std::fprintf(stderr, "yori: logs -f transport failed: %s\n",
                 yori::ipc::to_string(result.error));
    return kExitTransport;
  }
  if (result.ack.error != yori::ipc::IpcError::kNone) {
    std::fprintf(stderr, "yori: logs -f failed: %s%s%s\n", yori::ipc::to_string(result.ack.error),
                 result.ack.detail.empty() ? "" : ": ", result.ack.detail.c_str());
    return kExitRequestFailed;
  }
  if (printer.backpressure()) {
    return kExitBackpressure;
  }
  if (printer.eof_seen()) {
    // 退出码与 Job 终态对齐（设计 11.3）：FINISHED -> 0，其余终态 -> 1。
    const auto state = static_cast<yori::job::JobState>(printer.eof_state());
    if (state == yori::job::JobState::kFinished) {
      return kExitOk;
    }
    std::fprintf(stderr, "yori: job ended in state %s\n", job_state_name(printer.eof_state()));
    return kExitRequestFailed;
  }
  return kExitOk;  // 客户端侧无终止帧的干净结束（当前路径不出现，保守成功）。
}

int command_logs(const std::string& socket_path, std::vector<std::string> arguments) {
  std::uint32_t max_bytes = kDefaultLogBytes;
  bool follow = false;
  std::optional<std::uint64_t> since_stdout;
  std::optional<std::uint64_t> since_stderr;
  std::optional<std::uint64_t> since_both;

  CommandLine parser(std::move(arguments));
  std::string value;
  while (parser.take_flag("--bytes", value)) {
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsed == 0) {
      std::fprintf(stderr, "yori: --bytes expects a positive number\n");
      return kExitUsage;
    }
    max_bytes = static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 0xffffffffUL));
  }
  const auto parse_offset = [](const std::string& flag, const std::string& text,
                               std::optional<std::uint64_t>& out) {
    std::uint64_t parsed = 0;
    if (!parse_u64(text.c_str(), parsed)) {
      std::fprintf(stderr, "yori: %s expects a non-negative number\n", flag.c_str());
      return false;
    }
    out = parsed;
    return true;
  };
  while (parser.take_flag("--since-offset", value)) {
    if (!parse_offset("--since-offset", value, since_both)) {
      return kExitUsage;
    }
  }
  while (parser.take_flag("--since-stdout", value)) {
    if (!parse_offset("--since-stdout", value, since_stdout)) {
      return kExitUsage;
    }
  }
  while (parser.take_flag("--since-stderr", value)) {
    if (!parse_offset("--since-stderr", value, since_stderr)) {
      return kExitUsage;
    }
  }
  if (since_both) {
    if (!since_stdout) {
      since_stdout = since_both;
    }
    if (!since_stderr) {
      since_stderr = since_both;
    }
  }

  std::vector<std::string> rest = parser.remaining();
  if (!rest.empty() && (rest.front() == "-f" || rest.front() == "--follow")) {
    follow = true;
    rest.erase(rest.begin());
  }
  if (rest.empty()) {
    std::fprintf(stderr, "yori: logs requires a job id\n");
    return kExitUsage;
  }
  std::uint64_t job_id = 0;
  if (!parse_u64(rest.front().c_str(), job_id) || job_id == 0) {
    std::fprintf(stderr, "yori: logs expects a numeric job id\n");
    return kExitUsage;
  }
  if (follow) {
    return command_logs_follow(socket_path, job_id, since_stdout, since_stderr);
  }

  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kLogs;
  request.logs.job_id = job_id;
  request.logs.max_bytes = max_bytes;

  yori::ipc::IpcResponse response;
  const int status = call_daemon(socket_path, request, response);
  if (status != kExitOk) {
    if (status == kExitRequestFailed) {
      return fail_request("logs", response);
    }
    return status;
  }
  if (response.logs.stdout_truncated) {
    std::fprintf(stderr, "yori: stdout tail truncated\n");
  }
  if (response.logs.stderr_truncated) {
    std::fprintf(stderr, "yori: stderr tail truncated\n");
  }
  static_cast<void>(
      std::fwrite(response.logs.stdout_tail.data(), 1, response.logs.stdout_tail.size(), stdout));
  static_cast<void>(std::fflush(stdout));
  static_cast<void>(
      std::fwrite(response.logs.stderr_tail.data(), 1, response.logs.stderr_tail.size(), stderr));
  static_cast<void>(std::fflush(stderr));
  return kExitOk;
}

// ---------------------------------------------------------------------------
// yori tensorboard（DEC-003）：CLI 用户会话拉起，前台运行，CLI 退出即终止。
// ---------------------------------------------------------------------------

// 信号转发：终端 Ctrl-C 同时到达同进程组的子进程；仅发往 CLI 的信号经
// handler 转发，保证 TensorBoard 不成孤儿。
volatile sig_atomic_t tensorboard_pid = 0;

void forward_tensorboard_signal(int signal_number) {
  const int child = static_cast<int>(tensorboard_pid);
  if (child > 0) {
    static_cast<void>(kill(static_cast<pid_t>(child), signal_number));
  }
}

std::string resolve_logdir(const std::string& candidate, const std::string& job_cwd) {
  if (!candidate.empty() && candidate.front() == '/') {
    return candidate;
  }
  if (job_cwd.empty()) {
    return candidate;
  }
  if (job_cwd.back() == '/') {
    return job_cwd + candidate;
  }
  return job_cwd + "/" + candidate;
}

int command_tensorboard(const std::string& socket_path, std::vector<std::string> arguments) {
  std::optional<std::string> logdir_argument;
  std::uint64_t port = 0;  // 默认 0：OS 分配（DEC-003），TensorBoard 自行打印 URL。
  std::string host = "127.0.0.1";

  CommandLine parser(std::move(arguments));
  std::string value;
  while (parser.take_flag("--logdir", value)) {
    logdir_argument = value;
  }
  while (parser.take_flag("--port", value)) {
    std::uint64_t parsed = 0;
    if (!parse_u64(value.c_str(), parsed) || parsed > 0xffff) {
      std::fprintf(stderr, "yori: --port expects a port number\n");
      return kExitUsage;
    }
    port = parsed;
  }
  while (parser.take_flag("--host", value)) {
    host = value;
  }
  if (host.empty()) {
    std::fprintf(stderr, "yori: --host must not be empty\n");
    return kExitUsage;
  }

  const std::vector<std::string>& rest = parser.remaining();
  if (rest.size() != 1) {
    std::fprintf(stderr, "yori: tensorboard requires exactly one job id\n");
    return kExitUsage;
  }
  std::uint64_t job_id = 0;
  if (!parse_u64(rest.front().c_str(), job_id) || job_id == 0) {
    std::fprintf(stderr, "yori: tensorboard expects a numeric job id\n");
    return kExitUsage;
  }

  // logdir 解析原料来自 daemon 只读查询（owner/admin）；优先级判定在 CLI：
  // --logdir 参数 > spec.tensorboard_logdir > Job cwd（DEC-003）。
  yori::ipc::IpcRequest request;
  request.kind = yori::ipc::IpcRequestKind::kTensorboard;
  request.tensorboard.job_id = job_id;
  yori::ipc::IpcResponse response;
  const int status = call_daemon(socket_path, request, response);
  if (status != kExitOk) {
    if (status == kExitRequestFailed) {
      return fail_request("tensorboard", response);
    }
    return status;
  }
  std::string logdir;
  if (logdir_argument) {
    logdir = resolve_logdir(*logdir_argument, response.tensorboard.cwd);
  } else if (response.tensorboard.logdir) {
    logdir = resolve_logdir(*response.tensorboard.logdir, response.tensorboard.cwd);
  } else {
    logdir = response.tensorboard.cwd;
  }
  if (logdir.empty()) {
    std::fprintf(stderr, "yori: tensorboard: no logdir could be resolved\n");
    return kExitRequestFailed;
  }

  std::vector<std::string> child_argv = {"tensorboard",        "--logdir", logdir, "--port",
                                         std::to_string(port), "--host",   host};

  // 前台 spawn：继承 stdio（TensorBoard 打印自己的 URL），同进程组（终端
  // 信号直达），仅 CLI 的信号经 handler 转发。
  struct sigaction forward_action {};
  forward_action.sa_handler = forward_tensorboard_signal;
  static_cast<void>(sigemptyset(&forward_action.sa_mask));
  forward_action.sa_flags = 0;
  const bool forward_installed = ::sigaction(SIGINT, &forward_action, nullptr) == 0 &&
                                 ::sigaction(SIGTERM, &forward_action, nullptr) == 0;

  std::fflush(nullptr);
  const pid_t child = ::fork();
  if (child < 0) {
    std::fprintf(stderr, "yori: tensorboard: fork failed: %s\n", std::strerror(errno));
    return kExitRequestFailed;
  }
  if (child == 0) {
    std::vector<char*> argv_buffer;
    argv_buffer.reserve(child_argv.size() + 1);
    for (const std::string& argument : child_argv) {
      argv_buffer.push_back(const_cast<char*>(argument.c_str()));
    }
    argv_buffer.push_back(nullptr);
    ::execvp(argv_buffer[0], argv_buffer.data());
    std::fprintf(stderr, "yori: tensorboard: exec failed: %s\n", std::strerror(errno));
    ::_exit(127);
  }

  tensorboard_pid = static_cast<int>(child);
  if (port != 0) {
    std::printf("TensorBoard running at http://%s:%llu/ (job %llu)\n", host.c_str(),
                static_cast<unsigned long long>(port), static_cast<unsigned long long>(job_id));
  } else {
    std::printf(
        "TensorBoard starting for job %llu (port auto-assigned; URL is printed by "
        "TensorBoard itself)\n",
        static_cast<unsigned long long>(job_id));
  }
  std::fflush(stdout);

  int child_status = 0;
  while (::waitpid(child, &child_status, 0) < 0) {
    if (errno != EINTR) {
      tensorboard_pid = 0;
      std::fprintf(stderr, "yori: tensorboard: wait failed: %s\n", std::strerror(errno));
      return kExitRequestFailed;
    }
  }
  tensorboard_pid = 0;
  if (forward_installed) {
    struct sigaction restore_default {};
    restore_default.sa_handler = SIG_DFL;
    static_cast<void>(sigemptyset(&restore_default.sa_mask));
    static_cast<void>(::sigaction(SIGINT, &restore_default, nullptr));
    static_cast<void>(::sigaction(SIGTERM, &restore_default, nullptr));
  }

  if (WIFEXITED(child_status)) {
    return WEXITSTATUS(child_status) == 0 ? kExitOk : kExitRequestFailed;
  }
  if (WIFSIGNALED(child_status)) {
    std::fprintf(stderr, "yori: tensorboard terminated by signal %d\n", WTERMSIG(child_status));
    return kExitRequestFailed;
  }
  return kExitRequestFailed;
}

}  // namespace

// yori CLI：无状态客户端（设计第 3 节），不创建 Executor；身份与授权全部由
// daemon 基于 SO_PEERCRED 判定（DEC-010）。
int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("yori %s\n", yori::version());
    return kExitOk;
  }
  if (argc < 2) {
    print_usage();
    return kExitUsage;
  }

  // 端点解析顺序：--socket > $YORI_SOCKET > 默认路径。
  std::string socket_path = kDefaultSocketPath;
  if (const char* environment = ::getenv("YORI_SOCKET");
      environment != nullptr && environment[0] != '\0') {
    socket_path = environment;
  }

  std::vector<std::string> positional;
  bool command_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (!command_seen && argument == "--socket") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "yori: --socket requires a path\n");
        return kExitUsage;
      }
      socket_path = argv[++i];
      continue;
    }
    command_seen = true;
    positional.emplace_back(argument);
  }
  if (positional.empty()) {
    print_usage();
    return kExitUsage;
  }

  const std::string command = positional.front();
  std::vector<std::string> command_args(positional.begin() + 1, positional.end());
  if (command == "--help" || command == "-h" || command == "help") {
    print_usage();
    return kExitOk;
  }
  if (command == "submit") {
    return command_submit(socket_path, std::move(command_args));
  }
  if (command == "ps") {
    if (!command_args.empty()) {
      std::fprintf(stderr, "yori: ps takes no arguments\n");
      return kExitUsage;
    }
    return command_ps(socket_path);
  }
  if (command == "queue") {
    if (!command_args.empty()) {
      std::fprintf(stderr, "yori: queue takes no arguments\n");
      return kExitUsage;
    }
    return command_queue(socket_path);
  }
  if (command == "gpu") {
    if (!command_args.empty()) {
      std::fprintf(stderr, "yori: gpu takes no arguments\n");
      return kExitUsage;
    }
    return command_gpu(socket_path);
  }
  if (command == "cancel") {
    if (command_args.size() != 1) {
      std::fprintf(stderr, "yori: cancel requires exactly one job id\n");
      return kExitUsage;
    }
    return command_cancel(socket_path, command_args.front());
  }
  if (command == "logs") {
    return command_logs(socket_path, std::move(command_args));
  }
  if (command == "tensorboard") {
    return command_tensorboard(socket_path, std::move(command_args));
  }

  std::fprintf(stderr, "yori: unknown command %s\n", command.c_str());
  print_usage();
  return kExitUsage;
}
