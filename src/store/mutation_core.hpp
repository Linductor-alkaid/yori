#pragma once

// StateStore mutation 验证核心（M4-01）：内存实现与 SQLite adapter 共享，保证
// 两个后端对同一 mutation 产生完全相同的接受/拒绝决策（设计第 12 节）。内部
// 头，不安装、不得被公共头包含。

#include <cstddef>
#include <cstdint>
#include <map>
#include <yori/store/state_store.hpp>

namespace yori::store::mutation_core {

struct ValidationConfig final {
  std::size_t max_jobs{1024};
  std::size_t max_leases{128};
};

struct MutationOutcome final {
  StateStoreErrorCode code{StateStoreErrorCode::kNone};
  std::uint64_t next_revision{0};
  std::map<job::JobId, StoredJob> next_jobs{};
  std::map<gpu::GpuUuid, gpu::GpuLease> next_leases{};

  [[nodiscard]] bool ok() const noexcept { return code == StateStoreErrorCode::kNone; }
};

// 在当前状态（jobs/leases/current_revision）上验证并推进一个 mutation：副本
// 校验全部通过后一次性交出写后状态与新 revision（恰加一）。任何条目无效、
// revision 不匹配或容量耗尽都返回稳定错误码且 next_* 为空，调用方保持原状态
// 不变。后端失败注入与持久化由调用方负责；本函数不触碰 I/O。
[[nodiscard]] MutationOutcome apply_mutation(const std::map<job::JobId, StoredJob>& jobs,
                                             const std::map<gpu::GpuUuid, gpu::GpuLease>& leases,
                                             std::uint64_t current_revision,
                                             const ValidationConfig& config,
                                             const StateMutation& mutation);

}  // namespace yori::store::mutation_core
