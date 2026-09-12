#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <yori/launch/environment_capture.hpp>

extern "C" char** environ;

namespace yori::launch {
namespace {

bool contains_nul(const std::string& value) noexcept {
  return value.find('\0') != std::string::npos;
}

bool is_valid_environment_name(const std::string& name) noexcept {
  if (name.empty() || name.size() > job::JobSpecLimits::kMaxEnvironmentNameBytes ||
      contains_nul(name) || name.find('=') != std::string::npos) {
    return false;
  }
  for (const char c : name) {
    const bool ok =
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// 候选路径的可执行性探测。CLI 以自身（未提权）身份运行，access(2) 即真实判据。
ExecutableResolveErrorCode probe_candidate(const std::string& candidate) noexcept {
  if (::access(candidate.c_str(), X_OK) == 0) {
    return ExecutableResolveErrorCode::kNone;
  }
  if (errno == EACCES) {
    return ExecutableResolveErrorCode::kNotExecutable;
  }
  return ExecutableResolveErrorCode::kNotFound;
}

}  // namespace

const EnvironmentCapturePolicy& EnvironmentCapturePolicy::defaults() noexcept {
  static const EnvironmentCapturePolicy policy;
  return policy;
}

bool EnvironmentCapturePolicy::captures(const std::string& name) const {
  // 保留键（身份/GPU 管理/LD_PRELOAD/YORI_*）任何模式都不捕获。
  if (is_reserved_environment_key(name)) {
    return false;
  }
  if (inherit_all) {
    return true;
  }
  static constexpr std::array<std::string_view, 15> kDefaultKeys{
      "CONDA_DEFAULT_ENV",
      "CONDA_PREFIX",
      "CUDA_HOME",
      "CUDA_PATH",
      "HTTP_PROXY",
      "HTTPS_PROXY",
      "LD_LIBRARY_PATH",
      "MKL_NUM_THREADS",
      "NO_PROXY",
      "OMP_NUM_THREADS",
      "PATH",
      "PYTHONPATH",
      "VIRTUAL_ENV",
      "http_proxy",
      "https_proxy",
  };
  // 注意：no_proxy（小写）显式列入。
  if (name == "no_proxy") {
    return true;
  }
  if (std::find(kDefaultKeys.begin(), kDefaultKeys.end(), name) != kDefaultKeys.end()) {
    return true;
  }
  return std::find(extra_keys.begin(), extra_keys.end(), name) != extra_keys.end();
}

const char* to_string(EnvironmentCaptureErrorCode code) noexcept {
  switch (code) {
    case EnvironmentCaptureErrorCode::kNone:
      return "none";
    case EnvironmentCaptureErrorCode::kInvalidName:
      return "invalid environment name in source";
    case EnvironmentCaptureErrorCode::kTooManyVariables:
      return "captured environment exceeds the variable count limit";
    case EnvironmentCaptureErrorCode::kValueTooLong:
      return "captured environment value exceeds the single value limit";
    case EnvironmentCaptureErrorCode::kTooLarge:
      return "captured environment exceeds the total size limit";
  }
  return "unknown";
}

EnvironmentCaptureResult capture_environment(const std::vector<EnvironmentEntry>& source,
                                             const EnvironmentCapturePolicy& policy) {
  EnvironmentCaptureResult result;
  std::size_t total_bytes = 0;
  for (const auto& [name, value] : source) {
    if (!policy.captures(name)) {
      continue;
    }
    if (!is_valid_environment_name(name)) {
      // 畸形条目不可能成为合法环境变量，跳过（防御外部源）。
      continue;
    }
    if (result.env.size() >= job::JobSpecLimits::kMaxEnvironmentVariables) {
      result.code = EnvironmentCaptureErrorCode::kTooManyVariables;
      result.offending_name = name;
      result.env.clear();
      return result;
    }
    if (value.size() > job::JobSpecLimits::kMaxEnvironmentValueBytes) {
      result.code = EnvironmentCaptureErrorCode::kValueTooLong;
      result.offending_name = name;
      result.env.clear();
      return result;
    }
    const std::size_t entry_bytes = name.size() + value.size();
    if (entry_bytes > job::JobSpecLimits::kMaxEnvironmentBytes ||
        total_bytes > job::JobSpecLimits::kMaxEnvironmentBytes - entry_bytes) {
      result.code = EnvironmentCaptureErrorCode::kTooLarge;
      result.offending_name = name;
      result.env.clear();
      return result;
    }
    total_bytes += entry_bytes;
    result.env.emplace(name, value);
  }
  return result;
}

const char* to_string(ExecutableResolveErrorCode code) noexcept {
  switch (code) {
    case ExecutableResolveErrorCode::kNone:
      return "none";
    case ExecutableResolveErrorCode::kEmptyName:
      return "empty command name";
    case ExecutableResolveErrorCode::kInvalidName:
      return "invalid command name";
    case ExecutableResolveErrorCode::kRelativeWithoutCwd:
      return "relative command requires a working directory";
    case ExecutableResolveErrorCode::kNotFound:
      return "executable not found";
    case ExecutableResolveErrorCode::kNotExecutable:
      return "target is not executable";
  }
  return "unknown";
}

ExecutableResolveResult resolve_executable(const std::string& argv0, const std::string& cwd,
                                           const std::map<std::string, std::string>& captured_env) {
  if (argv0.empty()) {
    return {ExecutableResolveErrorCode::kEmptyName, {}, "argv[0] is empty"};
  }
  if (contains_nul(argv0) || argv0.size() > job::JobSpecLimits::kMaxExecutableBytes) {
    return {ExecutableResolveErrorCode::kInvalidName, {}, "argv[0] is invalid"};
  }

  if (argv0.find('/') != std::string::npos) {
    if (argv0.front() == '/') {
      const auto probed = probe_candidate(argv0);
      return {probed, probed == ExecutableResolveErrorCode::kNone ? argv0 : std::string{},
              probed == ExecutableResolveErrorCode::kNone ? std::string{} : to_string(probed)};
    }
    if (cwd.empty() || cwd.front() != '/') {
      return {ExecutableResolveErrorCode::kRelativeWithoutCwd, {}, "cwd must be absolute"};
    }
    std::string joined = cwd;
    if (joined.back() != '/') {
      joined.push_back('/');
    }
    joined += argv0;
    const auto probed = probe_candidate(joined);
    return {probed, probed == ExecutableResolveErrorCode::kNone ? joined : std::string{},
            probed == ExecutableResolveErrorCode::kNone ? std::string{} : to_string(probed)};
  }

  // 裸名：按捕获后的 PATH 逐段解析。
  const auto path_entry = captured_env.find("PATH");
  const std::string path = path_entry != captured_env.end() && !path_entry->second.empty()
                               ? path_entry->second
                               : "/usr/bin:/bin";
  std::size_t begin = 0;
  while (begin <= path.size()) {
    std::size_t end = path.find(':', begin);
    if (end == std::string::npos) {
      end = path.size();
    }
    const std::size_t length = end - begin;
    if (length > 0) {
      std::string candidate = path.substr(begin, length);
      if (candidate.back() != '/') {
        candidate.push_back('/');
      }
      candidate += argv0;
      if (probe_candidate(candidate) == ExecutableResolveErrorCode::kNone) {
        return {ExecutableResolveErrorCode::kNone, candidate, {}};
      }
    }
    if (end == path.size()) {
      break;
    }
    begin = end + 1;
  }
  return {ExecutableResolveErrorCode::kNotFound, {}, "command not found in captured PATH"};
}

job::EnvSource detect_environment_source(const std::map<std::string, std::string>& captured_env) {
  if (captured_env.contains("CONDA_PREFIX")) {
    return job::EnvSource::kConda;
  }
  if (captured_env.contains("VIRTUAL_ENV")) {
    return job::EnvSource::kVenv;
  }
  return job::EnvSource::kNone;
}

namespace {

// 解析 "Python 3.11.5" 形式的版本串（python2 写 stderr、python3 写 stdout，
// 两路合并后取首个 "Python x.y" 前缀）。
std::optional<std::string> parse_python_version(const std::string& output) {
  static constexpr std::string_view kPrefix = "Python ";
  const auto begin = output.find(kPrefix);
  if (begin == std::string::npos) {
    return std::nullopt;
  }
  std::size_t cursor = begin + kPrefix.size();
  std::size_t digits = 0;
  bool last_was_digit_or_dot = false;
  while (cursor < output.size()) {
    const char c = output[cursor];
    const bool digit_or_dot = (c >= '0' && c <= '9') || c == '.';
    if (!digit_or_dot) {
      break;
    }
    if (c >= '0' && c <= '9') {
      ++digits;
    }
    last_was_digit_or_dot = true;
    ++cursor;
  }
  if (!last_was_digit_or_dot || digits < 2) {
    return std::nullopt;
  }
  return output.substr(begin + kPrefix.size(), cursor - begin - kPrefix.size());
}

bool basename_contains_python(const std::string& executable) {
  const std::size_t slash = executable.find_last_of('/');
  const std::string base = slash == std::string::npos ? executable : executable.substr(slash + 1);
  return base.find("python") != std::string::npos;
}

}  // namespace

std::optional<std::string> probe_python_version(const std::string& executable) {
  if (executable.empty() || !basename_contains_python(executable)) {
    return std::nullopt;
  }

  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    return std::nullopt;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(pipe_fds[0]));
    static_cast<void>(::close(pipe_fds[1]));
    return std::nullopt;
  }
  if (child == 0) {
    static_cast<void>(::close(pipe_fds[0]));
    static_cast<void>(::dup2(pipe_fds[1], STDOUT_FILENO));
    static_cast<void>(::dup2(pipe_fds[1], STDERR_FILENO));
    static_cast<void>(::close(pipe_fds[1]));
    std::array<char*, 3> argv{const_cast<char*>(executable.c_str()), const_cast<char*>("--version"),
                              nullptr};
    std::array<char*, 2> envp{nullptr};
    ::execve(executable.c_str(), argv.data(), envp.data());
    ::_exit(127);
  }

  static_cast<void>(::close(pipe_fds[1]));
  std::string output;
  char buffer[512];
  // 有界读取 + 有界等待（3s 墙钟）：卡死/长驻的解释器不得阻塞提交。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (std::chrono::steady_clock::now() < deadline) {
    const ssize_t n = ::read(pipe_fds[0], buffer, sizeof(buffer));
    if (n > 0) {
      output.append(buffer, static_cast<std::size_t>(n));
      if (output.size() > 4096) {
        break;
      }
      continue;
    }
    if (n == 0) {
      break;
    }
    if (errno != EINTR) {
      break;
    }
  }
  static_cast<void>(::close(pipe_fds[0]));

  int status = 0;
  bool exited = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t reaped = ::waitpid(child, &status, WNOHANG);
    if (reaped == child) {
      exited = true;
      break;
    }
    if (reaped < 0 && errno != EINTR) {
      return std::nullopt;
    }
    struct timespec pause {
      0, 2L * 1000L * 1000L
    };
    ::nanosleep(&pause, nullptr);
  }
  if (!exited) {
    static_cast<void>(::kill(child, SIGKILL));
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return std::nullopt;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }
  return parse_python_version(output);
}

}  // namespace yori::launch
