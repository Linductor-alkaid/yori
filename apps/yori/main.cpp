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

// 退出码契约（M5 计划）：0 成功；1 请求失败（daemon 显式错误）；2 用法错误；
// 3 传输失败（连接/超时/协议）。
constexpr int kExitOk = 0;
constexpr int kExitRequestFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitTransport = 3;

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
               "  logs [--bytes N] <job-id>   print current log tail (follow lands in M6)\n"
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

int command_logs(const std::string& socket_path, std::vector<std::string> arguments) {
  std::uint32_t max_bytes = kDefaultLogBytes;
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

  const std::vector<std::string>& rest = parser.remaining();
  if (rest.empty()) {
    std::fprintf(stderr, "yori: logs requires a job id\n");
    return kExitUsage;
  }
  if (rest.front() == "-f" || rest.front() == "--follow") {
    // M6：流式跟随（offset 续传、GAP/EOF/BACKPRESSURE）。
    std::fprintf(stderr, "yori: logs --follow lands in M6; use snapshot form for now\n");
    return kExitUsage;
  }
  std::uint64_t job_id = 0;
  if (!parse_u64(rest.front().c_str(), job_id) || job_id == 0) {
    std::fprintf(stderr, "yori: logs expects a numeric job id\n");
    return kExitUsage;
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

  std::fprintf(stderr, "yori: unknown command %s\n", command.c_str());
  print_usage();
  return kExitUsage;
}
