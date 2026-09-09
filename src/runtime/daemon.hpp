#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_service.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/recovery/job_recovery.hpp>
#include <yori/store/state_store.hpp>

#include "gpu_manager.hpp"
#include "ipc_server.hpp"

namespace executor {
class Executor;
}

namespace yori::runtime {

struct DaemonConfig final {
  ipc::IpcServiceConfig service;
  UdsIpcServerConfig ipc;
  queue::QueueConfig queue;
  GpuManagerConfig gpu;
};

enum class DaemonStartCode {
  kStarted,
  kAlreadyStarted,
  kInvalidConfig,
  kRecoveryFailed,
  kGpuFailed,
  kIpcFailed,
};

[[nodiscard]] const char* to_string(DaemonStartCode code) noexcept;

struct DaemonStartResult final {
  DaemonStartCode code{DaemonStartCode::kInvalidConfig};
  std::string message;
  // 启动恢复的逐 Job 决策（审计；失败时保留已完成部分）。
  std::optional<recovery::RecoveryResult> recovery;

  [[nodiscard]] bool ok() const noexcept { return code == DaemonStartCode::kStarted; }
};

enum class DaemonStopCode {
  kStopped,
  kNotRunning,
  kAlreadyStopped,
};

[[nodiscard]] const char* to_string(DaemonStopCode code) noexcept;

// M5 子集的 daemon 总装：启动序 = 恢复（JobRecovery，同步有界，RULE-06）
// -> GPU 观察（GpuManager，EXEC-05/09）-> IPC 服务（UdsIpcServer，EXEC-02）；
// 停止序为 EXEC-10 的适用子集 = ① IPC -> ③ GPU 周期任务。Executor owner 是
// 进程主生命周期（yorid main / 集成测试），本类不做 executor 初始化或关闭。
// 调度触发与进程守护的总装（EXEC-06/07 后半）随守护总装收口接入；M5 daemon
// 不启动训练进程，Job 停留在 QUEUED。
//
// owner 纪律：单 owner 调用 start/stop；stop 后不可重启；析构以 stop 兜底。
class Daemon final {
 public:
  Daemon(executor::Executor& executor, gpu::GpuProvider& gpu_provider, store::StateStore& store,
         DaemonConfig config = {});
  ~Daemon();

  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;
  Daemon(Daemon&&) = delete;
  Daemon& operator=(Daemon&&) = delete;

  [[nodiscard]] DaemonStartResult start();
  [[nodiscard]] DaemonStopCode stop();

  [[nodiscard]] const std::optional<recovery::RecoveryResult>& last_recovery() const noexcept;

 private:
  // GpuStatusSource 的 GpuManager 投影（EXEC-09 DoubleBuffer 读取）。
  class ManagerGpuStatusSource final : public ipc::GpuStatusSource {
   public:
    explicit ManagerGpuStatusSource(GpuManager& manager) : manager_(manager) {}

    [[nodiscard]] bool try_get_snapshot(gpu::GpuObservationSnapshot& out) override {
      return manager_.try_get_snapshot(out);
    }

   private:
    GpuManager& manager_;
  };

  executor::Executor& executor_;
  gpu::GpuProvider& gpu_provider_;
  store::StateStore& store_;
  DaemonConfig config_;

  std::unique_ptr<queue::GlobalJobQueue> queue_;
  std::unique_ptr<GpuManager> gpu_manager_;
  std::unique_ptr<ManagerGpuStatusSource> gpu_status_;
  std::unique_ptr<ipc::LogSnapshotReader> log_reader_;
  std::unique_ptr<ipc::IpcService> service_;
  std::unique_ptr<UdsIpcServer> ipc_server_;
  std::optional<recovery::RecoveryResult> last_recovery_;
  bool started_{false};
  bool stop_requested_{false};
};

}  // namespace yori::runtime
