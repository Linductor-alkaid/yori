#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <yori/ipc/ipc_transport.hpp>

namespace executor {
class Executor;
}

namespace yori::runtime {

// UDS 服务端配置（DEC-010）。apply_ownership 为 true 时对 socket 执行
// fchown(owner_uid, owner_gid)；非 root 进程无法 chown 将显式启动失败。
struct UdsIpcServerConfig final {
  static constexpr std::size_t kMaxSocketPathBytes = 107;  // sockaddr_un::sun_path - 1

  std::string socket_path;
  std::uint32_t socket_owner_uid{0};
  std::uint32_t socket_owner_gid{0};
  bool apply_ownership{false};
  // 生产默认 0660（root:yori）；测试/本地以更紧的模式运行。
  std::uint32_t socket_mode{0660};
  // 每连接请求总预算（读 + 处理 + 写）；超时断开（慢客户端有界化）。
  std::chrono::milliseconds request_deadline{5000};

  [[nodiscard]] bool valid(std::string& error) const noexcept;
};

// IPC 连接接受与请求读取的 Executor 承载（总计划 EXEC-02）：单个 blocking
// worker 以 poll 等待 listen fd 与唤醒管道，串行 accept -> SO_PEERCRED ->
// 读一帧 -> 调用 handler -> 写回一帧 -> 关闭。请求/响应均为一次性有界帧
//（流式会话 M6 承载）。停止 = 停止任务生产者语义（EXEC-10 ①）：唤醒、join、
// 关闭 listen fd 并清理端点；已在处理中的请求以断开收尾。
//
// owner 纪律：start/stop 由非 worker 的单一 owner 调用；handler 在 worker
// 线程内被串行调用（单线程进入 IpcService）。停止后不可重启。
class UdsIpcServer final : public ipc::IpcServerTransport {
 public:
  UdsIpcServer(executor::Executor& executor, ipc::IpcRequestHandler& handler,
               UdsIpcServerConfig config);
  ~UdsIpcServer() override;

  UdsIpcServer(const UdsIpcServer&) = delete;
  UdsIpcServer& operator=(const UdsIpcServer&) = delete;
  UdsIpcServer(UdsIpcServer&&) = delete;
  UdsIpcServer& operator=(UdsIpcServer&&) = delete;

  [[nodiscard]] ipc::IpcTransportStartResult start() override;
  void stop() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
