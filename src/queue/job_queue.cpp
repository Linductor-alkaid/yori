#include <algorithm>
#include <iterator>
#include <set>
#include <utility>
#include <yori/queue/job_queue.hpp>

namespace yori::queue {
namespace {

bool entry_less(const QueueEntry& lhs, const QueueEntry& rhs) noexcept {
  if (lhs.submit_time != rhs.submit_time) {
    return lhs.submit_time < rhs.submit_time;
  }
  return lhs.job_id < rhs.job_id;
}

bool known_state(job::JobState state) noexcept {
  switch (state) {
    case job::JobState::kQueued:
    case job::JobState::kStarting:
    case job::JobState::kRunning:
    case job::JobState::kStopping:
    case job::JobState::kFinished:
    case job::JobState::kFailed:
    case job::JobState::kCancelled:
    case job::JobState::kLost:
      return true;
  }
  return false;
}

QueueOperationResult result(QueueEventKind kind, QueueErrorCode code,
                            std::optional<job::JobId> job_id, std::size_t size_before,
                            std::size_t size_after, std::size_t capacity,
                            std::optional<std::size_t> position = std::nullopt,
                            std::optional<std::size_t> item_index = std::nullopt,
                            job::JobSpecValidationResult spec_validation = {}) {
  return {code,
          {kind, code, job_id, size_before, size_after, capacity},
          position,
          item_index,
          spec_validation};
}

}  // namespace

const char* to_string(QueueErrorCode code) noexcept {
  switch (code) {
    case QueueErrorCode::kNone:
      return "NONE";
    case QueueErrorCode::kInvalidCapacity:
      return "INVALID_CAPACITY";
    case QueueErrorCode::kInvalidSnapshotRevision:
      return "INVALID_SNAPSHOT_REVISION";
    case QueueErrorCode::kInvalidJobId:
      return "INVALID_JOB_ID";
    case QueueErrorCode::kInvalidJobSpec:
      return "INVALID_JOB_SPEC";
    case QueueErrorCode::kInvalidJobState:
      return "INVALID_JOB_STATE";
    case QueueErrorCode::kInvalidJobRevision:
      return "INVALID_JOB_REVISION";
    case QueueErrorCode::kJobNotQueued:
      return "JOB_NOT_QUEUED";
    case QueueErrorCode::kDuplicateJob:
      return "DUPLICATE_JOB";
    case QueueErrorCode::kCapacityExceeded:
      return "CAPACITY_EXCEEDED";
    case QueueErrorCode::kJobNotFound:
      return "JOB_NOT_FOUND";
  }
  return "UNKNOWN";
}

const char* to_string(QueueEventKind kind) noexcept {
  switch (kind) {
    case QueueEventKind::kNone:
      return "NONE";
    case QueueEventKind::kAdmissionAccepted:
      return "ADMISSION_ACCEPTED";
    case QueueEventKind::kAdmissionRejected:
      return "ADMISSION_REJECTED";
    case QueueEventKind::kRemovalApplied:
      return "REMOVAL_APPLIED";
    case QueueEventKind::kRemovalRejected:
      return "REMOVAL_REJECTED";
    case QueueEventKind::kRestoreApplied:
      return "RESTORE_APPLIED";
    case QueueEventKind::kRestoreRejected:
      return "RESTORE_REJECTED";
  }
  return "UNKNOWN";
}

std::unique_ptr<GlobalJobQueue> GlobalJobQueue::create(QueueConfig config, QueueErrorCode& error) {
  if (config.capacity == 0 || config.capacity > QueueConfig::kMaxCapacity) {
    error = QueueErrorCode::kInvalidCapacity;
    return nullptr;
  }

  auto queue = std::unique_ptr<GlobalJobQueue>(new GlobalJobQueue(config.capacity));
  error = QueueErrorCode::kNone;
  return queue;
}

GlobalJobQueue::GlobalJobQueue(std::size_t capacity) : capacity_(capacity) {
  entries_.reserve(capacity_);
}

QueueOperationResult GlobalJobQueue::admit(const store::StoredJob& record) {
  const auto size_before = entries_.size();
  const auto rejected = [this, &record, size_before](QueueErrorCode code,
                                                     job::JobSpecValidationResult validation = {}) {
    const auto id = record.id.valid() ? std::optional<job::JobId>{record.id} : std::nullopt;
    return result(QueueEventKind::kAdmissionRejected, code, id, size_before, size_before, capacity_,
                  std::nullopt, std::nullopt, validation);
  };

  if (!record.id.valid()) {
    return rejected(QueueErrorCode::kInvalidJobId);
  }
  const auto validation = job::validate(record.spec);
  if (!validation) {
    return rejected(QueueErrorCode::kInvalidJobSpec, validation);
  }
  if (!known_state(record.state)) {
    return rejected(QueueErrorCode::kInvalidJobState);
  }
  if (record.state != job::JobState::kQueued) {
    return rejected(QueueErrorCode::kJobNotQueued);
  }
  if (record.revision != 0) {
    return rejected(QueueErrorCode::kInvalidJobRevision);
  }
  if (contains(record.id)) {
    return rejected(QueueErrorCode::kDuplicateJob);
  }
  if (entries_.size() >= capacity_) {
    return rejected(QueueErrorCode::kCapacityExceeded);
  }

  const QueueEntry entry{record.id, record.spec.submit_time};
  const auto insertion = std::lower_bound(entries_.begin(), entries_.end(), entry, entry_less);
  const auto position = static_cast<std::size_t>(std::distance(entries_.begin(), insertion));
  entries_.insert(insertion, entry);
  return result(QueueEventKind::kAdmissionAccepted, QueueErrorCode::kNone, record.id, size_before,
                entries_.size(), capacity_, position);
}

QueueOperationResult GlobalJobQueue::remove(job::JobId id) {
  const auto size_before = entries_.size();
  if (!id.valid()) {
    return result(QueueEventKind::kRemovalRejected, QueueErrorCode::kInvalidJobId, std::nullopt,
                  size_before, size_before, capacity_);
  }

  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [id](const QueueEntry& entry) { return entry.job_id == id; });
  if (found == entries_.end()) {
    return result(QueueEventKind::kRemovalRejected, QueueErrorCode::kJobNotFound, id, size_before,
                  size_before, capacity_);
  }

  const auto position = static_cast<std::size_t>(std::distance(entries_.begin(), found));
  entries_.erase(found);
  return result(QueueEventKind::kRemovalApplied, QueueErrorCode::kNone, id, size_before,
                entries_.size(), capacity_, position);
}

QueueOperationResult GlobalJobQueue::restore(const store::StateSnapshot& snapshot) {
  const auto size_before = entries_.size();
  const auto rejected = [this, size_before](QueueErrorCode code,
                                            std::optional<job::JobId> id = std::nullopt,
                                            std::optional<std::size_t> item_index = std::nullopt,
                                            job::JobSpecValidationResult validation = {}) {
    return result(QueueEventKind::kRestoreRejected, code, id, size_before, size_before, capacity_,
                  std::nullopt, item_index, validation);
  };

  if (snapshot.revision == 0 && (!snapshot.jobs.empty() || !snapshot.leases.empty())) {
    return rejected(QueueErrorCode::kInvalidSnapshotRevision);
  }

  std::vector<QueueEntry> restored;
  restored.reserve(capacity_);
  std::set<job::JobId> seen;
  for (std::size_t index = 0; index < snapshot.jobs.size(); ++index) {
    const auto& record = snapshot.jobs[index];
    const auto id = record.id.valid() ? std::optional<job::JobId>{record.id} : std::nullopt;
    if (!record.id.valid()) {
      return rejected(QueueErrorCode::kInvalidJobId, std::nullopt, index);
    }
    const auto validation = job::validate(record.spec);
    if (!validation) {
      return rejected(QueueErrorCode::kInvalidJobSpec, id, index, validation);
    }
    if (!known_state(record.state)) {
      return rejected(QueueErrorCode::kInvalidJobState, id, index);
    }
    if ((record.state == job::JobState::kQueued && record.revision != 0) ||
        (record.state != job::JobState::kQueued && record.revision == 0)) {
      return rejected(QueueErrorCode::kInvalidJobRevision, id, index);
    }
    if (!seen.insert(record.id).second) {
      return rejected(QueueErrorCode::kDuplicateJob, id, index);
    }
    if (record.state != job::JobState::kQueued) {
      continue;
    }
    if (restored.size() >= capacity_) {
      return rejected(QueueErrorCode::kCapacityExceeded, id, index);
    }
    restored.push_back({record.id, record.spec.submit_time});
  }

  std::sort(restored.begin(), restored.end(), entry_less);
  entries_.swap(restored);
  return result(QueueEventKind::kRestoreApplied, QueueErrorCode::kNone, std::nullopt, size_before,
                entries_.size(), capacity_);
}

std::optional<QueueEntry> GlobalJobQueue::front() const noexcept {
  if (entries_.empty()) {
    return std::nullopt;
  }
  return entries_.front();
}

bool GlobalJobQueue::contains(job::JobId id) const noexcept {
  return std::any_of(entries_.begin(), entries_.end(),
                     [id](const QueueEntry& entry) { return entry.job_id == id; });
}

}  // namespace yori::queue
