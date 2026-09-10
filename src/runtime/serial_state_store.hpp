#pragma once

#include <memory>
#include <mutex>
#include <yori/store/state_store.hpp>

namespace yori::runtime {

// ---------------------------------------------------------------------------
// StateStore 的所有权串行化包装（M7 守护总装）。
//
// 两个后端（InMemoryStateStore/SqliteStateStore）都是"单 owner、无内部锁"
// 契约（DEC-009 第 5 条）。守护总装后，store 出现两个访问上下文：IPC worker
// 的同步读（ps/queue/gpu/logs）与 JobManager worker 的写（经 StoreTaskRunner
// 在 Executor 任务线程执行 apply）。本包装以一把互斥量串行化全部 load/apply
// 调用——与 LogStreamer 注册表同语义：mutex 是单 owner 资源的生命周期所有权
// 保护，不是通信通道的替代；写路径的语义串行（单在飞、revision 组合）仍由
// StoreTaskRunner/JobManager 承载，本类不重排、不缓存、不吞错误。
//
// owner 纪律：随 daemon 主生命周期构造；包装对象与被包装 store 的生命周期
// 由同一 owner 保证。逻辑一致性（expected_revision CAS）仍由调用方组合，
// 冲突显式返回 kRevisionConflict。
// ---------------------------------------------------------------------------
class SerialStateStore final : public store::StateStore {
 public:
  explicit SerialStateStore(std::unique_ptr<store::StateStore> inner);

  SerialStateStore(const SerialStateStore&) = delete;
  SerialStateStore& operator=(const SerialStateStore&) = delete;
  SerialStateStore(SerialStateStore&&) = delete;
  SerialStateStore& operator=(SerialStateStore&&) = delete;
  ~SerialStateStore() override = default;

  [[nodiscard]] store::StateStoreLoadResult load() override;
  [[nodiscard]] store::StateStoreWriteResult apply(const store::StateMutation& mutation) override;

  // 统计（测试与观测）：被串行化的调用次数。
  [[nodiscard]] std::uint64_t serialized_calls() const noexcept;

 private:
  std::unique_ptr<store::StateStore> inner_;
  mutable std::mutex mutex_;
  std::uint64_t serialized_calls_{0};
};

}  // namespace yori::runtime
