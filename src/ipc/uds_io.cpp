#include "uds_io.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <limits>

namespace yori::ipc::uds {
namespace {

int remaining_ms(SteadyTime deadline) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  const auto count = remaining.count();
  if (count <= 0) {
    return 0;
  }
  if (count > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(count) + 1;  // poll 超时向上取整，避免忙等
}

std::uint32_t decode_length(const std::uint8_t header[4]) noexcept {
  return static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8) |
         (static_cast<std::uint32_t>(header[2]) << 16) |
         (static_cast<std::uint32_t>(header[3]) << 24);
}

}  // namespace

const char* to_string(FrameIoError error) noexcept {
  switch (error) {
    case FrameIoError::kNone:
      return "none";
    case FrameIoError::kTimeout:
      return "timeout";
    case FrameIoError::kClosed:
      return "closed";
    case FrameIoError::kBadLength:
      return "bad frame length";
    case FrameIoError::kOversize:
      return "oversize frame";
    case FrameIoError::kIoError:
      return "io error";
  }
  return "unknown";
}

FrameIoError wait_readable(int fd, SteadyTime deadline) noexcept {
  while (true) {
    struct pollfd waiter {};
    waiter.fd = fd;
    waiter.events = POLLIN;
    const int ready = ::poll(&waiter, 1, remaining_ms(deadline));
    if (ready > 0) {
      if ((waiter.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        return FrameIoError::kNone;
      }
      return FrameIoError::kIoError;
    }
    if (ready == 0) {
      return FrameIoError::kTimeout;
    }
    if (errno != EINTR) {
      return FrameIoError::kIoError;
    }
  }
}

FrameIoError read_full(int fd, std::uint8_t* out, std::size_t n, SteadyTime deadline) noexcept {
  std::size_t received = 0;
  while (received < n) {
    const FrameIoError wait = wait_readable(fd, deadline);
    if (wait != FrameIoError::kNone) {
      return wait;
    }
    const ssize_t count = ::read(fd, out + received, n - received);
    if (count > 0) {
      received += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) {
      return FrameIoError::kClosed;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;  // 唤醒竞态：重新等待
    }
    if (errno != EINTR) {
      return FrameIoError::kIoError;
    }
  }
  return FrameIoError::kNone;
}

FrameIoError write_full(int fd, const std::uint8_t* data, std::size_t n,
                        SteadyTime deadline) noexcept {
  std::size_t sent = 0;
  while (sent < n) {
    struct pollfd waiter {};
    waiter.fd = fd;
    waiter.events = POLLOUT;
    const int ready = ::poll(&waiter, 1, remaining_ms(deadline));
    if (ready > 0) {
      if ((waiter.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
          (waiter.revents & POLLOUT) == 0) {
        return FrameIoError::kIoError;
      }
    } else if (ready == 0) {
      return FrameIoError::kTimeout;
    } else if (errno != EINTR) {
      return FrameIoError::kIoError;
    }

    const ssize_t count = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (count >= 0) {
      sent += static_cast<std::size_t>(count);
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    if (errno != EINTR) {
      return FrameIoError::kIoError;
    }
  }
  return FrameIoError::kNone;
}

FrameIoError write_frame(int fd, const std::vector<std::uint8_t>& payload,
                         SteadyTime deadline) noexcept {
  if (payload.size() > IpcProtocolLimits::kMaxPayloadBytes) {
    return FrameIoError::kOversize;
  }
  std::uint8_t header[4] = {static_cast<std::uint8_t>(payload.size() & 0xffu),
                            static_cast<std::uint8_t>((payload.size() >> 8) & 0xffu),
                            static_cast<std::uint8_t>((payload.size() >> 16) & 0xffu),
                            static_cast<std::uint8_t>((payload.size() >> 24) & 0xffu)};
  const FrameIoError header_write = write_full(fd, header, sizeof(header), deadline);
  if (header_write != FrameIoError::kNone) {
    return header_write;
  }
  return write_full(fd, payload.data(), payload.size(), deadline);
}

FrameIoError read_frame(int fd, std::vector<std::uint8_t>& payload, SteadyTime deadline) {
  std::uint8_t header[4] = {};
  const FrameIoError header_read = read_full(fd, header, sizeof(header), deadline);
  if (header_read != FrameIoError::kNone) {
    return header_read;
  }
  const std::uint32_t length = decode_length(header);
  if (length < IpcProtocolLimits::kMinPayloadBytes) {
    return FrameIoError::kBadLength;
  }
  if (length > IpcProtocolLimits::kMaxPayloadBytes) {
    return FrameIoError::kOversize;
  }
  payload.resize(length);
  return read_full(fd, payload.data(), payload.size(), deadline);
}

}  // namespace yori::ipc::uds
