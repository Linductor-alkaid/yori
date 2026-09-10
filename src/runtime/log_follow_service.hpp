#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <yori/ipc/ipc_service.hpp>
#include <yori/ipc/ipc_transport.hpp>

#include "ipc_server.hpp"
#include "log_streamer.hpp"

namespace executor {
class Executor;
}

namespace yori::runtime {

// ---------------------------------------------------------------------------
// LogFollowService（EXEC-03，设计 11.3-11.4）：logs -f 会话的 Executor 承载
// 与 UDS 流式委派实现。
//
// 单个 blocking worker 以 poll 服务全部跟随会话（会话 fd + 命令唤醒管道）：
// 初始 ack 帧 -> 回放（GAP 校正）-> 订阅排空（offset 去重）-> EOF 关闭。
// 每会话发送缓冲与订阅队列均有界；socket 写带截止时间。慢客户端以订阅者
// 侧 offset 间断检出，回 BACKPRESSURE 帧后断开（不静默丢弃）。
//
// owner 纪律：start/stop 由 daemon 主生命周期（非 worker）调用；停止时
// 断开全部会话（客户端见 EOF），幂等且不可重启。
// ---------------------------------------------------------------------------

struct LogFollowServiceConfig final {
  // 单帧 socket 写预算；超时会话被回收（慢客户端有界化）。
  std::chrono::milliseconds write_deadline{2000};
  // 每会话待写流式帧缓冲上限（字节）；超出即停止排空订阅，交由订阅队列
  // 溢出路径（BACKPRESSURE）处理。
  std::size_t max_session_buffer_bytes{2 * 1024 * 1024};
  // 单会话每轮 poll 最多排空的 chunk 数（公平性：多会话共享一个 worker）。
  std::uint32_t max_drain_chunks_per_cycle{32};

  [[nodiscard]] bool valid(std::string& error) const noexcept;
};

enum class LogFollowStartCode {
  kStarted,
  kAlreadyStarted,
  kWorkerRejected,
};

[[nodiscard]] const char* to_string(LogFollowStartCode code) noexcept;

struct LogFollowStartResult final {
  LogFollowStartCode code{LogFollowStartCode::kWorkerRejected};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == LogFollowStartCode::kStarted; }
};

struct LogFollowStatistics final {
  std::uint64_t sessions_started{0};
  std::uint64_t sessions_completed{0};     // EOF 正常收尾
  std::uint64_t sessions_disconnected{0};  // 对端断开/写失败
  std::uint64_t sessions_backpressure{0};  // BACKPRESSURE 断开
  std::uint64_t frames_sent{0};
  std::uint64_t active_sessions{0};
};

class LogFollowService final : public UdsIpcStreamDelegate {
 public:
  LogFollowService(executor::Executor& executor, ipc::IpcService& service, LogStreamer& streamer,
                   LogFollowServiceConfig config = {});
  ~LogFollowService() override;

  LogFollowService(const LogFollowService&) = delete;
  LogFollowService& operator=(const LogFollowService&) = delete;
  LogFollowService(LogFollowService&&) = delete;
  LogFollowService& operator=(LogFollowService&&) = delete;

  [[nodiscard]] LogFollowStartResult start();
  // 幂等：请求停止 -> 断开全部会话（EOF）-> 唤醒并 join worker。
  void stop();

  // 唤醒会话 worker（LogStreamer 变更监听的目标）：Topic 无 fd 可 poll，
  // 新 chunk/终态经此显式唤醒。非阻塞、线程安全。
  void notify() noexcept;

  [[nodiscard]] LogFollowStatistics statistics() const;

  [[nodiscard]] Outcome begin_stream(const ipc::PeerCredentials& peer,
                                     const ipc::IpcRequest& request, int client_fd) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::runtime
