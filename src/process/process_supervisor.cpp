#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <yori/process/process_supervisor.hpp>

namespace yori::process {
namespace {

// 子进程错误报告协议：fork 后子进程在 exec 失败路径上经该 fd 写入两个字节
// （阶段 + errno），成功 exec 时由 FD_CLOEXEC 自动关闭，父进程读到 EOF。
constexpr int kExecReportFd = 3;
constexpr std::chrono::milliseconds kExecConfirmationTimeout{10000};

enum class ChildStage : unsigned char {
  kProcessGroup = 1,
  kDemotion = 2,
  kChdir = 3,
  kExec = 4,
};

[[noreturn]] void write_child_report(ChildStage stage, int error_number) noexcept {
  const std::array<unsigned char, 2> report{static_cast<unsigned char>(stage),
                                            static_cast<unsigned char>(error_number & 0xFF)};
  std::size_t written = 0;
  while (written < report.size()) {
    const ssize_t n = ::write(kExecReportFd, report.data() + written, report.size() - written);
    if (n <= 0) {
      break;
    }
    written += static_cast<std::size_t>(n);
  }
  ::_exit(127);
}

// 手工路径拼接：子进程中不得使用 snprintf 等非 async-signal-safe 函数。
bool join_path(const char* directory, std::size_t directory_size, const char* name,
               std::size_t name_size, char* out, std::size_t out_size) noexcept {
  if (directory_size == 0 || name_size == 0 || directory_size + 1 + name_size + 1 > out_size) {
    return false;
  }
  std::memcpy(out, directory, directory_size);
  std::size_t offset = directory_size;
  if (out[offset - 1] != '/') {
    out[offset] = '/';
    ++offset;
  }
  std::memcpy(out + offset, name, name_size);
  out[offset + name_size] = '\0';
  return true;
}

// 子进程 PATH 解析 + execve。仅使用 async-signal-safe 调用；envp 无 PATH 时回退
// 系统默认路径（confstr(_CS_PATH) 的静态近似，与 posix_spawn 缺省一致）。
[[noreturn]] void child_exec(const launch::LaunchPlan& plan, char* const* argv,
                             char* const* envp) noexcept {
  if (plan.argv.front().find('/') != std::string::npos) {
    ::execve(argv[0], argv, envp);
    write_child_report(ChildStage::kExec, errno);
  }
  const char* path = nullptr;
  for (char* const* entry = envp; *entry != nullptr; ++entry) {
    if (std::strncmp(*entry, "PATH=", 5) == 0) {
      path = *entry + 5;
      break;
    }
  }
  if (path == nullptr) {
    path = "/bin:/usr/bin";
  }
  const char* cursor = path;
  std::array<char, 4096> candidate{};
  while (*cursor != '\0') {
    const char* end = std::strchr(cursor, ':');
    const std::size_t length =
        end != nullptr ? static_cast<std::size_t>(end - cursor) : std::strlen(cursor);
    if (length > 0 &&
        join_path(cursor, length, plan.argv.front().c_str(), plan.argv.front().size(),
                  candidate.data(), candidate.size()) &&
        ::access(candidate.data(), X_OK) == 0) {
      ::execve(candidate.data(), argv, envp);
      if (errno != EACCES && errno != ENOENT && errno != ENOTDIR && errno != ELOOP &&
          errno != ENAMETOOLONG) {
        break;
      }
    }
    if (end == nullptr) {
      break;
    }
    cursor = end + 1;
  }
  write_child_report(ChildStage::kExec, ENOENT);
}

// 子进程主路径：所有失败经 exec 报告管道上报，绝不能返回。argv/envp 的字符串与
// 指针数组都在 fork 前由父进程构造，子进程内不分配内存（async-signal-safe 纪律）。
[[noreturn]] void run_child(const launch::LaunchPlan& plan, int stdout_write, int stderr_write,
                            int exec_report_write, const gid_t* groups, std::size_t group_count,
                            char* const* argv, char* const* envp) noexcept {
  if (::dup2(exec_report_write, kExecReportFd) < 0) {
    ::_exit(126);
  }
  ::fcntl(kExecReportFd, F_SETFD, FD_CLOEXEC);

  if (::setpgid(0, 0) < 0) {
    write_child_report(ChildStage::kProcessGroup, errno);
  }

  // DEC-008：SIGPIPE 忽略跨 execve 存活，daemon 退出不击杀训练进程。
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  ::sigemptyset(&ignore.sa_mask);
  ::sigaction(SIGPIPE, &ignore, nullptr);

  const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (devnull < 0) {
    write_child_report(ChildStage::kExec, errno);
  }
  if (::dup2(devnull, STDIN_FILENO) < 0 || ::dup2(stdout_write, STDOUT_FILENO) < 0 ||
      ::dup2(stderr_write, STDERR_FILENO) < 0) {
    write_child_report(ChildStage::kExec, errno);
  }

  // 关闭 0-3 之外继承的全部描述符（fd 3 保留至 exec 以报告失败）。
  static_cast<void>(
      ::syscall(SYS_close_range, static_cast<unsigned int>(kExecReportFd) + 1, ~0U, 0U));

  // DEC-004/DEC-006：setgroups -> setgid -> setuid，全部在 fork 后、exec 前完成；
  // 目标即当前有效身份时为幂等空操作（非 root 环境自身份 spawn）。组列表为空时
  // 至少保留主组（initgroups 语义）。
  const uid_t target_uid = static_cast<uid_t>(plan.uid);
  const gid_t target_gid = static_cast<gid_t>(plan.gid);
  if (::geteuid() != target_uid || ::getegid() != target_gid) {
    gid_t primary_only[1] = {target_gid};
    const gid_t* group_list = groups;
    std::size_t group_size = group_count;
    if (group_size == 0) {
      group_list = primary_only;
      group_size = 1;
    }
    if (::setgroups(group_size, group_list) < 0) {
      write_child_report(ChildStage::kDemotion, errno);
    }
    if (::setgid(target_gid) < 0) {
      write_child_report(ChildStage::kDemotion, errno);
    }
    if (::setuid(target_uid) < 0) {
      write_child_report(ChildStage::kDemotion, errno);
    }
  }

  if (!plan.cwd.empty() && ::chdir(plan.cwd.c_str()) < 0) {
    write_child_report(ChildStage::kChdir, errno);
  }

  child_exec(plan, argv, envp);
}

SpawnErrorCode map_child_stage(ChildStage stage) noexcept {
  switch (stage) {
    case ChildStage::kProcessGroup:
      return SpawnErrorCode::kProcessGroupFailed;
    case ChildStage::kDemotion:
      return SpawnErrorCode::kIdentitySwitchFailed;
    case ChildStage::kChdir:
      return SpawnErrorCode::kChdirFailed;
    case ChildStage::kExec:
      return SpawnErrorCode::kExecFailed;
  }
  return SpawnErrorCode::kExecFailed;
}

ExitStatus status_from_wait(int raw_status) noexcept {
  ExitStatus status;
  if (WIFEXITED(raw_status)) {
    status.reason = ExitReason::kExited;
    status.exit_code = WEXITSTATUS(raw_status);
  } else if (WIFSIGNALED(raw_status)) {
    status.reason = ExitReason::kSignaled;
    status.signal_number = WTERMSIG(raw_status);
  }
  return status;
}

// 阻塞回收指定子进程，容忍 EINTR。
void reap_child(pid_t pid) noexcept {
  while (true) {
    int raw_status = 0;
    const pid_t reaped = ::waitpid(pid, &raw_status, 0);
    if (reaped == pid || (reaped < 0 && errno == ECHILD)) {
      return;
    }
    if (reaped < 0 && errno != EINTR) {
      return;
    }
  }
}

bool wait_for_exec_confirmation(int exec_report_read, SpawnResult& result) noexcept {
  struct pollfd poll_fd {};
  poll_fd.fd = exec_report_read;
  poll_fd.events = POLLIN;
  int ready = 0;
  do {
    ready = ::poll(&poll_fd, 1, static_cast<int>(kExecConfirmationTimeout.count()));
  } while (ready < 0 && errno == EINTR);
  if (ready == 0) {
    result.code = SpawnErrorCode::kExecConfirmationTimeout;
    result.error_number = 0;
    result.message = "child did not confirm exec within the timeout";
    return false;
  }
  if (ready < 0) {
    result.code = SpawnErrorCode::kExecConfirmationTimeout;
    result.error_number = errno;
    result.message = "poll on the exec report pipe failed";
    return false;
  }
  std::array<unsigned char, 2> report{};
  ssize_t total = 0;
  while (total < static_cast<ssize_t>(report.size())) {
    const ssize_t n = ::read(exec_report_read, report.data() + total, report.size() - total);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;  // EOF：exec 成功，报告管道被 FD_CLOEXEC 关闭。
    }
    total += n;
  }
  if (total == 0) {
    return true;
  }
  if (total == 1) {
    report[1] = 0;
  }
  result.code = map_child_stage(static_cast<ChildStage>(report[0]));
  result.error_number = report[1];
  result.message = "child failed before or during exec";
  return false;
}

}  // namespace

FileDescriptor::~FileDescriptor() { reset(); }

void FileDescriptor::reset() noexcept {
  if (fd_ >= 0) {
    static_cast<void>(::close(fd_));
    fd_ = -1;
  }
}

std::optional<std::uint64_t> read_process_start_ticks(std::int64_t pid) noexcept {
  if (pid <= 0) {
    return std::nullopt;
  }
  char path[48];
  const int written =
      std::snprintf(path, sizeof(path), "/proc/%lld/stat", static_cast<long long>(pid));
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path)) {
    return std::nullopt;
  }
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::nullopt;
  }
  std::array<char, 1024> buffer{};
  std::size_t used = 0;
  while (used < buffer.size() - 1) {
    const ssize_t n = ::read(fd, buffer.data() + used, buffer.size() - 1 - used);
    if (n <= 0) {
      break;
    }
    used += static_cast<std::size_t>(n);
  }
  static_cast<void>(::close(fd));
  buffer[used] = '\0';

  // comm 字段可能包含空格与括号，从最后一个 ')' 之后开始解析；starttime 是整个
  // stat 的第 22 字段，即 ')' 后的第 20 个空白分隔 token。
  const char* cursor = std::strrchr(buffer.data(), ')');
  if (cursor == nullptr) {
    return std::nullopt;
  }
  ++cursor;
  int token = 1;
  while (token < 20) {
    while (*cursor == ' ' || *cursor == '\t') {
      ++cursor;
    }
    if (*cursor == '\0') {
      return std::nullopt;
    }
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
      ++cursor;
    }
    ++token;
  }
  while (*cursor == ' ' || *cursor == '\t') {
    ++cursor;
  }
  if (*cursor == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const unsigned long long ticks = std::strtoull(cursor, &end, 10);
  if (end == cursor) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(ticks);
}

const char* to_string(ExitReason reason) noexcept {
  switch (reason) {
    case ExitReason::kExited:
      return "exited";
    case ExitReason::kSignaled:
      return "signaled";
    case ExitReason::kUnknown:
      return "unknown";
  }
  return "unknown";
}

const char* to_string(SpawnErrorCode code) noexcept {
  switch (code) {
    case SpawnErrorCode::kNone:
      return "none";
    case SpawnErrorCode::kAlreadyRunning:
      return "supervisor already manages a running process";
    case SpawnErrorCode::kInvalidPlan:
      return "invalid launch plan";
    case SpawnErrorCode::kPipeFailed:
      return "pipe creation failed";
    case SpawnErrorCode::kDevNullFailed:
      return "opening /dev/null failed";
    case SpawnErrorCode::kForkFailed:
      return "fork failed";
    case SpawnErrorCode::kProcessGroupFailed:
      return "setting up the process group failed";
    case SpawnErrorCode::kChdirFailed:
      return "changing into the working directory failed";
    case SpawnErrorCode::kIdentitySwitchFailed:
      return "switching to the job owner identity failed";
    case SpawnErrorCode::kExecFailed:
      return "exec failed";
    case SpawnErrorCode::kExecConfirmationTimeout:
      return "exec confirmation timed out";
    case SpawnErrorCode::kIdentityReadFailed:
      return "reading the process identity failed";
  }
  return "unknown";
}

const char* to_string(CancelOutcome outcome) noexcept {
  switch (outcome) {
    case CancelOutcome::kTerminating:
      return "terminating";
    case CancelOutcome::kAlreadyTerminating:
      return "already terminating";
    case CancelOutcome::kNotRunning:
      return "no running process";
    case CancelOutcome::kAlreadyExited:
      return "already exited";
    case CancelOutcome::kSignalFailed:
      return "signal delivery failed";
  }
  return "unknown";
}

const char* to_string(EscalationOutcome outcome) noexcept {
  switch (outcome) {
    case EscalationOutcome::kKilled:
      return "killed";
    case EscalationOutcome::kGraceNotExpired:
      return "grace period not expired";
    case EscalationOutcome::kNotTerminating:
      return "not terminating";
    case EscalationOutcome::kAlreadyExited:
      return "already exited";
    case EscalationOutcome::kSignalFailed:
      return "signal delivery failed";
  }
  return "unknown";
}

const char* to_string(ExitPollOutcome outcome) noexcept {
  switch (outcome) {
    case ExitPollOutcome::kStillRunning:
      return "still running";
    case ExitPollOutcome::kExited:
      return "exited";
    case ExitPollOutcome::kNotSpawned:
      return "not spawned";
    case ExitPollOutcome::kWaitFailed:
      return "waitpid failed";
  }
  return "unknown";
}

struct ProcessSupervisor::Impl final {
  CancelPolicy policy{};
  Phase phase{Phase::kIdle};
  ProcessIdentity identity{};
  ExitStatus exit_status{};
  std::chrono::steady_clock::time_point grace_deadline{};
};

const char* ProcessSupervisor::to_string(Phase phase) noexcept {
  switch (phase) {
    case Phase::kIdle:
      return "idle";
    case Phase::kRunning:
      return "running";
    case Phase::kTerminating:
      return "terminating";
    case Phase::kExited:
      return "exited";
  }
  return "unknown";
}

ProcessSupervisor::ProcessSupervisor(CancelPolicy policy) : impl_(std::make_unique<Impl>()) {
  impl_->policy = policy;
}

ProcessSupervisor::~ProcessSupervisor() {
  if (impl_->phase == Phase::kRunning || impl_->phase == Phase::kTerminating) {
    static_cast<void>(::kill(-static_cast<pid_t>(impl_->identity.pgid), SIGKILL));
    static_cast<void>(::kill(static_cast<pid_t>(impl_->identity.pid), SIGKILL));
    reap_child(static_cast<pid_t>(impl_->identity.pid));
    impl_->phase = Phase::kExited;
  }
}

SpawnResult ProcessSupervisor::spawn(const launch::LaunchPlan& plan) {
  SpawnResult result;
  if (impl_->phase == Phase::kRunning || impl_->phase == Phase::kTerminating) {
    result.code = SpawnErrorCode::kAlreadyRunning;
    result.message = yori::process::to_string(result.code);
    return result;
  }
  if (const launch::LaunchPlanValidationResult check = launch::validate(plan); !check.ok()) {
    result.code = SpawnErrorCode::kInvalidPlan;
    result.message = launch::to_string(check.code);
    return result;
  }
  if (!impl_->policy.valid()) {
    result.code = SpawnErrorCode::kInvalidPlan;
    result.message = "invalid cancel policy";
    return result;
  }

  std::array<int, 2> stdout_pipe{};
  std::array<int, 2> stderr_pipe{};
  std::array<int, 2> exec_report_pipe{};
  if (::pipe2(stdout_pipe.data(), O_CLOEXEC) < 0 || ::pipe2(stderr_pipe.data(), O_CLOEXEC) < 0 ||
      ::pipe2(exec_report_pipe.data(), O_CLOEXEC) < 0) {
    result.code = SpawnErrorCode::kPipeFailed;
    result.error_number = errno;
    result.message = yori::process::to_string(result.code);
    return result;
  }

  std::vector<gid_t> groups;
  groups.reserve(plan.supplementary_groups.size());
  for (const auto gid : plan.supplementary_groups) {
    groups.push_back(static_cast<gid_t>(gid));
  }

  // argv/envp 全部在 fork 前构造（子进程内不分配内存）。
  std::vector<char*> argv;
  argv.reserve(plan.argv.size() + 1);
  for (const auto& argument : plan.argv) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  std::vector<std::string> environment_strings;
  environment_strings.reserve(plan.env.size());
  for (const auto& [name, value] : plan.env) {
    std::string entry;
    entry.reserve(name.size() + 1 + value.size());
    entry.append(name);
    entry.push_back('=');
    entry.append(value);
    environment_strings.emplace_back(std::move(entry));
  }
  std::vector<char*> envp;
  envp.reserve(environment_strings.size() + 1);
  for (const auto& entry : environment_strings) {
    envp.push_back(const_cast<char*>(entry.c_str()));
  }
  envp.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    result.code = SpawnErrorCode::kForkFailed;
    result.error_number = errno;
    result.message = yori::process::to_string(result.code);
    return result;
  }
  if (child == 0) {
    run_child(plan, stdout_pipe[1], stderr_pipe[1], exec_report_pipe[1], groups.data(),
              groups.size(), argv.data(), envp.data());
  }

  FileDescriptor stdout_read(stdout_pipe[0]);
  FileDescriptor stderr_read(stderr_pipe[0]);
  FileDescriptor exec_report_read(exec_report_pipe[0]);
  static_cast<void>(::close(stdout_pipe[1]));
  static_cast<void>(::close(stderr_pipe[1]));
  static_cast<void>(::close(exec_report_pipe[1]));

  // 双端 setpgid 关闭竞态窗口；子进程已 exec 或退出时的失败由子进程报告兜底。
  if (::setpgid(child, child) < 0 && errno != EACCES && errno != ESRCH) {
    static_cast<void>(::kill(child, SIGKILL));
    reap_child(child);
    result.code = SpawnErrorCode::kProcessGroupFailed;
    result.error_number = errno;
    result.message = yori::process::to_string(result.code);
    return result;
  }

  if (!wait_for_exec_confirmation(exec_report_read.value(), result)) {
    static_cast<void>(::kill(child, SIGKILL));
    reap_child(child);
    return result;
  }

  const pid_t pgid = ::getpgid(child);
  const auto start_ticks = read_process_start_ticks(static_cast<std::int64_t>(child));
  if (pgid < 0 || !start_ticks.has_value()) {
    static_cast<void>(::kill(child, SIGKILL));
    reap_child(child);
    result.code = SpawnErrorCode::kIdentityReadFailed;
    result.error_number = errno;
    result.message = yori::process::to_string(result.code);
    return result;
  }

  impl_->identity = ProcessIdentity{static_cast<std::int64_t>(child),
                                    static_cast<std::int64_t>(pgid), *start_ticks};
  impl_->exit_status = ExitStatus{};
  impl_->phase = Phase::kRunning;

  result.code = SpawnErrorCode::kNone;
  result.identity = impl_->identity;
  result.stdout_read = std::move(stdout_read);
  result.stderr_read = std::move(stderr_read);
  return result;
}

CancelResult ProcessSupervisor::request_cancel() noexcept {
  CancelResult result;
  switch (impl_->phase) {
    case Phase::kIdle:
      result.outcome = CancelOutcome::kNotRunning;
      return result;
    case Phase::kRunning:
      break;
    case Phase::kTerminating:
      result.outcome = CancelOutcome::kAlreadyTerminating;
      return result;
    case Phase::kExited:
      result.outcome = CancelOutcome::kAlreadyExited;
      return result;
  }
  const pid_t pgid = static_cast<pid_t>(impl_->identity.pgid);
  if (::kill(-pgid, SIGTERM) < 0 && ::kill(pgid, SIGTERM) < 0) {
    result.outcome = CancelOutcome::kSignalFailed;
    result.error_number = errno;
    return result;
  }
  impl_->grace_deadline = std::chrono::steady_clock::now() + impl_->policy.grace_period;
  impl_->phase = Phase::kTerminating;
  result.outcome = CancelOutcome::kTerminating;
  return result;
}

bool ProcessSupervisor::grace_expired() const noexcept {
  return impl_->phase == Phase::kTerminating &&
         std::chrono::steady_clock::now() >= impl_->grace_deadline;
}

EscalationResult ProcessSupervisor::escalate() noexcept {
  EscalationResult result;
  switch (impl_->phase) {
    case Phase::kIdle:
    case Phase::kRunning:
      result.outcome = EscalationOutcome::kNotTerminating;
      return result;
    case Phase::kTerminating:
      break;
    case Phase::kExited:
      result.outcome = EscalationOutcome::kAlreadyExited;
      return result;
  }
  if (!grace_expired()) {
    result.outcome = EscalationOutcome::kGraceNotExpired;
    return result;
  }
  const pid_t pgid = static_cast<pid_t>(impl_->identity.pgid);
  if (::kill(-pgid, SIGKILL) < 0 && ::kill(pgid, SIGKILL) < 0) {
    if (errno == ESRCH) {
      result.outcome = EscalationOutcome::kAlreadyExited;
      return result;
    }
    result.outcome = EscalationOutcome::kSignalFailed;
    result.error_number = errno;
    return result;
  }
  result.outcome = EscalationOutcome::kKilled;
  return result;
}

ExitPollResult ProcessSupervisor::poll_exit() noexcept {
  ExitPollResult result;
  switch (impl_->phase) {
    case Phase::kIdle:
      result.outcome = ExitPollOutcome::kNotSpawned;
      return result;
    case Phase::kExited:
      result.outcome = ExitPollOutcome::kExited;
      result.status = impl_->exit_status;
      return result;
    case Phase::kRunning:
    case Phase::kTerminating:
      break;
  }
  int raw_status = 0;
  const pid_t reaped = ::waitpid(static_cast<pid_t>(impl_->identity.pid), &raw_status, WNOHANG);
  if (reaped == 0) {
    result.outcome = ExitPollOutcome::kStillRunning;
    return result;
  }
  if (reaped < 0) {
    result.outcome = ExitPollOutcome::kWaitFailed;
    result.error_number = errno;
    impl_->exit_status = ExitStatus{};
    impl_->phase = Phase::kExited;
    return result;
  }
  impl_->exit_status = status_from_wait(raw_status);
  impl_->phase = Phase::kExited;
  result.outcome = ExitPollOutcome::kExited;
  result.status = impl_->exit_status;
  return result;
}

ExitStatus ProcessSupervisor::consume_exit() noexcept {
  if (impl_->phase != Phase::kExited) {
    return ExitStatus{};
  }
  return impl_->exit_status;
}

ProcessSupervisor::Phase ProcessSupervisor::phase() const noexcept { return impl_->phase; }

const ProcessIdentity& ProcessSupervisor::identity() const noexcept { return impl_->identity; }

const CancelPolicy& ProcessSupervisor::cancel_policy() const noexcept { return impl_->policy; }

}  // namespace yori::process
