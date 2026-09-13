#include <cstring>
#include <limits>
#include <utility>
#include <yori/ipc/ipc_protocol.hpp>

namespace yori::ipc {
namespace {

// ---------------------------------------------------------------------------
// 编码端。Writer 只追加；上层先在临时缓冲编码 payload，成功后写长度前缀，
// 保证失败路径不产生半帧。
// ---------------------------------------------------------------------------
class Writer final {
 public:
  explicit Writer(std::vector<std::uint8_t>& buffer) : buffer_(buffer) {}

  void u8(std::uint8_t value) { buffer_.push_back(value); }

  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }

  void i32(std::int32_t value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "int32 width mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
  }

  // 成功返回 false 失败标记；调用方以 ok() 短路后续编码。
  [[nodiscard]] bool string(const std::string& value) {
    if (value.size() > IpcProtocolLimits::kMaxStringBytes ||
        value.find('\0') != std::string::npos) {
      return true;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    buffer_.insert(buffer_.end(), value.begin(), value.end());
    return false;
  }

  [[nodiscard]] bool string(const std::optional<std::string>& value) {
    if (!value) {
      u8(0);
      return false;
    }
    u8(1);
    return string(*value);
  }

  [[nodiscard]] bool bytes(const std::vector<std::uint8_t>& value) {
    if (value.size() > IpcProtocolLimits::kMaxBytesFieldBytes) {
      return true;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    buffer_.insert(buffer_.end(), value.begin(), value.end());
    return false;
  }

 private:
  std::vector<std::uint8_t>& buffer_;
};

// ---------------------------------------------------------------------------
// 解码端。所有读取先检查剩余长度；计数与长度先验证再分配；字符串拒绝 NUL。
// ---------------------------------------------------------------------------
class Reader final {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == size_; }

  [[nodiscard]] bool u8(std::uint8_t& out) {
    if (remaining() < 1) {
      return false;
    }
    out = data_[offset_];
    offset_ += 1;
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& out) {
    if (remaining() < 4) {
      return false;
    }
    out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      out |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(shift / 8)])
             << shift;
    }
    offset_ += 4;
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& out) {
    if (remaining() < 8) {
      return false;
    }
    out = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      out |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(shift / 8)])
             << shift;
    }
    offset_ += 8;
    return true;
  }

  [[nodiscard]] bool i32(std::int32_t& out) {
    std::uint32_t bits = 0;
    if (!u32(bits)) {
      return false;
    }
    static_assert(sizeof(bits) == sizeof(out), "int32 width mismatch");
    std::memcpy(&out, &bits, sizeof(out));
    return true;
  }

  // 长度不足/超上限返回 kOversize，含 NUL 返回 kBadString。
  [[nodiscard]] IpcDecodeError string(std::string& out) {
    std::uint32_t length = 0;
    if (!u32(length)) {
      return IpcDecodeError::kTruncated;
    }
    if (length > IpcProtocolLimits::kMaxStringBytes) {
      return IpcDecodeError::kOversize;
    }
    if (remaining() < length) {
      return IpcDecodeError::kTruncated;
    }
    out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    if (out.find('\0') != std::string::npos) {
      return IpcDecodeError::kBadString;
    }
    return IpcDecodeError::kNone;
  }

  [[nodiscard]] IpcDecodeError string(std::optional<std::string>& out) {
    std::uint8_t present = 0;
    if (!u8(present)) {
      return IpcDecodeError::kTruncated;
    }
    if (present > 1) {
      return IpcDecodeError::kInvalidValue;
    }
    if (present == 0) {
      out.reset();
      return IpcDecodeError::kNone;
    }
    out.emplace();
    return string(*out);
  }

  [[nodiscard]] IpcDecodeError bytes(std::vector<std::uint8_t>& out) {
    std::uint32_t length = 0;
    if (!u32(length)) {
      return IpcDecodeError::kTruncated;
    }
    if (length > IpcProtocolLimits::kMaxBytesFieldBytes) {
      return IpcDecodeError::kOversize;
    }
    if (remaining() < length) {
      return IpcDecodeError::kTruncated;
    }
    out.assign(data_ + offset_, data_ + offset_ + length);
    offset_ += length;
    return IpcDecodeError::kNone;
  }

  // 计数字段：先验证上限再产出。
  [[nodiscard]] IpcDecodeError count(std::uint32_t cap, std::uint32_t& out) {
    if (!u32(out)) {
      return IpcDecodeError::kTruncated;
    }
    if (out > cap) {
      return IpcDecodeError::kTooManyItems;
    }
    return IpcDecodeError::kNone;
  }

  // 0/1 布尔标记。
  [[nodiscard]] IpcDecodeError flag(bool& out) {
    std::uint8_t value = 0;
    if (!u8(value)) {
      return IpcDecodeError::kTruncated;
    }
    if (value > 1) {
      return IpcDecodeError::kInvalidValue;
    }
    out = value == 1;
    return IpcDecodeError::kNone;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_{0};
};

constexpr std::uint8_t kUnderlyingJobStateMax = 7;
constexpr std::uint8_t kUnderlyingGpuObservedStateMax = 2;
constexpr std::uint8_t kUnderlyingGpuLogicalStateMax = 3;
constexpr std::uint8_t kUnderlyingWaitReasonMax = 4;
constexpr std::uint8_t kUnderlyingGpuPlacementModeMax = 1;

// 编码请求 kind body（不含 version/kind 头）。返回 false 表示字段越界或
// 版本与字段不一致（v1 帧不允许携带 v2 扩展字段）。
bool encode_request_body(std::uint8_t version, const IpcRequest& request, Writer& writer) {
  switch (request.kind) {
    case IpcRequestKind::kSubmit: {
      const IpcSubmitRequest& submit = request.submit;
      if (submit.argv.size() > IpcProtocolLimits::kMaxItemCount ||
          submit.env.size() > IpcProtocolLimits::kMaxItemCount) {
        return false;
      }
      if (version < 2 && (submit.executable || submit.env_metadata)) {
        return false;
      }
      if (version < 3 && submit.gpu_spec) {
        return false;
      }
      writer.u32(static_cast<std::uint32_t>(submit.argv.size()));
      for (const std::string& argument : submit.argv) {
        if (writer.string(argument)) {
          return false;
        }
      }
      if (writer.string(submit.cwd)) {
        return false;
      }
      writer.u32(static_cast<std::uint32_t>(submit.env.size()));
      for (const auto& [name, value] : submit.env) {
        if (writer.string(name) || writer.string(value)) {
          return false;
        }
      }
      writer.u32(submit.gpu_request);
      if (writer.string(submit.launch_profile) || writer.string(submit.tensorboard_logdir)) {
        return false;
      }
      if (version >= 2) {
        if (writer.string(submit.executable)) {
          return false;
        }
        if (!submit.env_metadata) {
          writer.u8(0);
        } else {
          writer.u8(1);
          if (submit.env_metadata->source > 2) {
            return false;
          }
          writer.u8(submit.env_metadata->source);
          if (writer.string(submit.env_metadata->python_version)) {
            return false;
          }
        }
      }
      if (version >= 3) {
        // v3 尾部追加（DEC-012）：可选 placement 输入；v2 帧在此结束
        // （缺省 = kAny）。
        if (writer.string(submit.gpu_spec)) {
          return false;
        }
      }
      return true;
    }
    case IpcRequestKind::kPs:
    case IpcRequestKind::kQueue:
    case IpcRequestKind::kGpu:
      return true;
    case IpcRequestKind::kCancel:
      writer.u64(request.cancel.job_id);
      return true;
    case IpcRequestKind::kLogs:
      writer.u64(request.logs.job_id);
      writer.u32(request.logs.max_bytes);
      return true;
    case IpcRequestKind::kLogsFollow: {
      const IpcLogsFollowRequest& follow = request.logs_follow;
      writer.u64(follow.job_id);
      writer.u8(follow.since_stdout ? 1 : 0);
      if (follow.since_stdout) {
        writer.u64(*follow.since_stdout);
      }
      writer.u8(follow.since_stderr ? 1 : 0);
      if (follow.since_stderr) {
        writer.u64(*follow.since_stderr);
      }
      return true;
    }
    case IpcRequestKind::kTensorboard:
      writer.u64(request.tensorboard.job_id);
      return true;
    case IpcRequestKind::kInspect:
      writer.u64(request.inspect.job_id);
      return true;
  }
  return false;
}

bool encode_response_body(std::uint8_t version, const IpcResponse& response, Writer& writer) {
  writer.u8(static_cast<std::uint8_t>(response.error));
  if (writer.string(response.detail)) {
    return false;
  }
  switch (response.kind) {
    case IpcRequestKind::kSubmit:
      writer.u64(response.job_id);
      return true;
    case IpcRequestKind::kPs: {
      if (response.jobs.size() > IpcProtocolLimits::kMaxListItems) {
        return false;
      }
      writer.u32(static_cast<std::uint32_t>(response.jobs.size()));
      for (const IpcJobSummary& summary : response.jobs) {
        if (summary.argv.size() > IpcProtocolLimits::kMaxItemCount) {
          return false;
        }
        writer.u64(summary.job_id);
        writer.u8(summary.state);
        writer.u32(summary.owner_uid);
        writer.u64(summary.revision);
        writer.u8(summary.masked ? 1 : 0);
        if (!summary.masked) {
          writer.u32(static_cast<std::uint32_t>(summary.argv.size()));
          for (const std::string& argument : summary.argv) {
            if (writer.string(argument)) {
              return false;
            }
          }
          if (writer.string(summary.cwd) || writer.string(summary.tensorboard_logdir)) {
            return false;
          }
        }
        if (summary.exit) {
          writer.u8(1);
          writer.u8(summary.exit->exited_normally ? 1 : 0);
          writer.i32(summary.exit->code);
        } else {
          writer.u8(0);
        }
        if (version >= 3) {
          // v3（DEC-012）：QUEUED Job 的等待原因；detail（目标 UUID）仅
          // owner/admin 视图由服务端填充。
          if (summary.wait_reason > kUnderlyingWaitReasonMax) {
            return false;
          }
          writer.u8(summary.wait_reason);
          if (writer.string(summary.wait_detail)) {
            return false;
          }
        }
      }
      return true;
    }
    case IpcRequestKind::kQueue: {
      if (response.queue.size() > IpcProtocolLimits::kMaxListItems) {
        return false;
      }
      writer.u32(static_cast<std::uint32_t>(response.queue.size()));
      for (const IpcQueueEntry& entry : response.queue) {
        writer.u64(entry.job_id);
        writer.u32(entry.owner_uid);
        writer.u64(entry.submit_time_unix_ns);
        writer.u8(entry.state);
        if (version >= 3) {
          if (entry.wait_reason > kUnderlyingWaitReasonMax) {
            return false;
          }
          writer.u8(entry.wait_reason);
          if (writer.string(entry.wait_detail)) {
            return false;
          }
        }
      }
      return true;
    }
    case IpcRequestKind::kGpu: {
      if (response.devices.size() > IpcProtocolLimits::kMaxListItems) {
        return false;
      }
      writer.u64(response.gpu_revision);
      writer.u32(static_cast<std::uint32_t>(response.devices.size()));
      for (const IpcGpuDevice& device : response.devices) {
        if (writer.string(device.uuid)) {
          return false;
        }
        writer.u32(device.index);
        writer.u8(device.observed_state);
        writer.u8(device.logical_state);
        if (device.utilization_percent) {
          writer.u8(1);
          writer.u32(*device.utilization_percent);
        } else {
          writer.u8(0);
        }
        if (device.memory_used_bytes && device.memory_total_bytes) {
          writer.u8(1);
          writer.u64(*device.memory_used_bytes);
          writer.u64(*device.memory_total_bytes);
        } else {
          writer.u8(0);
        }
        if (device.leased_by_job) {
          writer.u8(1);
          writer.u64(*device.leased_by_job);
        } else {
          writer.u8(0);
        }
      }
      return true;
    }
    case IpcRequestKind::kCancel:
      writer.u8(response.state);
      return true;
    case IpcRequestKind::kLogs:
      writer.u8(response.logs.stdout_truncated ? 1 : 0);
      if (writer.bytes(response.logs.stdout_tail)) {
        return false;
      }
      writer.u8(response.logs.stderr_truncated ? 1 : 0);
      return !writer.bytes(response.logs.stderr_tail);
    case IpcRequestKind::kLogsFollow:
      writer.u8(response.logs_follow.job_state);
      writer.u64(response.logs_follow.stdout_offset);
      writer.u64(response.logs_follow.stderr_offset);
      return true;
    case IpcRequestKind::kTensorboard:
      return !(writer.string(response.tensorboard.logdir) ||
               writer.string(response.tensorboard.cwd));
    case IpcRequestKind::kInspect: {
      if (version < 2) {
        return false;
      }
      const IpcInspectPayload& inspect = response.inspect;
      if (inspect.argv.size() > IpcProtocolLimits::kMaxItemCount ||
          inspect.env.size() > IpcProtocolLimits::kMaxListItems) {
        return false;
      }
      writer.u64(inspect.job_id);
      writer.u8(inspect.state);
      writer.u32(inspect.owner_uid);
      writer.u64(inspect.revision);
      if (writer.string(inspect.cwd) || writer.string(inspect.executable)) {
        return false;
      }
      writer.u32(static_cast<std::uint32_t>(inspect.argv.size()));
      for (const std::string& argument : inspect.argv) {
        if (writer.string(argument)) {
          return false;
        }
      }
      if (!inspect.env_metadata) {
        writer.u8(0);
      } else {
        if (inspect.env_metadata->source > 2) {
          return false;
        }
        writer.u8(1);
        writer.u8(inspect.env_metadata->source);
        if (writer.string(inspect.env_metadata->python_version)) {
          return false;
        }
      }
      writer.u32(static_cast<std::uint32_t>(inspect.env.size()));
      for (const IpcEnvEntry& entry : inspect.env) {
        if (writer.string(entry.name)) {
          return false;
        }
        writer.u8(entry.masked ? 1 : 0);
        if (writer.string(entry.value)) {
          return false;
        }
      }
      if (inspect.gpu_uuid) {
        writer.u8(1);
        if (writer.string(*inspect.gpu_uuid)) {
          return false;
        }
      } else {
        writer.u8(0);
      }
      if (inspect.gpu_index) {
        writer.u8(1);
        writer.u32(*inspect.gpu_index);
      } else {
        writer.u8(0);
      }
      writer.u64(inspect.submit_time_unix_ns);
      if (inspect.exit) {
        writer.u8(1);
        writer.u8(inspect.exit->exited_normally ? 1 : 0);
        writer.i32(inspect.exit->code);
      } else {
        writer.u8(0);
      }
      if (writer.string(inspect.log_path)) {
        return false;
      }
      if (version >= 3) {
        // v3（DEC-012）：placement 输入（mode + required 时目标 UUID）。
        if (inspect.gpu_placement_mode > kUnderlyingGpuPlacementModeMax) {
          return false;
        }
        writer.u8(inspect.gpu_placement_mode);
        if (writer.string(inspect.gpu_placement_device)) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

IpcDecodeError decode_request_body(Reader& reader, IpcRequest& out) {
  std::uint8_t version = 0;
  std::uint8_t kind = 0;
  if (!reader.u8(version)) {
    return IpcDecodeError::kTruncated;
  }
  if (!reader.u8(kind)) {
    return IpcDecodeError::kTruncated;
  }
  if (!ipc_version_supported(version)) {
    return IpcDecodeError::kBadVersion;
  }
  if (kind < static_cast<std::uint8_t>(IpcRequestKind::kSubmit) ||
      kind > static_cast<std::uint8_t>(IpcRequestKind::kInspect)) {
    return IpcDecodeError::kBadKind;
  }
  // INSPECT 是 v2 新 kind：v1 帧携带视为坏 kind。
  if (kind == static_cast<std::uint8_t>(IpcRequestKind::kInspect) && version < 2) {
    return IpcDecodeError::kBadKind;
  }
  out = IpcRequest{};
  out.kind = static_cast<IpcRequestKind>(kind);
  out.version = version;

  switch (out.kind) {
    case IpcRequestKind::kSubmit: {
      IpcSubmitRequest& submit = out.submit;
      std::uint32_t argv_count = 0;
      IpcDecodeError error = reader.count(IpcProtocolLimits::kMaxItemCount, argv_count);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      submit.argv.reserve(argv_count);
      for (std::uint32_t i = 0; i < argv_count; ++i) {
        submit.argv.emplace_back();
        error = reader.string(submit.argv.back());
        if (error != IpcDecodeError::kNone) {
          return error;
        }
      }
      error = reader.string(submit.cwd);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      std::uint32_t env_count = 0;
      error = reader.count(IpcProtocolLimits::kMaxItemCount, env_count);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      for (std::uint32_t i = 0; i < env_count; ++i) {
        std::string name;
        std::string value;
        error = reader.string(name);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        error = reader.string(value);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        submit.env.emplace(std::move(name), std::move(value));
      }
      if (!reader.u32(submit.gpu_request)) {
        return IpcDecodeError::kTruncated;
      }
      error = reader.string(submit.launch_profile);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      error = reader.string(submit.tensorboard_logdir);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (version >= 2) {
        // v2 尾部追加（DEC-011）：可选 executable 与 env 元数据；v1 帧在此
        // 结束（缺省 = 无捕获）。
        error = reader.string(submit.executable);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        bool has_metadata = false;
        error = reader.flag(has_metadata);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        if (has_metadata) {
          IpcEnvMetadata metadata;
          if (!reader.u8(metadata.source)) {
            return IpcDecodeError::kTruncated;
          }
          if (metadata.source > 2) {
            return IpcDecodeError::kInvalidValue;
          }
          error = reader.string(metadata.python_version);
          if (error != IpcDecodeError::kNone) {
            return error;
          }
          submit.env_metadata = std::move(metadata);
        }
      }
      if (version >= 3) {
        // v3 尾部追加（DEC-012）：可选 placement 输入；v2 帧在此结束。
        error = reader.string(submit.gpu_spec);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
      }
      break;
    }
    case IpcRequestKind::kPs:
    case IpcRequestKind::kQueue:
    case IpcRequestKind::kGpu:
      break;
    case IpcRequestKind::kCancel:
      if (!reader.u64(out.cancel.job_id)) {
        return IpcDecodeError::kTruncated;
      }
      break;
    case IpcRequestKind::kLogs:
      if (!reader.u64(out.logs.job_id) || !reader.u32(out.logs.max_bytes)) {
        return IpcDecodeError::kTruncated;
      }
      break;
    case IpcRequestKind::kLogsFollow: {
      IpcLogsFollowRequest& follow = out.logs_follow;
      bool have_stdout = false;
      bool have_stderr = false;
      if (!reader.u64(follow.job_id)) {
        return IpcDecodeError::kTruncated;
      }
      IpcDecodeError error = reader.flag(have_stdout);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (have_stdout) {
        std::uint64_t since = 0;
        if (!reader.u64(since)) {
          return IpcDecodeError::kTruncated;
        }
        follow.since_stdout = since;
      }
      error = reader.flag(have_stderr);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (have_stderr) {
        std::uint64_t since = 0;
        if (!reader.u64(since)) {
          return IpcDecodeError::kTruncated;
        }
        follow.since_stderr = since;
      }
      break;
    }
    case IpcRequestKind::kTensorboard:
      if (!reader.u64(out.tensorboard.job_id)) {
        return IpcDecodeError::kTruncated;
      }
      break;
    case IpcRequestKind::kInspect:
      if (!reader.u64(out.inspect.job_id)) {
        return IpcDecodeError::kTruncated;
      }
      break;
  }
  if (!reader.exhausted()) {
    return IpcDecodeError::kTrailingBytes;
  }
  return IpcDecodeError::kNone;
}

IpcDecodeError decode_response_body(Reader& reader, IpcResponse& out) {
  std::uint8_t version = 0;
  std::uint8_t kind = 0;
  std::uint8_t error_code = 0;
  if (!reader.u8(version) || !reader.u8(kind)) {
    return IpcDecodeError::kTruncated;
  }
  if (!ipc_version_supported(version)) {
    return IpcDecodeError::kBadVersion;
  }
  if (kind < static_cast<std::uint8_t>(IpcRequestKind::kSubmit) ||
      kind > static_cast<std::uint8_t>(IpcRequestKind::kInspect)) {
    return IpcDecodeError::kBadKind;
  }
  if (kind == static_cast<std::uint8_t>(IpcRequestKind::kInspect) && version < 2) {
    return IpcDecodeError::kBadKind;
  }
  if (!reader.u8(error_code)) {
    return IpcDecodeError::kTruncated;
  }
  if (error_code > static_cast<std::uint8_t>(IpcError::kInternal)) {
    return IpcDecodeError::kInvalidValue;
  }
  out = IpcResponse{};
  out.kind = static_cast<IpcRequestKind>(kind);
  out.version = version;
  out.error = static_cast<IpcError>(error_code);

  IpcDecodeError error = reader.string(out.detail);
  if (error != IpcDecodeError::kNone) {
    return error;
  }

  switch (out.kind) {
    case IpcRequestKind::kSubmit:
      if (!reader.u64(out.job_id)) {
        return IpcDecodeError::kTruncated;
      }
      break;
    case IpcRequestKind::kPs: {
      std::uint32_t count = 0;
      error = reader.count(IpcProtocolLimits::kMaxListItems, count);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      out.jobs.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        IpcJobSummary summary;
        if (!reader.u64(summary.job_id) || !reader.u8(summary.state) ||
            !reader.u32(summary.owner_uid) || !reader.u64(summary.revision)) {
          return IpcDecodeError::kTruncated;
        }
        if (summary.state > kUnderlyingJobStateMax) {
          return IpcDecodeError::kInvalidValue;
        }
        bool masked = false;
        error = reader.flag(masked);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        summary.masked = masked;
        if (!masked) {
          std::uint32_t argv_count = 0;
          error = reader.count(IpcProtocolLimits::kMaxItemCount, argv_count);
          if (error != IpcDecodeError::kNone) {
            return error;
          }
          summary.argv.reserve(argv_count);
          for (std::uint32_t j = 0; j < argv_count; ++j) {
            summary.argv.emplace_back();
            error = reader.string(summary.argv.back());
            if (error != IpcDecodeError::kNone) {
              return error;
            }
          }
          error = reader.string(summary.cwd);
          if (error != IpcDecodeError::kNone) {
            return error;
          }
          error = reader.string(summary.tensorboard_logdir);
          if (error != IpcDecodeError::kNone) {
            return error;
          }
        }
        bool has_exit = false;
        error = reader.flag(has_exit);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        if (has_exit) {
          IpcExitStatus exit_status;
          std::uint8_t normal = 0;
          if (!reader.u8(normal)) {
            return IpcDecodeError::kTruncated;
          }
          if (normal > 1) {
            return IpcDecodeError::kInvalidValue;
          }
          exit_status.exited_normally = normal == 1;
          if (!reader.i32(exit_status.code)) {
            return IpcDecodeError::kTruncated;
          }
          summary.exit = exit_status;
        }
        if (version >= 3) {
          // v3（DEC-012）：等待原因 + 可选目标明细。
          if (!reader.u8(summary.wait_reason)) {
            return IpcDecodeError::kTruncated;
          }
          if (summary.wait_reason > kUnderlyingWaitReasonMax) {
            return IpcDecodeError::kInvalidValue;
          }
          error = reader.string(summary.wait_detail);
          if (error != IpcDecodeError::kNone) {
            return error;
          }
        }
        out.jobs.push_back(std::move(summary));
      }
      break;
    }
    case IpcRequestKind::kQueue: {
      std::uint32_t count = 0;
      error = reader.count(IpcProtocolLimits::kMaxListItems, count);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      out.queue.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        IpcQueueEntry entry;
        if (!reader.u64(entry.job_id) || !reader.u32(entry.owner_uid) ||
            !reader.u64(entry.submit_time_unix_ns) || !reader.u8(entry.state)) {
          return IpcDecodeError::kTruncated;
        }
        if (entry.state > kUnderlyingJobStateMax) {
          return IpcDecodeError::kInvalidValue;
        }
        if (version >= 3) {
          if (!reader.u8(entry.wait_reason)) {
            return IpcDecodeError::kTruncated;
          }
          if (entry.wait_reason > kUnderlyingWaitReasonMax) {
            return IpcDecodeError::kInvalidValue;
          }
          error = reader.string(entry.wait_detail);
          if (error != IpcDecodeError::kNone) {
            return error;
          }
        }
        out.queue.push_back(std::move(entry));
      }
      break;
    }
    case IpcRequestKind::kGpu: {
      if (!reader.u64(out.gpu_revision)) {
        return IpcDecodeError::kTruncated;
      }
      std::uint32_t count = 0;
      error = reader.count(IpcProtocolLimits::kMaxListItems, count);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      out.devices.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        IpcGpuDevice device;
        error = reader.string(device.uuid);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        bool has_util = false;
        bool has_memory = false;
        bool has_lease = false;
        if (!reader.u32(device.index) || !reader.u8(device.observed_state) ||
            !reader.u8(device.logical_state)) {
          return IpcDecodeError::kTruncated;
        }
        if (device.observed_state > kUnderlyingGpuObservedStateMax ||
            device.logical_state > kUnderlyingGpuLogicalStateMax) {
          return IpcDecodeError::kInvalidValue;
        }
        error = reader.flag(has_util);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        if (has_util) {
          std::uint32_t utilization = 0;
          if (!reader.u32(utilization)) {
            return IpcDecodeError::kTruncated;
          }
          if (utilization > 100) {
            return IpcDecodeError::kInvalidValue;
          }
          device.utilization_percent = utilization;
        }
        error = reader.flag(has_memory);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        if (has_memory) {
          std::uint64_t used = 0;
          std::uint64_t total = 0;
          if (!reader.u64(used) || !reader.u64(total)) {
            return IpcDecodeError::kTruncated;
          }
          if (total < used) {
            return IpcDecodeError::kInvalidValue;
          }
          device.memory_used_bytes = used;
          device.memory_total_bytes = total;
        }
        error = reader.flag(has_lease);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        if (has_lease) {
          std::uint64_t job = 0;
          if (!reader.u64(job)) {
            return IpcDecodeError::kTruncated;
          }
          device.leased_by_job = job;
        }
        out.devices.push_back(std::move(device));
      }
      break;
    }
    case IpcRequestKind::kCancel:
      if (!reader.u8(out.state)) {
        return IpcDecodeError::kTruncated;
      }
      if (out.state > kUnderlyingJobStateMax) {
        return IpcDecodeError::kInvalidValue;
      }
      break;
    case IpcRequestKind::kLogs: {
      IpcDecodeError flag_error = reader.flag(out.logs.stdout_truncated);
      if (flag_error != IpcDecodeError::kNone) {
        return flag_error;
      }
      error = reader.bytes(out.logs.stdout_tail);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      flag_error = reader.flag(out.logs.stderr_truncated);
      if (flag_error != IpcDecodeError::kNone) {
        return flag_error;
      }
      error = reader.bytes(out.logs.stderr_tail);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      break;
    }
    case IpcRequestKind::kLogsFollow:
      if (!reader.u8(out.logs_follow.job_state) || !reader.u64(out.logs_follow.stdout_offset) ||
          !reader.u64(out.logs_follow.stderr_offset)) {
        return IpcDecodeError::kTruncated;
      }
      if (out.logs_follow.job_state > kUnderlyingJobStateMax) {
        return IpcDecodeError::kInvalidValue;
      }
      break;
    case IpcRequestKind::kTensorboard: {
      error = reader.string(out.tensorboard.logdir);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      error = reader.string(out.tensorboard.cwd);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      break;
    }
    case IpcRequestKind::kInspect: {
      IpcInspectPayload& inspect = out.inspect;
      if (!reader.u64(inspect.job_id) || !reader.u8(inspect.state) ||
          !reader.u32(inspect.owner_uid) || !reader.u64(inspect.revision)) {
        return IpcDecodeError::kTruncated;
      }
      if (inspect.state > kUnderlyingJobStateMax) {
        return IpcDecodeError::kInvalidValue;
      }
      error = reader.string(inspect.cwd);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      error = reader.string(inspect.executable);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      std::uint32_t argv_count = 0;
      error = reader.count(IpcProtocolLimits::kMaxItemCount, argv_count);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      inspect.argv.reserve(argv_count);
      for (std::uint32_t i = 0; i < argv_count; ++i) {
        inspect.argv.emplace_back();
        error = reader.string(inspect.argv.back());
        if (error != IpcDecodeError::kNone) {
          return error;
        }
      }
      bool has_metadata = false;
      error = reader.flag(has_metadata);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (has_metadata) {
        IpcEnvMetadata metadata;
        if (!reader.u8(metadata.source)) {
          return IpcDecodeError::kTruncated;
        }
        if (metadata.source > 2) {
          return IpcDecodeError::kInvalidValue;
        }
        error = reader.string(metadata.python_version);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        inspect.env_metadata = std::move(metadata);
      }
      std::uint32_t env_count = 0;
      error = reader.count(IpcProtocolLimits::kMaxListItems, env_count);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      inspect.env.reserve(env_count);
      for (std::uint32_t i = 0; i < env_count; ++i) {
        IpcEnvEntry entry;
        error = reader.string(entry.name);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        bool masked = false;
        error = reader.flag(masked);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        entry.masked = masked;
        error = reader.string(entry.value);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        inspect.env.push_back(std::move(entry));
      }
      bool has_gpu_uuid = false;
      error = reader.flag(has_gpu_uuid);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (has_gpu_uuid) {
        std::string uuid;
        error = reader.string(uuid);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        inspect.gpu_uuid = std::move(uuid);
      }
      bool has_gpu_index = false;
      error = reader.flag(has_gpu_index);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (has_gpu_index) {
        std::uint32_t index = 0;
        if (!reader.u32(index)) {
          return IpcDecodeError::kTruncated;
        }
        inspect.gpu_index = index;
      }
      if (!reader.u64(inspect.submit_time_unix_ns)) {
        return IpcDecodeError::kTruncated;
      }
      bool has_exit = false;
      error = reader.flag(has_exit);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (has_exit) {
        std::uint8_t normal = 0;
        if (!reader.u8(normal)) {
          return IpcDecodeError::kTruncated;
        }
        if (normal > 1) {
          return IpcDecodeError::kInvalidValue;
        }
        IpcExitStatus exit_status;
        exit_status.exited_normally = normal == 1;
        if (!reader.i32(exit_status.code)) {
          return IpcDecodeError::kTruncated;
        }
        inspect.exit = exit_status;
      }
      error = reader.string(inspect.log_path);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (version >= 3) {
        // v3（DEC-012）：placement 输入（mode + required 时目标 UUID）。
        if (!reader.u8(inspect.gpu_placement_mode)) {
          return IpcDecodeError::kTruncated;
        }
        if (inspect.gpu_placement_mode > kUnderlyingGpuPlacementModeMax) {
          return IpcDecodeError::kInvalidValue;
        }
        error = reader.string(inspect.gpu_placement_device);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
      }
      break;
    }
  }
  if (!reader.exhausted()) {
    return IpcDecodeError::kTrailingBytes;
  }
  return IpcDecodeError::kNone;
}

// 编码公共骨架：写 [version][kind][body]；失败保持 out 不变。version 取自
// 消息（缺省当前版本；服务端回显请求版本）。
template <typename Message>
bool encode_payload(IpcRequestKind kind, const Message& message,
                    bool (*encode_body)(std::uint8_t, const Message&, Writer&),
                    std::vector<std::uint8_t>& out) {
  if (!ipc_version_supported(message.version)) {
    return false;
  }
  std::vector<std::uint8_t> payload;
  Writer writer(payload);
  writer.u8(message.version);
  writer.u8(static_cast<std::uint8_t>(kind));
  if (!encode_body(message.version, message, writer) ||
      payload.size() > IpcProtocolLimits::kMaxPayloadBytes) {
    return false;
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return true;
}

// ---------------------------------------------------------------------------
// 流式帧族（M6）。数据帧上限独立于 LOGS 快照，控制帧定长。
// ---------------------------------------------------------------------------

constexpr std::uint8_t kUnderlyingStreamMax = 1;  // observe::LogStreamKind::kStderr

bool encode_stream_frame_body(const IpcStreamFrame& frame, Writer& writer) {
  switch (frame.kind) {
    case IpcStreamFrameKind::kLogData:
      if (frame.stream > kUnderlyingStreamMax ||
          frame.data.size() > IpcProtocolLimits::kMaxStreamDataBytes ||
          frame.end_offset < frame.begin_offset) {
        return false;
      }
      writer.u8(frame.stream);
      writer.u64(frame.begin_offset);
      writer.u64(frame.end_offset);
      return !writer.bytes(frame.data);
    case IpcStreamFrameKind::kLogGap:
      if (frame.stream > kUnderlyingStreamMax || frame.end_offset < frame.begin_offset) {
        return false;
      }
      writer.u8(frame.stream);
      writer.u64(frame.begin_offset);
      writer.u64(frame.end_offset);
      return true;
    case IpcStreamFrameKind::kLogBackpressure:
      if (frame.stream > kUnderlyingStreamMax) {
        return false;
      }
      writer.u8(frame.stream);
      writer.u64(frame.begin_offset);
      return true;
    case IpcStreamFrameKind::kLogEof:
      if (frame.job_state > kUnderlyingJobStateMax) {
        return false;
      }
      writer.u8(frame.job_state);
      if (!frame.exit) {
        writer.u8(0);
        return true;
      }
      writer.u8(1);
      writer.u8(frame.exit->exited_normally ? 1 : 0);
      writer.i32(frame.exit->code);
      return true;
  }
  return false;
}

IpcDecodeError decode_stream_frame_body(Reader& reader, IpcStreamFrame& out) {
  std::uint8_t version = 0;
  std::uint8_t kind = 0;
  if (!reader.u8(version) || !reader.u8(kind)) {
    return IpcDecodeError::kTruncated;
  }
  // 流式帧族冻结于 v1（M6）。
  if (version != IpcProtocolLimits::kStreamFrameVersion) {
    return IpcDecodeError::kBadVersion;
  }
  if (kind < static_cast<std::uint8_t>(IpcStreamFrameKind::kLogData) ||
      kind > static_cast<std::uint8_t>(IpcStreamFrameKind::kLogEof)) {
    return IpcDecodeError::kBadKind;
  }
  out = IpcStreamFrame{};
  out.kind = static_cast<IpcStreamFrameKind>(kind);

  switch (out.kind) {
    case IpcStreamFrameKind::kLogData:
      if (!reader.u8(out.stream) || !reader.u64(out.begin_offset) || !reader.u64(out.end_offset)) {
        return IpcDecodeError::kTruncated;
      }
      if (out.stream > kUnderlyingStreamMax || out.end_offset < out.begin_offset) {
        return IpcDecodeError::kInvalidValue;
      }
      {
        // bytes() 以 kMaxBytesFieldBytes 兜底防越界分配；流式数据上限在此
        // 独立收紧为 kMaxStreamDataBytes。
        const IpcDecodeError error = reader.bytes(out.data);
        if (error != IpcDecodeError::kNone) {
          return error;
        }
        if (out.data.size() > IpcProtocolLimits::kMaxStreamDataBytes) {
          return IpcDecodeError::kOversize;
        }
      }
      break;
    case IpcStreamFrameKind::kLogGap:
      if (!reader.u8(out.stream) || !reader.u64(out.begin_offset) || !reader.u64(out.end_offset)) {
        return IpcDecodeError::kTruncated;
      }
      if (out.stream > kUnderlyingStreamMax || out.end_offset < out.begin_offset) {
        return IpcDecodeError::kInvalidValue;
      }
      break;
    case IpcStreamFrameKind::kLogBackpressure:
      if (!reader.u8(out.stream) || !reader.u64(out.begin_offset)) {
        return IpcDecodeError::kTruncated;
      }
      if (out.stream > kUnderlyingStreamMax) {
        return IpcDecodeError::kInvalidValue;
      }
      break;
    case IpcStreamFrameKind::kLogEof: {
      if (!reader.u8(out.job_state)) {
        return IpcDecodeError::kTruncated;
      }
      if (out.job_state > kUnderlyingJobStateMax) {
        return IpcDecodeError::kInvalidValue;
      }
      bool has_exit = false;
      IpcDecodeError error = reader.flag(has_exit);
      if (error != IpcDecodeError::kNone) {
        return error;
      }
      if (has_exit) {
        std::uint8_t normal = 0;
        if (!reader.u8(normal)) {
          return IpcDecodeError::kTruncated;
        }
        if (normal > 1) {
          return IpcDecodeError::kInvalidValue;
        }
        IpcExitStatus exit_status;
        exit_status.exited_normally = normal == 1;
        if (!reader.i32(exit_status.code)) {
          return IpcDecodeError::kTruncated;
        }
        out.exit = exit_status;
      }
      break;
    }
  }
  if (!reader.exhausted()) {
    return IpcDecodeError::kTrailingBytes;
  }
  return IpcDecodeError::kNone;
}

}  // namespace

const char* to_string(IpcRequestKind kind) noexcept {
  switch (kind) {
    case IpcRequestKind::kSubmit:
      return "submit";
    case IpcRequestKind::kPs:
      return "ps";
    case IpcRequestKind::kQueue:
      return "queue";
    case IpcRequestKind::kGpu:
      return "gpu";
    case IpcRequestKind::kCancel:
      return "cancel";
    case IpcRequestKind::kLogs:
      return "logs";
    case IpcRequestKind::kLogsFollow:
      return "logs-follow";
    case IpcRequestKind::kTensorboard:
      return "tensorboard";
    case IpcRequestKind::kInspect:
      return "inspect";
  }
  return "unknown";
}

const char* to_string(IpcWaitReason reason) noexcept {
  switch (reason) {
    case IpcWaitReason::kNone:
      return "NONE";
    case IpcWaitReason::kNoFreeGpu:
      return "NO_FREE_GPU";
    case IpcWaitReason::kAffinityGpuAllocated:
      return "AFFINITY_GPU_ALLOCATED";
    case IpcWaitReason::kAffinityGpuExternal:
      return "AFFINITY_GPU_EXTERNAL";
    case IpcWaitReason::kAffinityGpuState:
      return "AFFINITY_GPU_STATE";
  }
  return "UNKNOWN";
}

const char* to_string(IpcStreamFrameKind kind) noexcept {
  switch (kind) {
    case IpcStreamFrameKind::kLogData:
      return "log-data";
    case IpcStreamFrameKind::kLogGap:
      return "log-gap";
    case IpcStreamFrameKind::kLogBackpressure:
      return "log-backpressure";
    case IpcStreamFrameKind::kLogEof:
      return "log-eof";
  }
  return "unknown";
}

const char* to_string(IpcError error) noexcept {
  switch (error) {
    case IpcError::kNone:
      return "none";
    case IpcError::kProtocol:
      return "protocol";
    case IpcError::kUnsupported:
      return "unsupported";
    case IpcError::kDenied:
      return "denied";
    case IpcError::kInvalidSpec:
      return "invalid spec";
    case IpcError::kQueueRejected:
      return "queue rejected";
    case IpcError::kStoreFailed:
      return "store failed";
    case IpcError::kNotFound:
      return "not found";
    case IpcError::kInvalidState:
      return "invalid state";
    case IpcError::kNotAvailable:
      return "not available";
    case IpcError::kLimit:
      return "limit";
    case IpcError::kInternal:
      return "internal";
  }
  return "unknown";
}

const char* to_string(IpcDecodeError error) noexcept {
  switch (error) {
    case IpcDecodeError::kNone:
      return "none";
    case IpcDecodeError::kTruncated:
      return "truncated";
    case IpcDecodeError::kOversize:
      return "oversize";
    case IpcDecodeError::kBadVersion:
      return "bad version";
    case IpcDecodeError::kBadKind:
      return "bad kind";
    case IpcDecodeError::kBadString:
      return "bad string";
    case IpcDecodeError::kTooManyItems:
      return "too many items";
    case IpcDecodeError::kTrailingBytes:
      return "trailing bytes";
    case IpcDecodeError::kInvalidValue:
      return "invalid value";
  }
  return "unknown";
}

bool encode_request_payload(const IpcRequest& request, std::vector<std::uint8_t>& out) {
  return encode_payload(request.kind, request, &encode_request_body, out);
}

bool encode_response_payload(const IpcResponse& response, std::vector<std::uint8_t>& out) {
  return encode_payload(response.kind, response, &encode_response_body, out);
}

bool append_request_frame(const IpcRequest& request, std::vector<std::uint8_t>& out) {
  std::vector<std::uint8_t> payload;
  if (!encode_payload(request.kind, request, &encode_request_body, payload)) {
    return false;
  }
  Writer framed(out);
  framed.u32(static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return true;
}

bool append_response_frame(const IpcResponse& response, std::vector<std::uint8_t>& out) {
  std::vector<std::uint8_t> payload;
  if (!encode_payload(response.kind, response, &encode_response_body, payload)) {
    return false;
  }
  Writer framed(out);
  framed.u32(static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return true;
}

IpcRequestDecodeResult decode_request_payload(const std::uint8_t* data, std::size_t size) {
  IpcRequestDecodeResult result;
  if (size > IpcProtocolLimits::kMaxPayloadBytes) {
    result.error = IpcDecodeError::kOversize;
    return result;
  }
  Reader reader(data, size);
  result.error = decode_request_body(reader, result.value);
  return result;
}

IpcResponseDecodeResult decode_response_payload(const std::uint8_t* data, std::size_t size) {
  IpcResponseDecodeResult result;
  if (size > IpcProtocolLimits::kMaxPayloadBytes) {
    result.error = IpcDecodeError::kOversize;
    return result;
  }
  Reader reader(data, size);
  result.error = decode_response_body(reader, result.value);
  return result;
}

bool encode_stream_frame_payload(const IpcStreamFrame& frame, std::vector<std::uint8_t>& out) {
  std::vector<std::uint8_t> payload;
  Writer writer(payload);
  // 流式帧族冻结于 v1（M6），不随请求/响应 v2 变化。
  writer.u8(IpcProtocolLimits::kStreamFrameVersion);
  writer.u8(static_cast<std::uint8_t>(frame.kind));
  if (!encode_stream_frame_body(frame, writer) ||
      payload.size() > IpcProtocolLimits::kMaxPayloadBytes) {
    return false;
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return true;
}

bool append_stream_frame(const IpcStreamFrame& frame, std::vector<std::uint8_t>& out) {
  std::vector<std::uint8_t> payload;
  if (!encode_stream_frame_payload(frame, payload)) {
    return false;
  }
  Writer framed(out);
  framed.u32(static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return true;
}

IpcStreamFrameDecodeResult decode_stream_frame_payload(const std::uint8_t* data, std::size_t size) {
  IpcStreamFrameDecodeResult result;
  if (size > IpcProtocolLimits::kMaxPayloadBytes) {
    result.error = IpcDecodeError::kOversize;
    return result;
  }
  Reader reader(data, size);
  result.error = decode_stream_frame_body(reader, result.value);
  return result;
}

}  // namespace yori::ipc
