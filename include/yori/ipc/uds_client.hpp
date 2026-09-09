#pragma once

#include <chrono>
#include <string>
#include <yori/ipc/ipc_transport.hpp>

namespace yori::ipc {

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
};

}  // namespace yori::ipc
