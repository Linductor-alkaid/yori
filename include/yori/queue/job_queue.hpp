#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>
#include <yori/store/state_store.hpp>

namespace yori::queue {

struct QueueConfig final {
  static constexpr std::size_t kDefaultCapacity = 1024;
  static constexpr std::size_t kMaxCapacity = 4096;

  std::size_t capacity{kDefaultCapacity};
};

enum class QueueErrorCode {
  kNone,
  kInvalidCapacity,
  kInvalidSnapshotRevision,
  kInvalidJobId,
  kInvalidJobSpec,
  kInvalidJobState,
  kInvalidJobRevision,
  kJobNotQueued,
  kDuplicateJob,
  kCapacityExceeded,
  kJobNotFound,
};

[[nodiscard]] const char* to_string(QueueErrorCode code) noexcept;

enum class QueueEventKind {
  kNone,
  kAdmissionAccepted,
  kAdmissionRejected,
  kRemovalApplied,
  kRemovalRejected,
  kRestoreApplied,
  kRestoreRejected,
};

[[nodiscard]] const char* to_string(QueueEventKind kind) noexcept;

// 每次修改尝试都返回一个可由上层投递到观察通道的事件；队列本身不隐藏事件
// 缓冲、线程或回调执行上下文。
struct QueueEvent final {
  QueueEventKind kind{QueueEventKind::kNone};
  QueueErrorCode code{QueueErrorCode::kNone};
  std::optional<job::JobId> job_id;
  std::size_t size_before{0};
  std::size_t size_after{0};
  std::size_t capacity{0};
};

struct QueueEntry final {
  job::JobId job_id;
  std::chrono::system_clock::time_point submit_time{};
};

struct QueueOperationResult final {
  QueueErrorCode code{QueueErrorCode::kNone};
  QueueEvent event;
  std::optional<std::size_t> position;
  std::optional<std::size_t> item_index;
  job::JobSpecValidationResult spec_validation{};

  [[nodiscard]] constexpr bool ok() const noexcept { return code == QueueErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// 服务器级唯一的严格 FIFO 队列。该类是单 owner 的同步 Core 组件；Executor
// owner 负责在后续 JobManager/Scheduler 路径中串行调用它。
class GlobalJobQueue final {
 public:
  [[nodiscard]] static std::unique_ptr<GlobalJobQueue> create(QueueConfig config,
                                                              QueueErrorCode& error);

  GlobalJobQueue(const GlobalJobQueue&) = delete;
  GlobalJobQueue& operator=(const GlobalJobQueue&) = delete;
  GlobalJobQueue(GlobalJobQueue&&) = delete;
  GlobalJobQueue& operator=(GlobalJobQueue&&) = delete;
  ~GlobalJobQueue() = default;

  // 只接受有效的 QUEUED/revision=0 Job。排序键固定为
  // (submit_time, JobId)，与 DEC-005 一致。
  [[nodiscard]] QueueOperationResult admit(const store::StoredJob& record);

  // 调度或排队期取消在持久化状态转换成功后移除对应 Job。
  [[nodiscard]] QueueOperationResult remove(job::JobId id);

  // 从 StateStore 同一 revision 的快照原子重建派生队列；只恢复 QUEUED Job，
  // 其他合法状态不会进入队列。失败时保留原队列。
  [[nodiscard]] QueueOperationResult restore(const store::StateSnapshot& snapshot);

  [[nodiscard]] const std::vector<QueueEntry>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::optional<QueueEntry> front() const noexcept;
  [[nodiscard]] bool contains(job::JobId id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

 private:
  explicit GlobalJobQueue(std::size_t capacity);

  std::size_t capacity_;
  std::vector<QueueEntry> entries_;
};

}  // namespace yori::queue
