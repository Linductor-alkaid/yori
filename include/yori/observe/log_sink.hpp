#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace yori::observe {

enum class LogStreamKind {
  kStdout,
  kStderr,
};

[[nodiscard]] const char* to_string(LogStreamKind stream) noexcept;
[[nodiscard]] std::string log_file_name(LogStreamKind stream);

struct LogSinkLimits final {
  static constexpr std::uint64_t kMinFileBytes = 4096;
  static constexpr std::uint64_t kMaxFileBytes = 1ULL << 30;        // 1 GiB
  static constexpr std::uint64_t kDefaultFileBytes = 256ULL << 20;  // 256 MiB
  static constexpr std::uint32_t kMaxRotationHistory = 4;
  static constexpr std::uint32_t kDefaultRotationHistory = 1;
  static constexpr std::size_t kMaxDirectoryBytes = 4096;
};

// 单 Job 日志落盘配置。directory 是该 Job 的专属日志目录（如
// /var/lib/yori/jobs/<job-id>），由调用方创建并保证路径组件可信。
struct LogSinkConfig final {
  std::string directory;
  std::uint64_t max_file_bytes{LogSinkLimits::kDefaultFileBytes};
  std::uint32_t rotation_history{LogSinkLimits::kDefaultRotationHistory};

  [[nodiscard]] bool valid() const noexcept;
};

enum class LogSinkErrorCode {
  kNone,
  kInvalidConfig,
  kNotOpen,
  kOpenFailed,
  kOwnershipFailed,
  kCloseFailed,
  kWriteFailed,
  kRotateFailed,
};

[[nodiscard]] const char* to_string(LogSinkErrorCode code) noexcept;

// 文件后端 SPI：生产默认实现是 POSIX 文件操作（O_CREAT|O_APPEND|O_NOFOLLOW、
// fchmod 0640、root 时 fchown）；测试用以注入写失败等故障。
class LogIo {
 public:
  virtual ~LogIo() = default;

  // 返回 fd；失败时返回 -1 并设置 error。
  [[nodiscard]] virtual int open_append(const std::string& path, std::string& error) = 0;
  [[nodiscard]] virtual bool write_all(int fd, const char* data, std::size_t size,
                                       std::string& error) = 0;
  [[nodiscard]] virtual bool close_fd(int fd, std::string& error) = 0;
  // 轮转 rename；目标存在时由实现负责覆盖语义。
  [[nodiscard]] virtual bool rotate(const std::string& from, const std::string& to,
                                    std::string& error) = 0;
  [[nodiscard]] virtual bool remove_file(const std::string& path, std::string& error) = 0;
  // 当前文件大小（用于原位续写时恢复逻辑 offset）；失败返回 kUnknownFileSize。
  static constexpr std::uint64_t kUnknownFileSize = ~std::uint64_t{0};
  [[nodiscard]] virtual std::uint64_t file_size(int fd) noexcept = 0;
  // 应用 0640 与属主（owner_uid/owner_gid）。非 root 环境无法 fchown 时返回 false
  // 并在 error 中说明；调用方决定是否降级。
  [[nodiscard]] virtual bool apply_ownership(int fd, std::uint32_t owner_uid,
                                             std::uint32_t owner_gid, std::string& error) = 0;
};

[[nodiscard]] std::shared_ptr<LogIo> default_log_io();

struct LogSinkStatistics final {
  std::uint64_t bytes_accepted{0};  // 两个流合计
  std::uint64_t bytes_dropped{0};   // 写失败丢弃
  std::uint32_t rotations{0};
  std::uint32_t write_failures{0};
};

struct LogWriteResult final {
  LogSinkErrorCode code{LogSinkErrorCode::kNone};
  std::size_t bytes_accepted{0};
  std::uint64_t dropped_bytes{0};
  bool rotated{false};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == LogSinkErrorCode::kNone; }
};

struct LogSinkOpenOptions final {
  std::uint32_t owner_uid{0};
  std::uint32_t owner_gid{0};
  // 非 root 环境无法 fchown 时降级为仅设置 0640 并继续（结果仍为成功，统计可见）。
  bool allow_ownership_downgrade{true};
};

// 单 owner 的有界日志落盘器（设计第 11.2 节）：每流一个文件，单文件上限触发轮转，
// 写失败丢弃数据并在下次成功写入前插入 drop 标记；逻辑 offset 只计接受字节。
class LogSink final {
 public:
  LogSink();
  explicit LogSink(std::shared_ptr<LogIo> io);
  ~LogSink();

  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;
  LogSink(LogSink&&) noexcept;
  LogSink& operator=(LogSink&&) noexcept;

  // 打开 directory 下的 stdout.log / stderr.log（已存在则原位追加，DEC-008）。
  [[nodiscard]] LogSinkErrorCode open(const LogSinkConfig& config,
                                      const LogSinkOpenOptions& options, std::string& error);

  // 追加一个有界块；分块跨轮转写入。失败时丢弃未写部分并挂起 drop 标记。
  [[nodiscard]] LogWriteResult append(LogStreamKind stream, std::string_view data);

  [[nodiscard]] LogSinkErrorCode close(LogStreamKind stream, std::string& error);
  void close_all() noexcept;

  [[nodiscard]] bool is_open(LogStreamKind stream) const noexcept;
  // 逻辑 offset：该流自创建起累计接受的字节数（不含 drop 标记），单调不减。
  [[nodiscard]] std::uint64_t logical_offset(LogStreamKind stream) const noexcept;
  [[nodiscard]] const LogSinkConfig& config() const noexcept;
  [[nodiscard]] LogSinkStatistics statistics() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::observe
