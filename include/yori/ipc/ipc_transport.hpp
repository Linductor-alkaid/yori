#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <yori/ipc/ipc_protocol.hpp>

namespace yori::ipc {

// 连接对端身份（SO_PEERCRED 的 Core 投影，RULE-02：公开契约不暴露平台类型）。
// uid 是唯一参与授权的稳定事实（0 即 root，是合法身份）；gid 参与 DEC-010 的
// admin 主 GID 匹配；pid 仅作诊断，不参与授权。获取失败时由传输层断开连接，
// 不以零值进入 handler。
struct PeerCredentials final {
  std::uint32_t uid{0};
  std::uint32_t gid{0};
  std::uint32_t pid{0};
};

// daemon 侧请求处理入口。实现（IpcService）为单 owner 同步 Core；在 UDS 适配
// 的 blocking worker 线程内被串行调用，不得阻塞无界或自建并发。异常必须被
// 实现内部处理；逸出的异常由传输层映射为 kInternal 响应（不吞、不崩）。
class IpcRequestHandler {
 public:
  virtual ~IpcRequestHandler() = default;

  [[nodiscard]] virtual IpcResponse handle(const PeerCredentials& peer,
                                           const IpcRequest& request) = 0;
};

enum class IpcTransportStartCode {
  kStarted,
  kAlreadyStarted,
  kInvalidConfig,
  kEndpointRejected,
  kExecutorRejected,
};

[[nodiscard]] const char* to_string(IpcTransportStartCode code) noexcept;

struct IpcTransportStartResult final {
  IpcTransportStartCode code{IpcTransportStartCode::kInvalidConfig};
  std::string message;

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == IpcTransportStartCode::kStarted;
  }
};

// 服务端传输抽象：接受连接、读取请求帧、调用 handler、写回响应帧。UDS 适配
// 实现（EXEC-02 blocking worker 承载）在 Adapter 层；Core/测试可注入假实现。
class IpcServerTransport {
 public:
  virtual ~IpcServerTransport() = default;

  [[nodiscard]] virtual IpcTransportStartResult start() = 0;
  // 幂等：请求停止 -> 唤醒阻塞等待 -> join -> 清理端点。已在处理中的请求以
  // 显式失败（EOF）收尾，不静默丢弃已收请求。
  virtual void stop() = 0;
};

enum class IpcClientError {
  kNone,
  kConnectFailed,
  kTimeout,
  kClosed,
  kProtocol,
};

[[nodiscard]] const char* to_string(IpcClientError error) noexcept;

struct IpcClientResult final {
  IpcClientError error{IpcClientError::kNone};
  IpcResponse response{};

  [[nodiscard]] constexpr bool ok() const noexcept { return error == IpcClientError::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// 客户端传输抽象：一次连接内发送单请求并等待单响应（M5 请求/响应语义；
// M6 流式会话另行扩展）。实现同步有界，无隐藏线程。
class IpcClientTransport {
 public:
  virtual ~IpcClientTransport() = default;

  [[nodiscard]] virtual IpcClientResult call(const std::string& endpoint, const IpcRequest& request,
                                             std::chrono::milliseconds timeout) = 0;
};

}  // namespace yori::ipc
