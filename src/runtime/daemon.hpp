#pragma once

#include <executor/comm/phase_gate.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_service.hpp>
#include <yori/launch/launch_adapter.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/recovery/job_recovery.hpp>
#include <yori/store/state_store.hpp>

#include "gpu_manager.hpp"
#include "ipc_server.hpp"
#include "job_manager.hpp"
#include "log_follow_service.hpp"
#include "log_streamer.hpp"
#include "serial_state_store.hpp"

namespace executor {
class Executor;
}

namespace yori::runtime {

struct DaemonConfig final {
  ipc::IpcServiceConfig service;
  UdsIpcServerConfig ipc;
  queue::QueueConfig queue;
  GpuManagerConfig gpu;
  // M6 观察面：跟随会话与订阅分发。
  LogStreamerConfig log_streamer;
  LogFollowServiceConfig log_follow;
  // M7 守护总装：JobManager（取消宽限、日志根目录、daemon 环境快照）。
  JobManagerConfig job_manager;
};

enum class DaemonStartCode {
  kStarted,
  kAlreadyStarted,
  kInvalidConfig,
  kRecoveryFailed,
  kGpuFailed,
  kJobManagerFailed,
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

// daemon 全量总装（M7 守护总装收口）。
//
// 启动序（EXEC-09 PhaseGate：恢复 -> GPU 观察 -> 调度开启）：
//   1) 恢复（JobRecovery，同步有界，RULE-06；store 经 SerialStateStore 串行化）
//      -> gate kPhaseRecovery；
//   2) GPU 观察（GpuManager，EXEC-05/09）-> gate kPhaseGpuObserved；
//   3) 观察面（LogStreamer + LogFollowService，EXEC-03/04，M6）；
//   4) 守护承载（JobManager：恢复采纳 + 调度触发 + 进程守护，EXEC-06/07/08）
//      -> gate kPhaseSchedulingOpen（此后调度事件才被消费）；
//   5) IPC 服务（UdsIpcServer，EXEC-02）。
//
// 停止序为 EXEC-10 完整顺序：① IPC -> ② 跟随会话 -> ③ GPU 周期任务 ->
// ④ 守护承载（停止调度生产者；运行中训练进程 abandon，RULE-10，不终止）->
// ⑤ 退出监视/日志泵回收 -> ⑦ 终态与 lease 落盘（JobManager worker 串行
// 路径内完成）。Executor owner 是进程主生命周期（yorid main / 集成测试），
// 本类不做 executor 初始化或关闭。
//
// owner 纪律：单 owner 调用 start/stop；stop 后不可重启；析构以 stop 兜底。
class Daemon final {
 public:
  // store 以所有权移交（由 SerialStateStore 包装；IPC 读与 JobManager 写经
  // 其互斥串行化，满足后端单 owner 契约）。
  Daemon(executor::Executor& executor, gpu::GpuProvider& gpu_provider,
         std::unique_ptr<store::StateStore> store, DaemonConfig config = {});
  ~Daemon();
  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;
  Daemon(Daemon&&) = delete;
  Daemon& operator=(Daemon&&) = delete;

  [[nodiscard]] DaemonStartResult start();
  [[nodiscard]] DaemonStopCode stop();

  [[nodiscard]] const std::optional<recovery::RecoveryResult>& last_recovery() const noexcept;

  // 观察面与守护承载访问口（测试与总装）。
  [[nodiscard]] LogStreamer& log_streamer() noexcept;
  [[nodiscard]] LogFollowStatistics log_follow_statistics() const;
  [[nodiscard]] JobManagerStats job_manager_stats() const;
  // 提交/取消的守护委派入口（IPC 服务与测试共用同一入口）。
  [[nodiscard]] ipc::JobControl& job_control() noexcept { return *job_manager_; }

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
  std::unique_ptr<store::StateStore> store_;
  DaemonConfig config_;

  launch::PosixIdentityResolver identity_resolver_;
  launch::DefaultLaunchAdapter launch_adapter_;
  executor::comm::PhaseGate startup_gate_{"yori-daemon-startup"};

  std::unique_ptr<queue::GlobalJobQueue> queue_;
  std::unique_ptr<GpuManager> gpu_manager_;
  std::unique_ptr<ManagerGpuStatusSource> gpu_status_;
  std::unique_ptr<ipc::LogSnapshotReader> log_reader_;
  std::unique_ptr<LogStreamer> log_streamer_;
  std::unique_ptr<LogFollowService> log_follow_;
  std::unique_ptr<JobManager> job_manager_;
  std::unique_ptr<ipc::IpcService> service_;
  std::unique_ptr<UdsIpcServer> ipc_server_;
  std::optional<recovery::RecoveryResult> last_recovery_;
  bool started_{false};
  bool stop_requested_{false};
};

}  // namespace yori::runtime
