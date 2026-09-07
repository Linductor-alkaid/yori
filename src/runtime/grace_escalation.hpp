#pragma once

#include <optional>
#include <yori/process/process_supervisor.hpp>

namespace executor {
class Executor;
}

namespace yori::runtime {

enum class GraceArmCode {
  kArmed,
  kAlreadyArmed,
  kSupervisorNotTerminating,
  kExecutorRejected,
};

enum class GraceDisarmCode {
  kDisarmed,
  kNotArmed,
  kCancelledBeforeDispatch,
  kConsumedCompletion,
};

enum class GraceConsumeCode {
  kNotArmed,
  kNotReady,
  kConsumed,
};

struct GraceConsumeResult final {
  GraceConsumeCode code{GraceConsumeCode::kNotArmed};
  std::optional<process::EscalationResult> escalation;

  [[nodiscard]] bool consumed() const noexcept { return code == GraceConsumeCode::kConsumed; }
};

// 取消宽限升级的 Executor 承载（总计划 EXEC-07、DEC-007）：arm() 在
// request_cancel() 之后提交一个 submit_delayed 一次性任务，到期调用
// ProcessSupervisor::escalate()；进程提前退出时 disarm() 取消定时或同步消费已
// 派发的升级结果。
//
// owner 纪律：arm() 到 disarm()/consume 之间，单 owner 不得并发调用同一
// ProcessSupervisor 的其他可变方法（escalate 回调在 Executor 线程执行）；析构会
// 先 disarm，保证回调不再引用 supervisor。
class GraceEscalation final {
 public:
  GraceEscalation(executor::Executor& executor, process::ProcessSupervisor& supervisor);
  ~GraceEscalation();

  GraceEscalation(const GraceEscalation&) = delete;
  GraceEscalation& operator=(const GraceEscalation&) = delete;
  GraceEscalation(GraceEscalation&&) = delete;
  GraceEscalation& operator=(GraceEscalation&&) = delete;

  [[nodiscard]] GraceArmCode arm();

  // 取消未派发的定时；若已派发则等待回调完成并消费其 future（有界：回调只发送
  // 一个组信号）。返回实际采取的路径。
  [[nodiscard]] GraceDisarmCode disarm();

  // 非阻塞消费升级任务结果（future 必须被消费，异常不得吞掉）。
  [[nodiscard]] GraceConsumeResult try_consume();

  [[nodiscard]] bool armed() const noexcept;

 private:
  // 消费升级任务 future；任务被取消/关闭清理时返回 nullopt。
  [[nodiscard]] std::optional<process::EscalationResult> consume_completion();

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
