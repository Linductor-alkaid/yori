#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <yori/ipc/uds_client.hpp>

#include "runtime/executor_runtime.hpp"
#include "runtime/ipc_server.hpp"
#include "yori_test.hpp"

namespace {

using namespace std::chrono_literals;
using yori::ipc::IpcError;
using yori::ipc::IpcRequest;
using yori::ipc::IpcRequestKind;
using yori::ipc::IpcResponse;
using yori::ipc::PeerCredentials;
using yori::runtime::UdsIpcServer;
using yori::runtime::UdsIpcServerConfig;

std::string make_directory() {
  char pattern[] = "/tmp/yori-ipc-server-test-XXXXXX";
  char* directory = ::mkdtemp(pattern);
  YORI_CHECK(directory != nullptr);
  return directory;
}

// echo handler：回显请求 kind；可选抛异常或记录 peer。
class EchoHandler final : public yori::ipc::IpcRequestHandler {
 public:
  IpcResponse handle(const PeerCredentials& peer, const IpcRequest& request) override {
    last_peer = peer;
    if (throw_on_handle) {
      throw std::runtime_error("boom");
    }
    IpcResponse response;
    response.kind = request.kind;
    response.error = IpcError::kNone;
    response.detail = "echo";
    return response;
  }

  PeerCredentials last_peer{};
  bool throw_on_handle{false};
};

bool path_exists(const std::string& path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0;
}

bool path_is_socket(const std::string& path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode);
}

// 裸连接 + 原始帧写入（负向路径不走 UdsIpcClient）。
class RawConnection final {
 public:
  explicit RawConnection(const std::string& path) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      return;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    connected_ = ::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
  }

  ~RawConnection() {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
  }

  RawConnection(const RawConnection&) = delete;
  RawConnection& operator=(const RawConnection&) = delete;

  [[nodiscard]] bool connected() const noexcept { return connected_; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

  bool send_all(const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
      const ssize_t n = ::send(fd_, data + sent, size - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  // 读取至多 size 字节；返回 0 表示 EOF。
  std::size_t receive(std::uint8_t* out, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
      const ssize_t n = ::recv(fd_, out + received, size - received, 0);
      if (n == 0) {
        break;
      }
      if (n < 0) {
        break;
      }
      received += static_cast<std::size_t>(n);
    }
    return received;
  }

 private:
  int fd_{-1};
  bool connected_{false};
};

std::uint32_t frame_length(const std::vector<std::uint8_t>& frame, std::size_t offset = 0) {
  return static_cast<std::uint32_t>(frame[offset]) |
         (static_cast<std::uint32_t>(frame[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(frame[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(frame[offset + 3]) << 24);
}

UdsIpcServerConfig base_config(const std::string& path) {
  UdsIpcServerConfig config;
  config.socket_path = path;
  config.socket_mode = 0600;
  config.request_deadline = 300ms;
  return config;
}

void test_lifecycle_and_permissions() {
  const std::string directory = make_directory();
  const std::string socket_path = directory + "/yori.sock";

  yori::runtime::ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(yori::runtime::ExecutorRuntimeConfig{}, error));
  EchoHandler handler;

  UdsIpcServerConfig config = base_config(socket_path);
  UdsIpcServer server(runtime.executor(), handler, config);
  YORI_CHECK(server.start().ok());

  // 重复启动拒绝。
  const auto second = server.start();
  YORI_CHECK(second.code == yori::ipc::IpcTransportStartCode::kAlreadyStarted);

  // 权限收敛（DEC-010）：模式生效、属主为当前用户（非 root 不 chown）。
  struct stat status {};
  YORI_CHECK(::stat(socket_path.c_str(), &status) == 0);
  YORI_CHECK(S_ISSOCK(status.st_mode));
  YORI_CHECK((status.st_mode & 0777) == 0600);
  YORI_CHECK(status.st_uid == ::geteuid());

  // 停止：幂等、端点清理、不可重启。
  server.stop();
  server.stop();
  YORI_CHECK(!path_exists(socket_path));
  const auto restart = server.start();
  YORI_CHECK(restart.code == yori::ipc::IpcTransportStartCode::kInvalidConfig);

  // EXEC-10 顺序下，worker join 后 shutdown 正常。
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_stale_endpoint_rules() {
  const std::string directory = make_directory();
  yori::runtime::ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(yori::runtime::ExecutorRuntimeConfig{}, error));
  EchoHandler handler;

  // 非 socket 文件占用端点：显式拒绝，不删除任意文件。
  const std::string file_path = directory + "/occupied";
  FILE* file = std::fopen(file_path.c_str(), "w");
  YORI_CHECK(file != nullptr);
  std::fputs("data", file);
  std::fclose(file);

  UdsIpcServer server(runtime.executor(), handler, base_config(file_path));
  const auto rejected = server.start();
  YORI_CHECK(rejected.code == yori::ipc::IpcTransportStartCode::kEndpointRejected);
  YORI_CHECK(path_exists(file_path));

  // 陈旧 socket 文件：替换后正常服务。
  const std::string socket_path = directory + "/yori.sock";
  const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
  YORI_CHECK(stale >= 0);
  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  YORI_CHECK(::bind(stale, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
  static_cast<void>(::close(stale));
  YORI_CHECK(path_is_socket(socket_path));

  UdsIpcServer replacement(runtime.executor(), handler, base_config(socket_path));
  YORI_CHECK(replacement.start().ok());
  replacement.stop();
  YORI_CHECK(!path_exists(socket_path));

  // 无效配置：相对路径 / 坏模式 / 非正 deadline。
  UdsIpcServerConfig relative = base_config("relative.sock");
  UdsIpcServer bad_path(runtime.executor(), handler, relative);
  YORI_CHECK(bad_path.start().code == yori::ipc::IpcTransportStartCode::kInvalidConfig);

  UdsIpcServerConfig bad_mode = base_config(socket_path);
  bad_mode.socket_mode = 07777;
  UdsIpcServer mode_server(runtime.executor(), handler, bad_mode);
  YORI_CHECK(mode_server.start().code == yori::ipc::IpcTransportStartCode::kInvalidConfig);

  UdsIpcServerConfig bad_deadline = base_config(socket_path);
  bad_deadline.request_deadline = 0ms;
  UdsIpcServer deadline_server(runtime.executor(), handler, bad_deadline);
  YORI_CHECK(deadline_server.start().code == yori::ipc::IpcTransportStartCode::kInvalidConfig);

  // 非 root 配置他人属主：显式失败（DEC-010）。
  if (::geteuid() != 0) {
    UdsIpcServerConfig ownership = base_config(socket_path);
    ownership.apply_ownership = true;
    ownership.socket_owner_uid = 12345;
    ownership.socket_owner_gid = 12345;
    UdsIpcServer ownership_server(runtime.executor(), handler, ownership);
    const auto ownership_result = ownership_server.start();
    YORI_CHECK(ownership_result.code == yori::ipc::IpcTransportStartCode::kEndpointRejected);
    YORI_CHECK(!path_exists(socket_path));
  }

  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_protocol_roundtrip_and_peer() {
  const std::string directory = make_directory();
  const std::string socket_path = directory + "/yori.sock";

  yori::runtime::ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(yori::runtime::ExecutorRuntimeConfig{}, error));
  EchoHandler handler;
  UdsIpcServer server(runtime.executor(), handler, base_config(socket_path));
  YORI_CHECK(server.start().ok());

  // 真实客户端：请求 -> 响应；SO_PEERCRED 身份进入 handler。
  yori::ipc::UdsIpcClient client;
  IpcRequest request;
  request.kind = IpcRequestKind::kPs;
  const auto result = client.call(socket_path, request, 2000ms);
  YORI_CHECK(result.ok());
  YORI_CHECK(result.response.error == IpcError::kNone);
  YORI_CHECK(result.response.detail == "echo");
  YORI_CHECK(handler.last_peer.uid == static_cast<std::uint32_t>(::geteuid()));
  YORI_CHECK(handler.last_peer.pid == static_cast<std::uint32_t>(::getpid()));

  // 不存在的端点：连接失败。
  const auto unreachable =
      client.call(directory + "/missing.sock", request, 500ms);
  YORI_CHECK(unreachable.error == yori::ipc::IpcClientError::kConnectFailed);

  server.stop();
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

void test_malformed_and_slow_clients() {
  const std::string directory = make_directory();
  const std::string socket_path = directory + "/yori.sock";

  yori::runtime::ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(yori::runtime::ExecutorRuntimeConfig{}, error));
  EchoHandler handler;
  UdsIpcServerConfig config = base_config(socket_path);
  config.request_deadline = 300ms;
  UdsIpcServer server(runtime.executor(), handler, config);
  YORI_CHECK(server.start().ok());

  // 超载帧声明：服务端回 PROTOCOL 错误帧后断开。
  {
    RawConnection connection(socket_path);
    YORI_CHECK(connection.connected());
    std::uint8_t oversize[4] = {0xff, 0xff, 0xff, 0x7f};  // ~2 GiB
    YORI_CHECK(connection.send_all(oversize, sizeof(oversize)));

    std::vector<std::uint8_t> buffer(4096);
    const std::size_t received = connection.receive(buffer.data(), buffer.size());
    YORI_CHECK(received > 4);
    // 响应帧 payload 应可解码为 kProtocol 错误。
    const std::uint32_t payload_length = frame_length(buffer, 0);
    YORI_CHECK(payload_length >= 2 && payload_length <= received - 4);
    const auto decoded = yori::ipc::decode_response_payload(buffer.data() + 4, payload_length);
    YORI_CHECK(decoded.ok());
    YORI_CHECK(decoded.value.error == IpcError::kProtocol);
  }

  // 静默客户端：连接后不发送 -> deadline 后被断开（EOF）。
  {
    RawConnection connection(socket_path);
    YORI_CHECK(connection.connected());
    std::vector<std::uint8_t> buffer(16);
    const std::size_t received = connection.receive(buffer.data(), buffer.size());
    YORI_CHECK(received == 0);
  }

  // 半帧请求（截断的合法帧）：deadline 后断开。
  {
    RawConnection connection(socket_path);
    YORI_CHECK(connection.connected());
    const std::uint8_t partial[6] = {0x10, 0, 0, 0, 0x01, 0x02};
    YORI_CHECK(connection.send_all(partial, sizeof(partial)));
    std::vector<std::uint8_t> buffer(16);
    YORI_CHECK(connection.receive(buffer.data(), buffer.size()) == 0);
  }

  // handler 异常 -> INTERNAL 响应（不吞、不崩）。
  handler.throw_on_handle = true;
  {
    yori::ipc::UdsIpcClient client;
    IpcRequest request;
    request.kind = IpcRequestKind::kGpu;
    const auto result = client.call(socket_path, request, 2000ms);
    YORI_CHECK(result.ok());
    YORI_CHECK(result.response.error == IpcError::kInternal);
  }
  handler.throw_on_handle = false;

  // 服务端仍健康：正常请求继续成功。
  {
    yori::ipc::UdsIpcClient client;
    IpcRequest request;
    request.kind = IpcRequestKind::kQueue;
    const auto result = client.call(socket_path, request, 2000ms);
    YORI_CHECK(result.ok() && result.response.error == IpcError::kNone);
  }

  server.stop();
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
}

}  // namespace

int main() {
  test_lifecycle_and_permissions();
  test_stale_endpoint_rules();
  test_protocol_roundtrip_and_peer();
  test_malformed_and_slow_clients();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "ipc server: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("ipc server: all checks passed\n");
  return 0;
}
