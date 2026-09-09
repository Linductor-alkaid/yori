#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>
#include <yori/ipc/uds_client.hpp>

#include "uds_io.hpp"

namespace yori::ipc {
namespace {

constexpr std::size_t kMaxEndpointBytes = sizeof(sockaddr_un::sun_path) - 1;

// 小型 fd 守卫：任何路径（含协议错误）都不泄漏描述符。
class FdGuard final {
 public:
  explicit FdGuard(int fd) : fd_(fd) {}
  ~FdGuard() {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
  }

  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  FdGuard(FdGuard&&) = delete;
  FdGuard& operator=(FdGuard&&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  int release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

IpcClientResult client_error(IpcClientError error) {
  IpcClientResult result;
  result.error = error;
  return result;
}

// 非阻塞 connect + poll 完成判定。
bool connect_with_deadline(int fd, const sockaddr_un& address, uds::SteadyTime deadline) {
  const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  if (rc == 0) {
    return true;
  }
  if (errno != EINPROGRESS) {
    return false;
  }
  while (true) {
    struct pollfd waiter {};
    waiter.fd = fd;
    waiter.events = POLLOUT;
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    int wait_ms = static_cast<int>(remaining.count());
    if (remaining.count() <= 0) {
      wait_ms = 0;
    } else if (remaining.count() > std::numeric_limits<int>::max()) {
      wait_ms = std::numeric_limits<int>::max();
    }
    const int ready = ::poll(&waiter, 1, wait_ms);
    if (ready > 0) {
      int socket_error = 0;
      socklen_t error_length = sizeof(socket_error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0) {
        return false;
      }
      errno = socket_error;
      return socket_error == 0;
    }
    if (ready == 0) {
      errno = ETIMEDOUT;
      return false;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

}  // namespace

IpcClientResult UdsIpcClient::call(const std::string& endpoint, const IpcRequest& request,
                                   std::chrono::milliseconds timeout) {
  if (endpoint.empty() || endpoint.front() != '/' || endpoint.size() > kMaxEndpointBytes) {
    return client_error(IpcClientError::kConnectFailed);
  }

  std::vector<std::uint8_t> payload;
  if (!encode_request_payload(request, payload)) {
    return client_error(IpcClientError::kProtocol);
  }

  const auto started = std::chrono::steady_clock::now();
  const uds::SteadyTime deadline = started + timeout;

  FdGuard guard(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (guard.get() < 0) {
    return client_error(IpcClientError::kConnectFailed);
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  if (!connect_with_deadline(guard.get(), address, deadline)) {
    if (errno == ETIMEDOUT) {
      return client_error(IpcClientError::kTimeout);
    }
    return client_error(IpcClientError::kConnectFailed);
  }

  const uds::FrameIoError write_error = uds::write_frame(guard.get(), payload, deadline);
  if (write_error != uds::FrameIoError::kNone) {
    if (write_error == uds::FrameIoError::kTimeout) {
      return client_error(IpcClientError::kTimeout);
    }
    if (write_error == uds::FrameIoError::kClosed) {
      return client_error(IpcClientError::kClosed);
    }
    return client_error(IpcClientError::kProtocol);
  }

  std::vector<std::uint8_t> response_payload;
  const uds::FrameIoError read_error = uds::read_frame(guard.get(), response_payload, deadline);
  if (read_error == uds::FrameIoError::kTimeout) {
    return client_error(IpcClientError::kTimeout);
  }
  if (read_error == uds::FrameIoError::kClosed) {
    return client_error(IpcClientError::kClosed);
  }
  if (read_error != uds::FrameIoError::kNone) {
    return client_error(IpcClientError::kProtocol);
  }

  const IpcResponseDecodeResult decoded =
      decode_response_payload(response_payload.data(), response_payload.size());
  if (!decoded.ok()) {
    return client_error(IpcClientError::kProtocol);
  }

  IpcClientResult result;
  result.error = IpcClientError::kNone;
  result.response = std::move(decoded.value);
  return result;
}

}  // namespace yori::ipc
