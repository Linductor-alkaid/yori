#include "runtime/grace_escalation.hpp"

#include <chrono>
#include <executor/executor.hpp>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

namespace yori::runtime {

class GraceEscalation::Impl final {
 public:
  Impl(executor::Executor& executor_ref, process::ProcessSupervisor& supervisor_ref)
      : executor(executor_ref), supervisor(supervisor_ref) {}

  executor::Executor& executor;
  process::ProcessSupervisor& supervisor;
  executor::TimerHandle timer;
  std::future<process::EscalationResult> completion;
  bool armed{false};
};

GraceEscalation::GraceEscalation(executor::Executor& executor,
                                 process::ProcessSupervisor& supervisor)
    : impl_(std::make_unique<Impl>(executor, supervisor)) {}

GraceEscalation::~GraceEscalation() { static_cast<void>(disarm()); }

GraceArmCode GraceEscalation::arm() {
  if (impl_->armed) {
    return GraceArmCode::kAlreadyArmed;
  }
  if (impl_->supervisor.phase() != process::ProcessSupervisor::Phase::kTerminating) {
    return GraceArmCode::kSupervisorNotTerminating;
  }
  const auto grace_ms =
      static_cast<std::int64_t>(impl_->supervisor.cancel_policy().grace_period.count());
  try {
    auto submission = impl_->executor.submit_delayed_with_handle(
        grace_ms, [supervisor = &impl_->supervisor]() { return supervisor->escalate(); });
    impl_->timer = std::move(submission.handle);
    impl_->completion = std::move(submission.future);
  } catch (const std::exception&) {
    return GraceArmCode::kExecutorRejected;
  } catch (...) {
    return GraceArmCode::kExecutorRejected;
  }
  impl_->armed = true;
  return GraceArmCode::kArmed;
}

GraceDisarmCode GraceEscalation::disarm() {
  if (!impl_->armed) {
    return GraceDisarmCode::kNotArmed;
  }
  const auto cancel_result = impl_->timer.cancel();
  if (cancel_result == executor::TimerOperationResult::CancelledBeforeDispatch) {
    static_cast<void>(consume_completion());
    impl_->armed = false;
    return GraceDisarmCode::kCancelledBeforeDispatch;
  }
  // 已派发/已完成/关闭清理：同步等待回调结束并消费 future，此后 supervisor 不再
  // 被该任务引用。
  static_cast<void>(consume_completion());
  impl_->armed = false;
  return GraceDisarmCode::kConsumedCompletion;
}

GraceConsumeResult GraceEscalation::try_consume() {
  if (!impl_->armed) {
    return {};
  }
  if (!impl_->completion.valid() ||
      impl_->completion.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
    return {GraceConsumeCode::kNotReady, std::nullopt};
  }
  GraceConsumeResult result;
  result.code = GraceConsumeCode::kConsumed;
  result.escalation = consume_completion();
  impl_->armed = false;
  return result;
}

bool GraceEscalation::armed() const noexcept { return impl_->armed; }

// 消费升级任务的 future。escalate() 本身 noexcept，异常只可能来自 Executor 生命周期
// （取消/关闭清理）；此时任务未执行，以 nullopt 区分"未执行"与"已执行"。
std::optional<process::EscalationResult> GraceEscalation::consume_completion() {
  if (!impl_->completion.valid()) {
    return std::nullopt;
  }
  try {
    return impl_->completion.get();
  } catch (const executor::TaskCancelled&) {
    return std::nullopt;
  } catch (const executor::ExecutorStopping&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace yori::runtime
