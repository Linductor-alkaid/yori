#pragma once

#include <cstdint>
#include <optional>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/store/state_store.hpp>

namespace yori::scheduler {

enum class SchedulerTrigger {
  kJobSubmitted,
  kJobExited,
  kJobCancelled,
  kGpuStateChanged,
  kRecoveryCompleted,
  kAdminStateChanged,
};

[[nodiscard]] const char* to_string(SchedulerTrigger trigger) noexcept;

enum class ScheduleResultCode {
  kScheduled,
  kQueueEmpty,
  kHeadBlocked,
  kCancelled,
  kInvalidGpuSnapshot,
  kStateLoadFailed,
  kQueueStateDiverged,
  kStateWriteFailed,
  kQueueRollbackFailed,
};

[[nodiscard]] const char* to_string(ScheduleResultCode code) noexcept;

struct SchedulerEvent final {
  SchedulerTrigger trigger{SchedulerTrigger::kJobSubmitted};
  ScheduleResultCode code{ScheduleResultCode::kQueueEmpty};
  std::optional<job::JobId> job_id;
  std::optional<gpu::GpuUuid> gpu_uuid;
  gpu::GpuObservationValidationResult gpu_validation{};
  store::StateStoreErrorCode store_error{store::StateStoreErrorCode::kNone};
  queue::QueueErrorCode queue_error{queue::QueueErrorCode::kNone};
  std::uint64_t store_revision{0};
};

struct ScheduleResult final {
  ScheduleResultCode code{ScheduleResultCode::kQueueEmpty};
  SchedulerEvent event;

  [[nodiscard]] constexpr bool scheduled() const noexcept {
    return code == ScheduleResultCode::kScheduled;
  }
  [[nodiscard]] constexpr bool failed() const noexcept {
    return code == ScheduleResultCode::kInvalidGpuSnapshot ||
           code == ScheduleResultCode::kStateLoadFailed ||
           code == ScheduleResultCode::kQueueStateDiverged ||
           code == ScheduleResultCode::kStateWriteFailed ||
           code == ScheduleResultCode::kQueueRollbackFailed;
  }
};

// 单 owner、事件驱动的严格 FIFO 调度 Core。一次调用只处理一个触发事件，且
// 最多把一个队首 Job 原子推进到 STARTING 并建立一个 GPU lease。
class FifoScheduler final {
 public:
  FifoScheduler(queue::GlobalJobQueue& queue, store::StateStore& store)
      : queue_(queue), store_(store) {}

  [[nodiscard]] ScheduleResult run_once(SchedulerTrigger trigger,
                                        const gpu::GpuObservationSnapshot& gpu_snapshot);

  // Executor adapter 在任务开始前观察到 StopToken 后使用此结果；取消不会触碰
  // Job 状态、队列或 lease。
  [[nodiscard]] static ScheduleResult cancelled(SchedulerTrigger trigger) noexcept;

 private:
  queue::GlobalJobQueue& queue_;
  store::StateStore& store_;
};

}  // namespace yori::scheduler
