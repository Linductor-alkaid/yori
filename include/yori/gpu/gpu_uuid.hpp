#pragma once

#include <compare>
#include <cstddef>
#include <string>
#include <utility>

namespace yori::gpu {

// GPU 稳定身份（设计第 7 节）：lease 与 StateStore 的持久化身份，NVML index
// 只是易用输入。M9（DEC-012）起 placement 约束同样以该身份表达；独立成头以
// 允许 job 契约引用而不再依赖完整 provider 接口。
class GpuUuid final {
 public:
  static constexpr std::size_t kMaxBytes = 96;

  GpuUuid() = default;
  explicit GpuUuid(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept {
    return !value_.empty() && value_.size() <= kMaxBytes && value_.find('\0') == std::string::npos;
  }

  auto operator<=>(const GpuUuid&) const = default;

 private:
  std::string value_;
};

}  // namespace yori::gpu
