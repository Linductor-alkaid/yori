#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <yori/ipc/ipc_protocol.hpp>
#include <yori/ipc/uds_client.hpp>

#include "yori_test.hpp"

// M7 后续修复（真机问题三）：connect(2) 的 EACCES 与"连不上"此前映射为同一
// 错误（"connect failed"），误导排查——用户会话缺端点属组身份（DEC-010 的
// root:yori 0660 准入，usermod 后未重新登录）是最常见的 EACCES 来源。
// 客户端现区分 kPermissionDenied 并在消息中提示重新登录/newgrp。
//
// 非 root 验证方式：以本人身份起 0600 端点后 chmod 0000，connect 因无写
// 权限返回 EACCES。
namespace {

using namespace std::chrono_literals;
using yori::ipc::IpcClientError;
using yori::ipc::IpcRequest;
using yori::ipc::IpcRequestKind;
using yori::ipc::UdsIpcClient;

}  // namespace

int main() {
  // 独立监听 socket：手工 bind 一个 unix socket 作为连接目标（不需要完整
  // 服务器——客户端在 connect 阶段即返回）。
  const std::string path = "/tmp/yori-client-perm-test.sock";
  static_cast<void>(::unlink(path.c_str()));

  const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  YORI_CHECK(listen_fd >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  path.copy(address.sun_path, path.size());
  address.sun_path[path.size()] = '\0';
  YORI_CHECK(::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == 0);
  YORI_CHECK(::listen(listen_fd, 4) == 0);
  YORI_CHECK(::chmod(path.c_str(), 0600) == 0);

  // 有权限（本人 0600）：连接成功（请求级错误不在此断言，仅排除权限路径）。
  {
    IpcRequest request;
    request.kind = IpcRequestKind::kPs;
    UdsIpcClient client;
    const auto result = client.call(path, request, 2000ms);
    YORI_CHECK(result.error != IpcClientError::kPermissionDenied);
    YORI_CHECK(result.error != IpcClientError::kConnectFailed);
  }

  // 无权限（chmod 0000，本人也失去写位 -> EACCES）：显式 permission denied。
  YORI_CHECK(::chmod(path.c_str(), 0000) == 0);
  {
    IpcRequest request;
    request.kind = IpcRequestKind::kPs;
    UdsIpcClient client;
    const auto result = client.call(path, request, 2000ms);
    YORI_CHECK(result.error == IpcClientError::kPermissionDenied);
    const std::string message = yori::ipc::to_string(result.error);
    YORI_CHECK(message.find("permission denied") != std::string::npos);
    YORI_CHECK(message.find("newgrp") != std::string::npos);
  }

  static_cast<void>(::close(listen_fd));
  static_cast<void>(::unlink(path.c_str()));

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "uds client permission: %d failure(s)\n",
                 yori::testing::failure_count);
    return 1;
  }
  std::printf("uds client permission: all checks passed\n");
  return 0;
}
