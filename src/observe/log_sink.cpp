#include <yori/observe/log_sink.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <string_view>
#include <utility>

namespace yori::observe {
namespace {

class PosixLogIo final : public LogIo {
 public:
  int open_append(const std::string& path, std::string& error) override {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
                          S_IRUSR | S_IWUSR | S_IRGRP);
    if (fd < 0) {
      error = std::string("open(") + path + ") failed: errno " + std::to_string(errno);
      return -1;
    }
    return fd;
  }

  bool write_all(int fd, const char* data, std::size_t size, std::string& error) override {
    std::size_t written = 0;
    while (written < size) {
      const ssize_t n = ::write(fd, data + written, size - written);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        error = "write failed: errno " + std::to_string(errno);
        return false;
      }
      written += static_cast<std::size_t>(n);
    }
    return true;
  }

  bool close_fd(int fd, std::string& error) override {
    if (::close(fd) < 0 && errno != EINTR) {
      error = "close failed: errno " + std::to_string(errno);
      return false;
    }
    return true;
  }

  bool rotate(const std::string& from, const std::string& to, std::string& error) override {
    if (::rename(from.c_str(), to.c_str()) < 0) {
      error = "rename(" + from + ", " + to + ") failed: errno " + std::to_string(errno);
      return false;
    }
    return true;
  }

  bool remove_file(const std::string& path, std::string& error) override {
    if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
      error = "unlink(" + path + ") failed: errno " + std::to_string(errno);
      return false;
    }
    return true;
  }

  std::uint64_t file_size(int fd) noexcept override {
    struct stat st {};
    if (::fstat(fd, &st) < 0) {
      return LogIo::kUnknownFileSize;
    }
    return static_cast<std::uint64_t>(st.st_size);
  }

  bool apply_ownership(int fd, std::uint32_t owner_uid, std::uint32_t owner_gid,
                       std::string& error) override {
    if (::fchmod(fd, S_IRUSR | S_IWUSR | S_IRGRP) < 0) {
      error = "fchmod 0640 failed: errno " + std::to_string(errno);
      return false;
    }
    if (::fchown(fd, static_cast<uid_t>(owner_uid), static_cast<gid_t>(owner_gid)) < 0) {
      error = "fchown failed: errno " + std::to_string(errno);
      return false;
    }
    return true;
  }
};

std::string join_directory(const std::string& directory, const std::string& name) {
  if (!directory.empty() && directory.back() == '/') {
    return directory + name;
  }
  return directory + "/" + name;
}

}  // namespace

const char* to_string(LogStreamKind stream) noexcept {
  switch (stream) {
    case LogStreamKind::kStdout:
      return "stdout";
    case LogStreamKind::kStderr:
      return "stderr";
  }
  return "unknown";
}

std::string log_file_name(LogStreamKind stream) {
  switch (stream) {
    case LogStreamKind::kStdout:
      return "stdout.log";
    case LogStreamKind::kStderr:
      return "stderr.log";
  }
  return "unknown.log";
}

bool LogSinkConfig::valid() const noexcept {
  return !directory.empty() && directory.size() <= LogSinkLimits::kMaxDirectoryBytes &&
         directory.find('\0') == std::string::npos && directory.front() == '/' &&
         max_file_bytes >= LogSinkLimits::kMinFileBytes &&
         max_file_bytes <= LogSinkLimits::kMaxFileBytes &&
         rotation_history <= LogSinkLimits::kMaxRotationHistory;
}

const char* to_string(LogSinkErrorCode code) noexcept {
  switch (code) {
    case LogSinkErrorCode::kNone:
      return "none";
    case LogSinkErrorCode::kInvalidConfig:
      return "invalid log sink config";
    case LogSinkErrorCode::kNotOpen:
      return "log stream is not open";
    case LogSinkErrorCode::kOpenFailed:
      return "opening the log file failed";
    case LogSinkErrorCode::kOwnershipFailed:
      return "applying log file ownership failed";
    case LogSinkErrorCode::kCloseFailed:
      return "closing the log file failed";
    case LogSinkErrorCode::kWriteFailed:
      return "writing log data failed";
    case LogSinkErrorCode::kRotateFailed:
      return "rotating the log file failed";
  }
  return "unknown";
}

std::shared_ptr<LogIo> default_log_io() { return std::make_shared<PosixLogIo>(); }

struct LogSink::Impl final {
  struct Stream final {
    int fd{-1};
    bool open{false};
    std::uint64_t file_bytes{0};
    std::uint64_t offset{0};
    std::uint64_t pending_drop{0};
  };

  std::shared_ptr<LogIo> io;
  LogSinkConfig config{};
  LogSinkOpenOptions options{};
  bool ownership_downgraded{false};
  bool opened{false};
  std::array<Stream, 2> streams{};
  LogSinkStatistics statistics{};

  Stream& stream(LogStreamKind kind) noexcept {
    return streams[kind == LogStreamKind::kStdout ? 0 : 1];
  }

  const Stream& stream(LogStreamKind kind) const noexcept {
    return streams[kind == LogStreamKind::kStdout ? 0 : 1];
  }

  std::string current_path(LogStreamKind kind) const {
    return join_directory(config.directory, log_file_name(kind));
  }

  std::string history_path(LogStreamKind kind, std::uint32_t index) const {
    return current_path(kind) + "." + std::to_string(index);
  }

  LogSinkErrorCode apply_ownership_for_new_file(int fd, std::string& error) {
    if (io->apply_ownership(fd, options.owner_uid, options.owner_gid, error)) {
      return LogSinkErrorCode::kNone;
    }
    if (options.allow_ownership_downgrade) {
      ownership_downgraded = true;
      error.clear();
      return LogSinkErrorCode::kNone;
    }
    return LogSinkErrorCode::kOwnershipFailed;
  }

  // 打开当前文件（不存在则创建）并恢复逻辑 offset。返回 kNone 或失败码。
  LogSinkErrorCode open_stream(LogStreamKind kind, std::string& error) {
    Stream& state = stream(kind);
    const int fd = io->open_append(current_path(kind), error);
    if (fd < 0) {
      return LogSinkErrorCode::kOpenFailed;
    }
    const LogSinkErrorCode ownership = apply_ownership_for_new_file(fd, error);
    if (ownership != LogSinkErrorCode::kNone) {
      std::string close_error;
      static_cast<void>(io->close_fd(fd, close_error));
      return ownership;
    }
    const std::uint64_t size = io->file_size(fd);
    if (size == LogIo::kUnknownFileSize) {
      static_cast<void>(io->close_fd(fd, error));
      error = "fstat on the log file failed";
      return LogSinkErrorCode::kOpenFailed;
    }
    state.fd = fd;
    state.open = true;
    // DEC-008：原位续写，offset 从既有文件大小继续。
    state.file_bytes = size;
    state.offset = size;
    return LogSinkErrorCode::kNone;
  }

  bool rotate_stream(LogStreamKind kind, std::string& error) {
    Stream& state = stream(kind);
    const std::uint64_t offset_before = state.offset;
    if (state.fd >= 0) {
      std::string close_error;
      static_cast<void>(io->close_fd(state.fd, close_error));
      state.fd = -1;
      state.open = false;
    }
    if (config.rotation_history == 0) {
      if (!io->remove_file(current_path(kind), error)) {
        return false;
      }
    } else {
      for (std::uint32_t index = config.rotation_history; index >= 2; --index) {
        std::string rotate_error;
        static_cast<void>(
            io->rotate(history_path(kind, index - 1), history_path(kind, index), rotate_error));
      }
      if (!io->rotate(current_path(kind), history_path(kind, 1), error)) {
        return false;
      }
    }
    if (open_stream(kind, error) != LogSinkErrorCode::kNone) {
      return false;
    }
    // 逻辑 offset 跨轮转单调（轮转对新文件恢复 size=0，但总 offset 保留）。
    state.offset = offset_before;
    ++statistics.rotations;
    return true;
  }
};

LogSink::LogSink() : impl_(std::make_unique<Impl>()) { impl_->io = default_log_io(); }

LogSink::LogSink(std::shared_ptr<LogIo> io) : impl_(std::make_unique<Impl>()) {
  impl_->io = std::move(io);
}

LogSink::~LogSink() { close_all(); }

LogSink::LogSink(LogSink&&) noexcept = default;

LogSink& LogSink::operator=(LogSink&&) noexcept = default;

LogSinkErrorCode LogSink::open(const LogSinkConfig& config, const LogSinkOpenOptions& options,
                               std::string& error) {
  if (!config.valid()) {
    error = "log sink config is invalid";
    return LogSinkErrorCode::kInvalidConfig;
  }
  impl_->config = config;
  impl_->options = options;
  for (const LogStreamKind kind : {LogStreamKind::kStdout, LogStreamKind::kStderr}) {
    const LogSinkErrorCode code = impl_->open_stream(kind, error);
    if (code != LogSinkErrorCode::kNone) {
      close_all();
      return code;
    }
  }
  impl_->opened = true;
  return LogSinkErrorCode::kNone;
}

LogWriteResult LogSink::append(LogStreamKind stream_kind, std::string_view data) {
  LogWriteResult result;
  Impl::Stream& state = impl_->stream(stream_kind);
  if (!impl_->opened || !state.open) {
    result.code = LogSinkErrorCode::kNotOpen;
    result.message = to_string(result.code);
    return result;
  }

  if (state.pending_drop > 0) {
    const std::string marker =
        "[yori] dropped " + std::to_string(state.pending_drop) + " bytes\n";
    std::string marker_error;
    if (impl_->io->write_all(state.fd, marker.data(), marker.size(), marker_error)) {
      state.file_bytes += marker.size();
      state.pending_drop = 0;
    } else {
      ++impl_->statistics.write_failures;
    }
  }

  std::size_t written = 0;
  while (written < data.size()) {
    if (state.file_bytes >= impl_->config.max_file_bytes) {
      std::string rotate_error;
      if (!impl_->rotate_stream(stream_kind, rotate_error)) {
        result.code = LogSinkErrorCode::kRotateFailed;
        result.message = rotate_error;
        break;
      }
      result.rotated = true;
      continue;
    }
    const std::size_t room =
        static_cast<std::size_t>(impl_->config.max_file_bytes - state.file_bytes);
    const std::size_t chunk = std::min(room, data.size() - written);
    std::string write_error;
    if (!impl_->io->write_all(state.fd, data.data() + written, chunk, write_error)) {
      ++impl_->statistics.write_failures;
      result.code = LogSinkErrorCode::kWriteFailed;
      result.message = write_error;
      break;
    }
    state.file_bytes += chunk;
    state.offset += chunk;
    result.bytes_accepted += chunk;
    written += chunk;
  }

  if (written < data.size()) {
    const std::uint64_t dropped = data.size() - written;
    state.pending_drop += dropped;
    impl_->statistics.bytes_dropped += dropped;
    result.dropped_bytes = dropped;
  }
  impl_->statistics.bytes_accepted += result.bytes_accepted;
  return result;
}

LogSinkErrorCode LogSink::close(LogStreamKind stream_kind, std::string& error) {
  Impl::Stream& state = impl_->stream(stream_kind);
  if (!state.open || state.fd < 0) {
    return LogSinkErrorCode::kNotOpen;
  }
  if (!impl_->io->close_fd(state.fd, error)) {
    state.fd = -1;
    state.open = false;
    return LogSinkErrorCode::kCloseFailed;
  }
  state.fd = -1;
  state.open = false;
  return LogSinkErrorCode::kNone;
}

void LogSink::close_all() noexcept {
  if (impl_ == nullptr) {
    return;  // 已被移动的 LogSink。
  }
  for (const LogStreamKind kind : {LogStreamKind::kStdout, LogStreamKind::kStderr}) {
    Impl::Stream& state = impl_->stream(kind);
    if (state.open && state.fd >= 0) {
      std::string error;
      static_cast<void>(impl_->io->close_fd(state.fd, error));
    }
    state.open = false;
    state.fd = -1;
  }
  impl_->opened = false;
}

bool LogSink::is_open(LogStreamKind stream_kind) const noexcept {
  return impl_->opened && impl_->stream(stream_kind).open;
}

std::uint64_t LogSink::logical_offset(LogStreamKind stream_kind) const noexcept {
  return impl_->stream(stream_kind).offset;
}

const LogSinkConfig& LogSink::config() const noexcept { return impl_->config; }

LogSinkStatistics LogSink::statistics() const noexcept { return impl_->statistics; }

}  // namespace yori::observe
