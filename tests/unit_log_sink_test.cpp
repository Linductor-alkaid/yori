#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <yori/observe/log_sink.hpp>

#include "yori_test.hpp"

namespace {

using namespace yori::observe;

std::string read_file(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::string data;
  char buffer[4096];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, n);
  }
  std::fclose(file);
  return data;
}

std::uint64_t file_size_of(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) < 0) {
    return ~std::uint64_t{0};
  }
  return static_cast<std::uint64_t>(st.st_size);
}

std::string make_directory() {
  char pattern[] = "/tmp/yori-log-sink-test-XXXXXX";
  char* dir = ::mkdtemp(pattern);
  YORI_CHECK(dir != nullptr);
  return dir;
}

// 故障注入后端：前 fail_first_n 次写失败，其余透传真实现。
class FlakyLogIo final : public LogIo {
 public:
  explicit FlakyLogIo(std::shared_ptr<LogIo> backend) : backend_(std::move(backend)) {}

  int fail_writes_remaining{0};

  int open_append(const std::string& path, std::string& error) override {
    return backend_->open_append(path, error);
  }

  bool write_all(int fd, const char* data, std::size_t size, std::string& error) override {
    if (fail_writes_remaining > 0) {
      --fail_writes_remaining;
      error = "injected write failure";
      return false;
    }
    return backend_->write_all(fd, data, size, error);
  }

  bool close_fd(int fd, std::string& error) override { return backend_->close_fd(fd, error); }

  bool rotate(const std::string& from, const std::string& to, std::string& error) override {
    return backend_->rotate(from, to, error);
  }

  bool remove_file(const std::string& path, std::string& error) override {
    return backend_->remove_file(path, error);
  }

  std::uint64_t file_size(int fd) noexcept override { return backend_->file_size(fd); }

  bool apply_ownership(int fd, std::uint32_t owner_uid, std::uint32_t owner_gid,
                       std::string& error) override {
    return backend_->apply_ownership(fd, owner_uid, owner_gid, error);
  }

 private:
  std::shared_ptr<LogIo> backend_;
};

LogSinkConfig small_config(const std::string& directory, std::uint64_t max_bytes,
                           std::uint32_t history) {
  LogSinkConfig config;
  config.directory = directory;
  config.max_file_bytes = max_bytes;
  config.rotation_history = history;
  return config;
}

}  // namespace

int main() {
  // ---- 配置校验 --------------------------------------------------------------
  YORI_CHECK(!LogSinkConfig{}.valid());  // 空 directory。
  YORI_CHECK(!small_config("relative", 4096, 1).valid());
  YORI_CHECK(!small_config("/tmp", 4095, 1).valid());
  YORI_CHECK(!small_config("/tmp", 1 << 31, 1).valid());
  YORI_CHECK(!small_config("/tmp", 4096, 5).valid());
  YORI_CHECK(small_config("/tmp", 4096, 0).valid());
  YORI_CHECK(small_config("/tmp", 4096, 4).valid());

  // ---- 打开、追加、逻辑 offset 与权限 ----------------------------------------
  const std::string directory = make_directory();
  {
    LogSink sink;
    LogSinkOpenOptions options;
    options.owner_uid = static_cast<std::uint32_t>(::geteuid());
    options.owner_gid = static_cast<std::uint32_t>(::getegid());
    std::string error;
    YORI_CHECK(sink.open(small_config(directory, 1 << 20, 1), options, error) ==
               LogSinkErrorCode::kNone);

    const auto out = sink.append(LogStreamKind::kStdout, "hello stdout\n");
    const auto err = sink.append(LogStreamKind::kStderr, "hello stderr\n");
    YORI_CHECK(out.ok() && out.bytes_accepted == 13);
    YORI_CHECK(err.ok() && err.bytes_accepted == 13);
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStdout) == 13);
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStderr) == 13);

    struct stat st {};
    YORI_CHECK(::stat((directory + "/stdout.log").c_str(), &st) == 0);
    YORI_CHECK((st.st_mode & 0777) == 0640);

    YORI_CHECK(sink.close(LogStreamKind::kStdout, error) == LogSinkErrorCode::kNone);
    YORI_CHECK(read_file(directory + "/stdout.log") == "hello stdout\n");
    YORI_CHECK(read_file(directory + "/stderr.log") == "hello stderr\n");
  }

  // ---- DEC-008：原位续写（offset 从既有大小继续） -----------------------------
  {
    LogSink sink;
    LogSinkOpenOptions options;
    std::string error;
    YORI_CHECK(sink.open(small_config(directory, 1 << 20, 1), options, error) ==
               LogSinkErrorCode::kNone);
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStdout) == 13);  // 既有文件 13 字节。
    const auto result = sink.append(LogStreamKind::kStdout, "more\n");
    YORI_CHECK(result.ok());
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStdout) == 18);
    YORI_CHECK(read_file(directory + "/stdout.log") == "hello stdout\nmore\n");
  }

  // ---- 轮转：单文件上限、历史保留与大块跨轮转 --------------------------------
  {
    const std::string rotate_dir = make_directory();
    LogSink sink;
    LogSinkOpenOptions options;
    std::string error;
    // 上限 4 KiB、保留 1 个历史文件。
    YORI_CHECK(sink.open(small_config(rotate_dir, 4096, 1), options, error) ==
               LogSinkErrorCode::kNone);
    // 一个 10 KiB 的块必须跨三次轮转分块写入（单块分块上限 64 KiB）。
    const std::string blob(10 * 1024, 'x');
    const auto result = sink.append(LogStreamKind::kStdout, blob);
    YORI_CHECK(result.ok());
    YORI_CHECK(result.bytes_accepted == 10 * 1024);
    YORI_CHECK(result.rotated);
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStdout) == 10 * 1024);
    YORI_CHECK(read_file(rotate_dir + "/stdout.log").size() == 10 * 1024 - 2 * 4096);
    YORI_CHECK(read_file(rotate_dir + "/stdout.log.1").size() == 4096);
    YORI_CHECK(sink.statistics().rotations >= 2);

    // 再写满当前文件触发一次轮转：.1 被覆盖（前 2048 字节为 x，补满部分为 y）。
    const auto again = sink.append(LogStreamKind::kStdout, std::string(4096, 'y'));
    YORI_CHECK(again.ok() && again.rotated);
    YORI_CHECK(read_file(rotate_dir + "/stdout.log").size() == 2048);
    YORI_CHECK(read_file(rotate_dir + "/stdout.log.1") ==
               std::string(2048, 'x') + std::string(2048, 'y'));
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStdout) == 10 * 1024 + 4096);
  }

  // ---- rotation_history=0：不保留历史 ----------------------------------------
  {
    const std::string rotate_dir = make_directory();
    LogSink sink;
    LogSinkOpenOptions options;
    std::string error;
    YORI_CHECK(sink.open(small_config(rotate_dir, 4096, 0), options, error) ==
               LogSinkErrorCode::kNone);
    const auto result = sink.append(LogStreamKind::kStdout, std::string(10 * 1024, 'z'));
    YORI_CHECK(result.ok() && result.bytes_accepted == 10 * 1024);
    YORI_CHECK(read_file(rotate_dir + "/stdout.log").size() == 10 * 1024 - 2 * 4096);
    YORI_CHECK(file_size_of(rotate_dir + "/stdout.log.1") == ~std::uint64_t{0});
  }

  // ---- 写失败丢弃 + drop 标记（注入故障） -------------------------------------
  {
    auto flaky = std::make_shared<FlakyLogIo>(default_log_io());
    LogSink sink(flaky);
    LogSinkOpenOptions options;
    std::string error;
    YORI_CHECK(sink.open(small_config(make_directory(), 1 << 20, 1), options, error) ==
               LogSinkErrorCode::kNone);

    flaky->fail_writes_remaining = 1;
    const auto dropped = sink.append(LogStreamKind::kStdout, "payload\n");
    YORI_CHECK(dropped.code == LogSinkErrorCode::kWriteFailed);
    YORI_CHECK(dropped.dropped_bytes == 8);
    YORI_CHECK(sink.statistics().bytes_dropped == 8);
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStdout) == 0);

    // 下一次成功写入前插入 drop 标记。
    const auto recovered = sink.append(LogStreamKind::kStdout, "after\n");
    YORI_CHECK(recovered.ok());
    const std::string content = read_file(sink.config().directory + "/stdout.log");
    YORI_CHECK(content.find("[yori] dropped 8 bytes\n") != std::string::npos);
    YORI_CHECK(content.find("after\n") != std::string::npos);
    YORI_CHECK(sink.logical_offset(LogStreamKind::kStdout) == 6);  // 标记不计入 offset。
  }

  // ---- 非 root 属主降级：文件仍可写、0640 生效 --------------------------------
  {
    if (::geteuid() != 0) {
      LogSink sink;
      LogSinkOpenOptions options;
      options.owner_uid = 12345;  // 非 root 无法 chown：允许降级。
      options.owner_gid = 12345;
      std::string error;
      YORI_CHECK(sink.open(small_config(make_directory(), 4096, 1), options, error) ==
                 LogSinkErrorCode::kNone);
      YORI_CHECK(sink.append(LogStreamKind::kStdout, "downgraded\n").ok());
      struct stat st {};
      YORI_CHECK(::stat((sink.config().directory + "/stdout.log").c_str(), &st) == 0);
      YORI_CHECK((st.st_mode & 0777) == 0640);
      YORI_CHECK(st.st_uid == ::geteuid());  // chown 未生效，仍是当前用户。
    }
    {
      // 不允许降级时显式失败。
      LogSink sink;
      LogSinkOpenOptions options;
      options.owner_uid = 12345;
      options.owner_gid = 12345;
      options.allow_ownership_downgrade = false;
      std::string error;
      if (::geteuid() != 0) {
        YORI_CHECK(sink.open(small_config(make_directory(), 4096, 1), options, error) ==
                   LogSinkErrorCode::kOwnershipFailed);
      }
    }
  }

  // ---- 未打开流写入的显式拒绝 --------------------------------------------------
  {
    LogSink sink;
    const auto result = sink.append(LogStreamKind::kStdout, "x");
    YORI_CHECK(result.code == LogSinkErrorCode::kNotOpen);
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
