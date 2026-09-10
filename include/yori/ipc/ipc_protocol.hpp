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
  // 流式帧 LOG_DATA 数据上限（覆盖 LogPump 64 KiB 读块与回放分块）。
  static constexpr std::uint32_t kMaxStreamDataBytes = 256 * 1024;
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
  // M6：流式跟随（初始响应帧后接流式帧族，见 IpcStreamFrame）。
  kLogsFollow = 7,
  // M6：TensorBoard logdir 解析查询（DEC-003；优先级判定在 CLI 侧）。
  kTensorboard = 8,
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

// logs -f 会话请求（M6）。两路流 offset 独立（设计 11.3 的 --since-offset
// 按流具体化）；未设置的流从当前末尾开始跟随。不携带身份字段。
struct IpcLogsFollowRequest final {
  std::uint64_t job_id{0};
  std::optional<std::uint64_t> since_stdout;
  std::optional<std::uint64_t> since_stderr;
};

// TensorBoard logdir 解析查询（M6，DEC-003）。daemon 返回解析原料，优先级
// （--logdir 参数 > spec.tensorboard_logdir > cwd）由 CLI 判定。
struct IpcTensorboardRequest final {
  std::uint64_t job_id{0};
};

struct IpcRequest final {
  IpcRequestKind kind{IpcRequestKind::kSubmit};
  // 按 kind 取用；未用字段保持默认。
  IpcSubmitRequest submit;
  IpcCancelRequest cancel;
  IpcLogsRequest logs;
  IpcLogsFollowRequest logs_follow;
  IpcTensorboardRequest tensorboard;
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

// LOGS_FOLLOW 接受后的会话上下文（初始响应帧）：两路流的实际起始 offset
// （GAP 校正后）与当时 Job 状态。offset 之后由各流式帧携带。
struct IpcLogsFollowPayload final {
  std::uint8_t job_state{0};
  std::uint64_t stdout_offset{0};
  std::uint64_t stderr_offset{0};
};

// TENSORBOARD 查询结果：logdir 为 spec.tensorboard_logdir（可能为空），
// cwd 为 Job 工作目录；两者均只对 owner/admin 可见（脱敏不适用，直接拒绝）。
struct IpcTensorboardPayload final {
  std::optional<std::string> logdir;
  std::string cwd;
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
  IpcLogsFollowPayload logs_follow;   // LOGS_FOLLOW ack
  IpcTensorboardPayload tensorboard;  // TENSORBOARD
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

// ---------------------------------------------------------------------------
// 流式帧族（M6，设计 13.2）：仅 daemon -> CLI，在 LOGS_FOLLOW 的初始响应帧
// 之后连续推送，同受 [u32 LE payload_bytes] 帧界约束。payload =
//   [u8 version=1][u8 frame_kind][kind body]
// LOG_DATA 携带 [begin_offset, end_offset) 数据（丢弃标记 chunk 为
// begin==end 且 data 非空的帧）；LOG_GAP 告知不可回放区间并发送即跳到
// to_offset；LOG_BACKPRESSURE 携带当前 offset，发送后会话即断开；
// LOG_EOF 携带 Job 终态（可选退出状态），发送后排空关闭。
// ---------------------------------------------------------------------------

enum class IpcStreamFrameKind : std::uint8_t {
  kLogData = 1,
  kLogGap = 2,
  kLogBackpressure = 3,
  kLogEof = 4,
};

[[nodiscard]] const char* to_string(IpcStreamFrameKind kind) noexcept;

struct IpcStreamFrame final {
  IpcStreamFrameKind kind{IpcStreamFrameKind::kLogData};
  // 流标识：0 = stdout，1 = stderr（observe::LogStreamKind 的数值；控制帧
  // 中仅 DATA/GAP/BACKPRESSURE 有意义，EOF 为整会话语义）。
  std::uint8_t stream{0};
  // DATA：区间 [begin_offset, end_offset)；GAP：[from, to)；
  // BACKPRESSURE：当前 offset。
  std::uint64_t begin_offset{0};
  std::uint64_t end_offset{0};
  std::vector<std::uint8_t> data;     // DATA
  std::uint8_t job_state{0};          // EOF：job::JobState 数值
  std::optional<IpcExitStatus> exit;  // EOF
};

struct IpcStreamFrameDecodeResult final {
  IpcDecodeError error{IpcDecodeError::kTruncated};
  IpcStreamFrame value{};

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
[[nodiscard]] IpcStreamFrameDecodeResult decode_stream_frame_payload(const std::uint8_t* data,
                                                                     std::size_t size);

// 流式帧的编码镜像（payload 本体 / 含长度前缀的完整帧），约束同上。
[[nodiscard]] bool encode_stream_frame_payload(const IpcStreamFrame& frame,
                                               std::vector<std::uint8_t>& out);
[[nodiscard]] bool append_stream_frame(const IpcStreamFrame& frame, std::vector<std::uint8_t>& out);

}  // namespace yori::ipc
