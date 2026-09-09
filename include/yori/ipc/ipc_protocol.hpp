#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace yori::ipc {

// ---------------------------------------------------------------------------
// IPC 协议 v1（设计第 13 节，M5 冻结）。
//
// 帧格式（两个方向一致）：
//   [u32 LE payload_bytes][payload]
//   payload = [u8 version=1][u8 kind][kind body]
//
// 协议层只承担结构安全：负载/字符串/计数/字节数组上限、NUL 禁止、全量消费、
// 枚举值域校验；语义上限（argv/env/cwd/logdir 的精确 JobSpec 限制）由
// daemon 侧 IpcService 调用 job::validate 承担。请求结构不携带任何身份字段：
// Job owner 只来自 SO_PEERCRED（DEC-010、威胁模型基线 2）。
// ---------------------------------------------------------------------------

struct IpcProtocolLimits final {
  // 单帧负载上限。须覆盖最大合法 SUBMIT（JobSpec argv 64 KiB + env 256 KiB
  // + cwd/logdir 等）与 LOGS 响应（两路尾部各 256 KiB），留余量。
  static constexpr std::uint32_t kMaxPayloadBytes = 1u << 20;  // 1 MiB
  static constexpr std::uint32_t kMinPayloadBytes = 2;         // version + kind
  static constexpr std::uint32_t kProtocolVersion = 1;
  // 单个字符串字段（argv/env/cwd/uuid/detail 等）。
  static constexpr std::uint32_t kMaxStringBytes = 64 * 1024;
  // 请求内数组/映射计数（与 JobSpecLimits 的 argv/env 计数对齐）。
  static constexpr std::uint32_t kMaxItemCount = 256;
  // 响应列表字段计数（ps/queue/gpu 列表的服务端上限一致）。
  static constexpr std::uint32_t kMaxListItems = 1024;
  // LOGS 响应尾部字节数组的服务端上限（每流）。
  static constexpr std::uint32_t kMaxLogTailBytes = 256 * 1024;
  // 解码器接受的字节数组上限（两路尾部合计加协议开销须 < kMaxPayloadBytes）。
  static constexpr std::uint32_t kMaxBytesFieldBytes = 512 * 1024;
};

[[nodiscard]] constexpr bool ipc_payload_length_valid(std::uint32_t payload_bytes) noexcept {
  return payload_bytes >= IpcProtocolLimits::kMinPayloadBytes &&
         payload_bytes <= IpcProtocolLimits::kMaxPayloadBytes;
}

enum class IpcRequestKind : std::uint8_t {
  kSubmit = 1,
  kPs = 2,
  kQueue = 3,
  kGpu = 4,
  kCancel = 5,
  kLogs = 6,
};

[[nodiscard]] const char* to_string(IpcRequestKind kind) noexcept;

struct IpcSubmitRequest final {
  std::vector<std::string> argv;
  std::string cwd;
  std::map<std::string, std::string> env;
  std::uint32_t gpu_request{1};
  std::optional<std::string> launch_profile;
  std::optional<std::string> tensorboard_logdir;
};

struct IpcCancelRequest final {
  std::uint64_t job_id{0};
};

struct IpcLogsRequest final {
  std::uint64_t job_id{0};
  // 每流请求字节数；daemon 以 min(请求, kMaxLogTailBytes) 为准。
  std::uint32_t max_bytes{0};
};

struct IpcRequest final {
  IpcRequestKind kind{IpcRequestKind::kSubmit};
  // 按 kind 取用；未用字段保持默认。
  IpcSubmitRequest submit;
  IpcCancelRequest cancel;
  IpcLogsRequest logs;
};

enum class IpcError : std::uint8_t {
  kNone = 0,
  // 请求帧无法解析（版本/kind/边界/值域）。连接随后被 daemon 断开。
  kProtocol = 1,
  // 请求 kind 合法但当前构建不支持（如 M5 对 RUNNING Job 的 cancel）。
  kUnsupported = 2,
  // owner/admin 授权失败。
  kDenied = 3,
  // JobSpec 语义校验失败（detail 携带 JobSpecErrorCode 名称）。
  kInvalidSpec = 4,
  // 队列准入拒绝（容量等；detail 说明）。
  kQueueRejected = 5,
  // StateStore 读写失败。
  kStoreFailed = 6,
  // JobId 不存在。
  kNotFound = 7,
  // Job 状态不满足该操作（如对终态 Job cancel、logs 于未启动 Job）。
  kInvalidState = 8,
  // 依赖的数据源暂不可用（如尚无 GPU 观测、日志读取失败）。
  kNotAvailable = 9,
  // 有界列表被截断（ps/queue 超出列表上限时返回前缀并置此码）。
  kLimit = 10,
  // daemon 内部错误（handler 异常等，不泄露内部细节）。
  kInternal = 11,
};

[[nodiscard]] const char* to_string(IpcError error) noexcept;

struct IpcExitStatus final {
  bool exited_normally{false};
  // exited_normally 为 true 时是退出码；否则是信号号。
  std::int32_t code{0};
};

// ps 响应的单 Job 视图。masked=true 时 argv/cwd/tensorboard_logdir 为空
// （设计第 11.5 节脱敏：非 owner 非 admin 仅见 JobId/状态/revision/owner/
// 退出状态）。state 为 job::JobState 的数值（to_underlying 由服务端保证）。
struct IpcJobSummary final {
  std::uint64_t job_id{0};
  std::uint8_t state{0};
  std::uint32_t owner_uid{0};
  std::uint64_t revision{0};
  bool masked{false};
  std::vector<std::string> argv;
  std::string cwd;
  std::optional<std::string> tensorboard_logdir;
  std::optional<IpcExitStatus> exit;
};

struct IpcQueueEntry final {
  std::uint64_t job_id{0};
  std::uint32_t owner_uid{0};
  std::uint64_t submit_time_unix_ns{0};
  std::uint8_t state{0};
};

// gpu 响应的单设备视图。observed_state 为 gpu::GpuObservedState 数值，
// logical_state 为 gpu::GpuLogicalState 数值（lease 合并后的调度视图）。
struct IpcGpuDevice final {
  std::string uuid;
  std::uint32_t index{0};
  std::uint8_t observed_state{0};
  std::uint8_t logical_state{0};
  std::optional<std::uint32_t> utilization_percent;
  std::optional<std::uint64_t> memory_used_bytes;
  std::optional<std::uint64_t> memory_total_bytes;
  // 持有该设备 lease 的 JobId。
  std::optional<std::uint64_t> leased_by_job;
};

struct IpcLogsPayload final {
  bool stdout_truncated{false};
  std::vector<std::uint8_t> stdout_tail;
  bool stderr_truncated{false};
  std::vector<std::uint8_t> stderr_tail;
};

// 响应按 kind 取用结果字段；error 非 kNone 时除 kind/detail（及 CANCEL 的
// state 上下文）外字段无意义。
struct IpcResponse final {
  IpcRequestKind kind{IpcRequestKind::kSubmit};
  IpcError error{IpcError::kNone};
  std::string detail;
  std::uint64_t job_id{0};            // SUBMIT 成功时的 JobId
  std::vector<IpcJobSummary> jobs;    // PS
  std::vector<IpcQueueEntry> queue;   // QUEUE
  std::uint64_t gpu_revision{0};      // GPU
  std::vector<IpcGpuDevice> devices;  // GPU
  std::uint8_t state{0};              // CANCEL：终态或拒绝时的当前状态
  IpcLogsPayload logs;                // LOGS
};

enum class IpcDecodeError : std::uint8_t {
  kNone = 0,
  // 输入早于结构结束耗尽。
  kTruncated = 1,
  // 声明长度超过协议上限（由帧层判定后透传，或字段长度超限）。
  kOversize = 2,
  kBadVersion = 3,
  kBadKind = 4,
  // 字符串/字节数组非法（含 NUL、长度超上限）。
  kBadString = 5,
  // 计数超过上限。
  kTooManyItems = 6,
  // 解码完成后仍有未消费字节。
  kTrailingBytes = 7,
  // 值域非法（状态字节、布尔标记、枚举等）。
  kInvalidValue = 8,
};

[[nodiscard]] const char* to_string(IpcDecodeError error) noexcept;

struct IpcRequestDecodeResult final {
  IpcDecodeError error{IpcDecodeError::kTruncated};
  IpcRequest value{};

  [[nodiscard]] constexpr bool ok() const noexcept { return error == IpcDecodeError::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

struct IpcResponseDecodeResult final {
  IpcDecodeError error{IpcDecodeError::kTruncated};
  IpcResponse value{};

  [[nodiscard]] constexpr bool ok() const noexcept { return error == IpcDecodeError::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// 编码 payload 本体（[version][kind][body]，不含长度前缀）。任何字段越界
// （长度、计数、NUL、总负载超限）返回 false 且 out 保持调用前状态。传输层
// 以此配合自身的长度前缀读写，避免双重封装。
[[nodiscard]] bool encode_request_payload(const IpcRequest& request,
                                          std::vector<std::uint8_t>& out);
[[nodiscard]] bool encode_response_payload(const IpcResponse& response,
                                           std::vector<std::uint8_t>& out);

// 追加一个完整帧（含 u32 长度前缀）到 out；失败同样保持 out 不变。
[[nodiscard]] bool append_request_frame(const IpcRequest& request, std::vector<std::uint8_t>& out);
[[nodiscard]] bool append_response_frame(const IpcResponse& response,
                                         std::vector<std::uint8_t>& out);

// 解码一帧的 payload（不含长度前缀）。任何长度/内容的输入都只产生稳定错误码
// 或完整解析结果，不访问越界内存。
[[nodiscard]] IpcRequestDecodeResult decode_request_payload(const std::uint8_t* data,
                                                            std::size_t size);
[[nodiscard]] IpcResponseDecodeResult decode_response_payload(const std::uint8_t* data,
                                                              std::size_t size);

}  // namespace yori::ipc
