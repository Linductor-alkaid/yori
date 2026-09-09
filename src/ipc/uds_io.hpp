#pragma once

// UDS 帧传输内部共享（client 与 server 适配共用）：长度前缀帧的阻塞式
// poll I/O 与截止时间。内部头，不安装、不得被公共头包含。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <yori/ipc/ipc_protocol.hpp>

namespace yori::ipc::uds {

enum class FrameIoError : std::uint8_t {
  kNone = 0,
  kTimeout = 1,
  kClosed = 2,     // 对端 EOF
  kBadLength = 3,  // 帧长度低于协议下限
  kOversize = 4,   // 帧长度超过协议上限
  kIoError = 5,
};

[[nodiscard]] const char* to_string(FrameIoError error) noexcept;

using SteadyTime = std::chrono::steady_clock::time_point;

// 等待 fd 可读（POLLIN/HUP/ERR），直至截止时间。
[[nodiscard]] FrameIoError wait_readable(int fd, SteadyTime deadline) noexcept;

// 读满 n 字节；deadline 后返回 kTimeout，EOF 返回 kClosed。
[[nodiscard]] FrameIoError read_full(int fd, std::uint8_t* out, std::size_t n,
                                     SteadyTime deadline) noexcept;

// 经 poll + send(MSG_DONTWAIT|MSG_NOSIGNAL) 写满 n 字节。
[[nodiscard]] FrameIoError write_full(int fd, const std::uint8_t* data, std::size_t n,
                                      SteadyTime deadline) noexcept;

// 读写一个完整帧（u32 长度前缀 + payload）。read_frame 在长度越界时返回
// kBadLength/kOversize 且不读取 payload。
[[nodiscard]] FrameIoError write_frame(int fd, const std::vector<std::uint8_t>& payload,
                                       SteadyTime deadline) noexcept;
[[nodiscard]] FrameIoError read_frame(int fd, std::vector<std::uint8_t>& payload,
                                      SteadyTime deadline);

}  // namespace yori::ipc::uds
