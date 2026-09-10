#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <executor/comm/channel.hpp>
#include <functional>
#include <memory>
#include <yori/process/process_supervisor.hpp>

namespace executor {
class Executor;
}

namespace yori::runtime {

// waitpid 回收结果事件（EXEC-07）。identity_verified=false 表示回收前身份核验失败
// （PID 已被他人回收并复用），status 为 unknown。
struct ExitEvent final {
  std::int64_t pid{0};
  process::ProcessIdentity identity;
  process::ExitStatus status;
  bool identity_verified{true};
};

enum class ExitMonitorStartCode {
  kStarted,
  kAlreadyStarted,
  kWorkerRejected,
  kSignalInstallFailed,
};

struct ExitMonitorStartResult final {
  ExitMonitorStartCode code{ExitMonitorStartCode::kWorkerRejected};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == ExitMonitorStartCode::kStarted; }
};

enum class ExitRegisterCode {
  kRegistered,
  kNotStarted,
  kInvalidIdentity,
  kDuplicate,
  kChannelFull,
  kAckTimeout,
  kStopped,
};

struct ExitRegisterResult final {
  ExitRegisterCode code{ExitRegisterCode::kNotStarted};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == ExitRegisterCode::kRegistered; }
};

enum class ExitUnregisterCode {
  kUnregistered,
  kNotStarted,
  kNotFound,
  kChannelFull,
  kAckTimeout,
};

struct ExitUnregisterResult final {
  ExitUnregisterCode code{ExitUnregisterCode::kNotStarted};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == ExitUnregisterCode::kUnregistered; }
};

// 进程退出监视与回收的 Executor 承载（总计划 EXEC-07）：单个 blocking worker，
// SIGCHLD 经自管道唤醒，按注册 PID 逐个 waitpid(WNOHANG) 回收并核验身份。注册、
// 注销命令与退出事件都走有界 MpscChannel；事件容量满时 worker 有界重试并经 comm
// 统计可见（背压显式，不静默丢弃子进程退出）。
class ProcessExitMonitor final {
 public:
  // event_capacity：退出事件通道容量；command_capacity：命令通道容量（准入上限）。
  ProcessExitMonitor(executor::Executor& executor, std::size_t event_capacity = 256,
                     std::size_t command_capacity = 256);
  ~ProcessExitMonitor();

  ProcessExitMonitor(const ProcessExitMonitor&) = delete;
  ProcessExitMonitor& operator=(const ProcessExitMonitor&) = delete;
  ProcessExitMonitor(ProcessExitMonitor&&) = delete;
  ProcessExitMonitor& operator=(ProcessExitMonitor&&) = delete;

  // 安装 SIGCHLD 处理器并启动 blocking worker。重复 start 为幂等成功。
  [[nodiscard]] ExitMonitorStartResult start();

  // 注册一个已 spawn 的进程身份；注册后立即补扫一次，覆盖"注册前已退出"竞态。
  [[nodiscard]] ExitRegisterResult register_process(process::ProcessIdentity identity);

  // 注销 PID：不再回收、不再投递其退出事件（进程组仍在运行，调用方自行负责）。
  [[nodiscard]] ExitUnregisterResult unregister_process(std::int64_t pid);

  // 事件消费（单消费者）。stop 之后缓冲事件仍可读取，析构前应排空。
  [[nodiscard]] bool try_receive_exit(ExitEvent& out);
  [[nodiscard]] bool receive_exit_for(ExitEvent& out, std::chrono::milliseconds timeout);

  // 变更通知（M7 守护总装）：事件成功投递进通道后回调，供守护承载
  // （JobManager）的唤醒管道触发（MpscChannel 无 fd 可 poll）。必须在并发
  // 使用前设置一次；回调自身必须非阻塞、不抛出、可从 worker 线程调用；
  // 清空以解除挂钩。
  void set_event_listener(std::function<void()> listener);

  [[nodiscard]] std::size_t registered_count() const noexcept;
  [[nodiscard]] std::uint64_t delivery_retry_count() const noexcept;

  // 停止 worker（请求停止、唤醒并 join），恢复 SIGCHLD 旧处置。不终止任何被监视
  // 进程（RULE-10）。幂等。
  void stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
