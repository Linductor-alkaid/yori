#pragma once

#include <chrono>
#include <cstddef>
#include <executor/comm/channel.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <yori/job/job.hpp>
#include <yori/observe/log_sink.hpp>
#include <yori/process/process_supervisor.hpp>

namespace executor {
class Executor;
}

namespace yori::runtime {

// 日志块观察者（M6，EXEC-04 数据面）：LogPump worker 在落盘接受后调用，
// 携带该次接受的逻辑 offset 区间。实现（LogStreamer 桥）必须非阻塞、不抛
// 出；观察失败不得影响落盘主路径（观察不得影响被观察者，设计 11.1）。
class LogChunkObserver {
 public:
  virtual ~LogChunkObserver() = default;

  // 一块已落盘接受的字节：[begin_offset, end_offset)，data 即接受的数据。
  virtual void on_chunk(const job::JobId& job, observe::LogStreamKind stream, std::string_view data,
                        std::uint64_t begin_offset, std::uint64_t end_offset) = 0;
  // 落盘写失败丢弃：offset 不前进，观察侧发布丢弃标记（设计 11.2）。
  virtual void on_drop(const job::JobId& job, observe::LogStreamKind stream, std::uint64_t offset,
                       std::uint64_t dropped_bytes) = 0;
};

// attach 到日志泵的一个 Job：两路管道读端与已打开的 LogSink。读端可以无效（该流
// 未捕获），无效流立即视为完成。observer 可为空（不启用直播分发，纯落盘）。
struct LogPumpJobInput final {
  job::JobId job{};
  process::FileDescriptor stdout_read;
  process::FileDescriptor stderr_read;
  observe::LogSink sink;
  std::shared_ptr<LogChunkObserver> observer;
};

// Job 日志排空结果：completed 表示两路流均 EOF；error 汇总读/写错误。
struct LogPumpDone final {
  job::JobId job{};
  bool completed{false};
  std::string error;
  observe::LogSinkStatistics statistics;
};

enum class LogPumpStartCode {
  kStarted,
  kAlreadyStarted,
  kWorkerRejected,
};

struct LogPumpStartResult final {
  LogPumpStartCode code{LogPumpStartCode::kWorkerRejected};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == LogPumpStartCode::kStarted; }
};

enum class LogPumpAttachCode {
  kAttached,
  kNotStarted,
  kInvalidInput,
  kDuplicateJob,
  kChannelFull,
  kAckTimeout,
};

struct LogPumpAttachResult final {
  LogPumpAttachCode code{LogPumpAttachCode::kNotStarted};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == LogPumpAttachCode::kAttached; }
};

enum class LogPumpDetachCode {
  kDetached,
  kNotStarted,
  kNotFound,
  kChannelFull,
  kAckTimeout,
};

struct LogPumpDetachResult final {
  LogPumpDetachCode code{LogPumpDetachCode::kNotStarted};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == LogPumpDetachCode::kDetached; }
};

// 日志捕获的 Executor 承载（总计划 EXEC-03）：单个 blocking worker 以 poll 排空
// 多个 Job 的 stdout/stderr 管道并同步写入 LogSink（轮转、丢弃标记由 LogSink 处
// 理）。attach/detach 命令与完成事件都走有界 MpscChannel；wakeup 经自管道。停止
// 时关闭全部读端（子进程写端获得 EPIPE，不致死，DEC-008）。
class LogPump final {
 public:
  LogPump(executor::Executor& executor, std::size_t done_capacity = 256);
  ~LogPump();

  LogPump(const LogPump&) = delete;
  LogPump& operator=(const LogPump&) = delete;
  LogPump(LogPump&&) = delete;
  LogPump& operator=(LogPump&&) = delete;

  [[nodiscard]] LogPumpStartResult start();

  // 接管一个 Job 的管道读端与 LogSink；两路 EOF 后投递 LogPumpDone 并释放资源。
  [[nodiscard]] LogPumpAttachResult attach(LogPumpJobInput&& input);

  // 提前移除一个 Job（关闭读端、落盘收尾），不投递完成事件。
  [[nodiscard]] LogPumpDetachResult detach(job::JobId job);

  [[nodiscard]] bool try_receive_done(LogPumpDone& out);
  [[nodiscard]] bool receive_done_for(LogPumpDone& out, std::chrono::milliseconds timeout);

  // 完成事件投递成功后由 pump worker 线程调用（与 ProcessExitMonitor 的
  // set_event_listener 同型）。消费方（JobManager worker）以通道 poll 不可达，
  // 必须经此唤醒，否则 EOF 发布汇合（退出事件 + 泵完成）可能滞留到下一个
  // 无关事件。回调必须非阻塞、不抛出；置空解除。
  void set_done_listener(std::function<void()> listener);

  // 停止 worker（请求停止、唤醒并 join），幂等；不向子进程发送任何信号。
  void stop();

 private:
  void kick_worker() noexcept;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
