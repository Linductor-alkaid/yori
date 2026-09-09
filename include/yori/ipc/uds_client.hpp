#pragma once

#include <chrono>
#include <string>
#include <yori/ipc/ipc_transport.hpp>

namespace yori::ipc {

// 流式帧消费回调（M6）：返回 false 表示客户端主动停止读取（连接随后关闭）。
class IpcStreamFrameHandler {
 public:
  virtual ~IpcStreamFrameHandler() = default;

  [[nodiscard]] virtual bool on_frame(const IpcStreamFrame& frame) = 0;
};

// logs -f 的客户端结果：ack 为初始响应；end_frame 携带结束传输的控制帧
// （EOF/BACKPRESSURE）或空 kind（客户端停止/传输错误）。
struct IpcFollowResult final {
  IpcClientError error{IpcClientError::kNone};
  IpcResponse ack{};

  [[nodiscard]] constexpr bool ok() const noexcept { return error == IpcClientError::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// Unix Domain Socket 客户端适配（Adapter 层，DEC-010）：一次连接内发送单
// 请求帧并等待单响应帧。同步有界（poll + 截止时间），不创建线程或 Executor；
// `yori` CLI 的无状态客户端直接使用。
class UdsIpcClient final : public IpcClientTransport {
 public:
  UdsIpcClient() = default;
  ~UdsIpcClient() override = default;

  UdsIpcClient(const UdsIpcClient&) = delete;
  UdsIpcClient& operator=(const UdsIpcClient&) = delete;
  UdsIpcClient(UdsIpcClient&&) = delete;
  UdsIpcClient& operator=(UdsIpcClient&&) = delete;

  // endpoint 为 socket 文件系统路径（绝对路径）；timeout 覆盖连接、发送与
  // 等待响应的总预算。
  [[nodiscard]] IpcClientResult call(const std::string& endpoint, const IpcRequest& request,
                                     std::chrono::milliseconds timeout) override;

  // 流式跟随（M6）：连接 + 发送请求 + 读取初始 ack 在 setup_timeout 预算内；
  // 此后循环读取流式帧并回调。frame_timeout <= 0 表示无限等待（跟随安静流），
  // 否则为单帧预算。EOF/BACKPRESSURE 帧或回调返回 false 后正常结束（kNone）；
  // 对端在终止帧前关闭、超时或协议违规按 IpcClientError 返回。
  [[nodiscard]] IpcFollowResult follow(const std::string& endpoint, const IpcRequest& request,
                                       std::chrono::milliseconds setup_timeout,
                                       std::chrono::milliseconds frame_timeout,
                                       IpcStreamFrameHandler& handler);
};

}  // namespace yori::ipc
