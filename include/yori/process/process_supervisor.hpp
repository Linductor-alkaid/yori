#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <yori/launch/launch_adapter.hpp>

namespace yori::process {

// 父进程侧文件描述符所有权句柄。close 在析构时执行；release() 交出所有权
// （例如转交日志泵）。
class FileDescriptor final {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  ~FileDescriptor();

  [[nodiscard]] int value() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset() noexcept;

 private:
  int fd_{-1};
};

// 进程身份三元组（设计第 10.2 节）：恢复与 PID reuse 核验依据（RULE-06）。
// start_ticks 是 /proc/<pid>/stat 的第 22 字段（进程启动时的时钟滴答）。
struct ProcessIdentity final {
  std::int64_t pid{0};
  std::int64_t pgid{0};
  std::uint64_t start_ticks{0};

  [[nodiscard]] bool valid() const noexcept { return pid > 0 && pgid > 0 && start_ticks > 0; }
};

// 读取 /proc/<pid>/stat 的启动 ticks。进程不存在、已彻底消失或解析失败时返回
// nullopt；僵尸进程仍可读。
[[nodiscard]] std::optional<std::uint64_t> read_process_start_ticks(std::int64_t pid) noexcept;

enum class ExitReason {
  kExited,
  kSignaled,
  kUnknown,
};

[[nodiscard]] const char* to_string(ExitReason reason) noexcept;

struct ExitStatus final {
  ExitReason reason{ExitReason::kUnknown};
  int exit_code{0};
  int signal_number{0};

  [[nodiscard]] bool exited() const noexcept { return reason == ExitReason::kExited; }
  [[nodiscard]] bool signaled() const noexcept { return reason == ExitReason::kSignaled; }
  [[nodiscard]] bool is_success() const noexcept {
    return reason == ExitReason::kExited && exit_code == 0;
  }
};

struct CancelPolicyLimits final {
  static constexpr std::chrono::milliseconds kMinGracePeriod{100};
  static constexpr std::chrono::milliseconds kMaxGracePeriod{600000};
  static constexpr std::chrono::milliseconds kDefaultGracePeriod{10000};
};

// 取消升级策略（DEC-007）。grace_period 为 SIGTERM 到 SIGKILL 的软期限。
struct CancelPolicy final {
  std::chrono::milliseconds grace_period{CancelPolicyLimits::kDefaultGracePeriod};

  [[nodiscard]] bool valid() const noexcept {
    return grace_period >= CancelPolicyLimits::kMinGracePeriod &&
           grace_period <= CancelPolicyLimits::kMaxGracePeriod;
  }
};

enum class SpawnErrorCode {
  kNone,
  kAlreadyRunning,
  kInvalidPlan,
  kPipeFailed,
  kDevNullFailed,
  kForkFailed,
  kProcessGroupFailed,
  kChdirFailed,
  kIdentitySwitchFailed,
  kExecFailed,
  kExecConfirmationTimeout,
  kIdentityReadFailed,
};

[[nodiscard]] const char* to_string(SpawnErrorCode code) noexcept;

struct SpawnResult final {
  SpawnErrorCode code{SpawnErrorCode::kNone};
  ProcessIdentity identity;
  FileDescriptor stdout_read;
  FileDescriptor stderr_read;
  int error_number{0};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == SpawnErrorCode::kNone; }
  explicit operator bool() const noexcept { return ok(); }
};

enum class CancelOutcome {
  kTerminating,
  kAlreadyTerminating,
  kNotRunning,
  kAlreadyExited,
  kSignalFailed,
};

[[nodiscard]] const char* to_string(CancelOutcome outcome) noexcept;

struct CancelResult final {
  CancelOutcome outcome{CancelOutcome::kNotRunning};
  int error_number{0};

  [[nodiscard]] bool terminating() const noexcept {
    return outcome == CancelOutcome::kTerminating || outcome == CancelOutcome::kAlreadyTerminating;
  }
};

enum class EscalationOutcome {
  kKilled,
  kGraceNotExpired,
  kNotTerminating,
  kAlreadyExited,
  kSignalFailed,
};

[[nodiscard]] const char* to_string(EscalationOutcome outcome) noexcept;

struct EscalationResult final {
  EscalationOutcome outcome{EscalationOutcome::kNotTerminating};
  int error_number{0};

  [[nodiscard]] bool killed() const noexcept { return outcome == EscalationOutcome::kKilled; }
};

enum class ExitPollOutcome {
  kStillRunning,
  kExited,
  kNotSpawned,
  kWaitFailed,
};

[[nodiscard]] const char* to_string(ExitPollOutcome outcome) noexcept;

struct ExitPollResult final {
  ExitPollOutcome outcome{ExitPollOutcome::kNotSpawned};
  ExitStatus status;
  int error_number{0};

  [[nodiscard]] bool exited() const noexcept { return outcome == ExitPollOutcome::kExited; }
};

// 单 owner、单进程的守护状态机（设计第 10.2 节）。同步推进、无内部线程；异步承载
// 与宽限定时由 Executor 适配层（yori_runtime）驱动。取消是外部进程语义，不与
// Executor 任务取消混同。
class ProcessSupervisor final {
 public:
  enum class Phase {
    kIdle,
    kRunning,
    kTerminating,
    kExited,
  };

  [[nodiscard]] static const char* to_string(Phase phase) noexcept;

  explicit ProcessSupervisor(CancelPolicy policy = {});

  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;
  ProcessSupervisor(ProcessSupervisor&&) = delete;
  ProcessSupervisor& operator=(ProcessSupervisor&&) = delete;

  // 兜底语义：析构时若进程仍在运行/终止中，对进程组 SIGKILL 并有界回收，保证不
  // 留孤儿与僵尸；正常路径应先显式推进到 kExited。
  ~ProcessSupervisor();

  // fork/exec 一个训练进程：独立进程组、stdout/stderr 管道捕获、子进程 exec 前
  // SIGPIPE=SIG_IGN（DEC-008）并完成 setgroups -> setgid -> setuid 降权（DEC-004）。
  // 仅在 kIdle/kExited 阶段可调用。
  [[nodiscard]] SpawnResult spawn(const launch::LaunchPlan& plan);

  // 对整个进程组发送 SIGTERM 并记录宽限截止时刻（DEC-007）。
  [[nodiscard]] CancelResult request_cancel() noexcept;

  // 宽限截止是否已到（steady_clock 语义）。
  [[nodiscard]] bool grace_expired() const noexcept;

  // 宽限期到后的升级：对进程组发送 SIGKILL。宽限未到或未在取消中时为显式空结果。
  [[nodiscard]] EscalationResult escalate() noexcept;

  // 非阻塞回收一步：waitpid(pid, WNOHANG)。kExited 结果由 consume_exit 取走。
  [[nodiscard]] ExitPollResult poll_exit() noexcept;

  // 取走已回收的退出状态（仅在 poll_exit 返回 kExited 后有效）；重复消费返回
  // kUnknown，终态幂等。
  [[nodiscard]] ExitStatus consume_exit() noexcept;

  [[nodiscard]] Phase phase() const noexcept;
  [[nodiscard]] const ProcessIdentity& identity() const noexcept;
  [[nodiscard]] const CancelPolicy& cancel_policy() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::process
