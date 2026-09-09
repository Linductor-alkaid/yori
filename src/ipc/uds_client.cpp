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

  IpcResponseDecodeResult decoded =
      decode_response_payload(response_payload.data(), response_payload.size());
  if (!decoded.ok()) {
    return client_error(IpcClientError::kProtocol);
  }

  IpcClientResult result;
  result.error = IpcClientError::kNone;
  result.response = std::move(decoded.value);
  return result;
}

IpcFollowResult UdsIpcClient::follow(const std::string& endpoint, const IpcRequest& request,
                                     std::chrono::milliseconds setup_timeout,
                                     std::chrono::milliseconds frame_timeout,
                                     IpcStreamFrameHandler& handler) {
  IpcFollowResult follow_result;
  const auto follow_error = [&follow_result](IpcClientError error) -> IpcFollowResult {
    follow_result.error = error;
    return follow_result;
  };

  if (endpoint.empty() || endpoint.front() != '/' || endpoint.size() > kMaxEndpointBytes) {
    return follow_error(IpcClientError::kConnectFailed);
  }

  std::vector<std::uint8_t> payload;
  if (!encode_request_payload(request, payload)) {
    return follow_error(IpcClientError::kProtocol);
  }

  const uds::SteadyTime setup_deadline = std::chrono::steady_clock::now() + setup_timeout;

  FdGuard guard(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (guard.get() < 0) {
    return follow_error(IpcClientError::kConnectFailed);
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  if (!connect_with_deadline(guard.get(), address, setup_deadline)) {
    if (errno == ETIMEDOUT) {
      return follow_error(IpcClientError::kTimeout);
    }
    return follow_error(IpcClientError::kConnectFailed);
  }

  const uds::FrameIoError write_error = uds::write_frame(guard.get(), payload, setup_deadline);
  if (write_error != uds::FrameIoError::kNone) {
    if (write_error == uds::FrameIoError::kTimeout) {
      return follow_error(IpcClientError::kTimeout);
    }
    if (write_error == uds::FrameIoError::kClosed) {
      return follow_error(IpcClientError::kClosed);
    }
    return follow_error(IpcClientError::kProtocol);
  }

  std::vector<std::uint8_t> ack_payload;
  const uds::FrameIoError ack_error = uds::read_frame(guard.get(), ack_payload, setup_deadline);
  if (ack_error == uds::FrameIoError::kTimeout) {
    return follow_error(IpcClientError::kTimeout);
  }
  if (ack_error == uds::FrameIoError::kClosed) {
    return follow_error(IpcClientError::kClosed);
  }
  if (ack_error != uds::FrameIoError::kNone) {
    return follow_error(IpcClientError::kProtocol);
  }

  const IpcResponseDecodeResult ack_decoded =
      decode_response_payload(ack_payload.data(), ack_payload.size());
  if (!ack_decoded.ok()) {
    return follow_error(IpcClientError::kProtocol);
  }
  follow_result.ack = ack_decoded.value;
  if (follow_result.ack.error != IpcError::kNone) {
    // daemon 显式拒绝：会话未建立，正常返回由调用方呈现。
    return follow_result;
  }

  // 流式阶段：frame_timeout <= 0 视为无限等待（跟随安静流）。
  while (true) {
    const uds::SteadyTime frame_deadline = frame_timeout > std::chrono::milliseconds::zero()
                                               ? std::chrono::steady_clock::now() + frame_timeout
                                               : uds::SteadyTime::max();
    std::vector<std::uint8_t> frame_payload;
    const uds::FrameIoError frame_error =
        uds::read_frame(guard.get(), frame_payload, frame_deadline);
    if (frame_error == uds::FrameIoError::kTimeout) {
      return follow_error(IpcClientError::kTimeout);
    }
    if (frame_error == uds::FrameIoError::kClosed) {
      // 对端在终止帧前关闭（或终止帧后正常关闭由下方 break 路径处理）。
      return follow_error(IpcClientError::kClosed);
    }
    if (frame_error != uds::FrameIoError::kNone) {
      return follow_error(IpcClientError::kProtocol);
    }

    const IpcStreamFrameDecodeResult decoded =
        decode_stream_frame_payload(frame_payload.data(), frame_payload.size());
    if (!decoded.ok()) {
      return follow_error(IpcClientError::kProtocol);
    }
    if (!handler.on_frame(decoded.value)) {
      return follow_result;  // 客户端主动停止。
    }
    if (decoded.value.kind == IpcStreamFrameKind::kLogEof ||
        decoded.value.kind == IpcStreamFrameKind::kLogBackpressure) {
      return follow_result;  // 终止帧已回调，会话由 daemon 侧关闭。
    }
  }
}

}  // namespace yori::ipc
