#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_protocol.hpp>
#include <yori/ipc/ipc_transport.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/store/state_store.hpp>

namespace yori::ipc {

// ---------------------------------------------------------------------------
// daemon 侧请求服务（Core，设计第 13 节、DEC-010）。
//
// 单 owner 同步组件：由 UDS 适配的 blocking worker 串行调用（EXEC-02），
// 不自建线程/队列/锁；所有失败映射为 IpcResponse 的显式错误码。授权与脱敏
// 只依赖 PeerCredentials（SO_PEERCRED），请求结构不携带身份字段。
// ---------------------------------------------------------------------------

struct IpcServiceConfig final {
  // DEC-010：admin = 主 GID 匹配任一 admin_gid，或 UID 属于 admin_uids
  // （daemon 启动时从系统组数据库解析的成员集）。
  std::vector<std::uint32_t> admin_gids;
  std::vector<std::uint32_t> admin_uids;
  // ps/queue 列表的服务端上限（超出截断并置 kLimit）。
  std::uint32_t max_listed_jobs{IpcProtocolLimits::kMaxListItems};
  // logs 快照每流上限（请求值取 min）。
  std::uint32_t max_log_tail_bytes{IpcProtocolLimits::kMaxLogTailBytes};

  [[nodiscard]] bool valid() const noexcept;
};

// GPU 调度视图数据源（daemon 以 GpuManager 的 DoubleBuffer 快照实现；测试
// 注入）。返回 false 表示尚无有效观测（GPU 响应置 kNotAvailable）。
class GpuStatusSource {
 public:
  virtual ~GpuStatusSource() = default;

  [[nodiscard]] virtual bool try_get_snapshot(gpu::GpuObservationSnapshot& out) = 0;
};

struct LogTailResult final {
  bool ok{false};
  bool truncated{false};
  std::vector<std::uint8_t> tail;
};

// Job 日志尾部读取（daemon 以文件实现；测试注入）。path 为该流日志文件
// 绝对路径（LogSink 目录 + log_file_name）。文件缺失视为空（未产生输出），
// 读取失败返回 ok=false。
class LogSnapshotReader {
 public:
  virtual ~LogSnapshotReader() = default;

  [[nodiscard]] virtual LogTailResult read_tail(const std::string& path,
                                                std::uint32_t max_bytes) = 0;
};

// 文件实现：从文件末尾有界读取（O_NOFOLLOW，基线 8/23）。
// 大于 max_bytes 时返回尾部并置 truncated。
[[nodiscard]] std::unique_ptr<LogSnapshotReader> file_log_snapshot_reader();

// ---------------------------------------------------------------------------
// 提交与取消的守护委派接口（M7 守护总装收口）。`StateStore` 与
// `GlobalJobQueue` 是单 owner 契约：全部运行期变更由唯一的写者上下文
// （daemon 的 JobManager worker，经 StoreTaskRunner）执行；IPC 服务在完成
// 授权与校验后把状态变更委派到这里，同步等待有界结果。授权（owner/admin）、
// 请求校验与脱敏仍属于 IpcService；本接口只承载状态变更及其结果。
// ---------------------------------------------------------------------------

struct JobSubmitOutcome final {
  enum class Code : std::uint8_t {
    kSubmitted,
    kStoreFailed,
    kQueueRejected,
    kUnavailable,  // 承载未启动、停止中或应答超时
  } code{Code::kUnavailable};
  std::uint64_t job_id{0};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == Code::kSubmitted; }
};

struct JobCancelOutcome final {
  enum class Code : std::uint8_t {
    kCancelled,  // QUEUED：已终态化并移出队列（幂等成功）
    kStopping,   // STARTING/RUNNING/STOPPING：已进入/已在取消升级路径
    kNotFound,
    kInvalidState,  // 其他终态：显式拒绝（携带当前状态）
    kStoreFailed,
    kUnavailable,
  } code{Code::kUnavailable};
  // 结果状态的 wire 值（kCancelled -> CANCELLED；kStopping -> STOPPING；
  // kInvalidState -> 拒绝时的当前状态）。
  std::uint8_t state{0};
  std::string detail;
};

class JobControl {
 public:
  virtual ~JobControl() = default;

  // 提交一个已通过 job::validate 的 JobSpec（owner 身份来自 peer，已由调用方
  // 填写）。成功返回分配的 JobId；失败映射稳定错误。
  [[nodiscard]] virtual JobSubmitOutcome submit_job(const job::JobSpec& spec) = 0;

  // 请求取消（授权已由调用方完成）。按执行时点的实际状态决定路径并返回结果。
  [[nodiscard]] virtual JobCancelOutcome cancel_job(std::uint64_t job_id) = 0;
};

class IpcService final : public IpcRequestHandler {
 public:
  IpcService(IpcServiceConfig config, store::StateStore& store, GpuStatusSource& gpu_source,
             LogSnapshotReader& log_reader, JobControl& job_control);

  IpcService(const IpcService&) = delete;
  IpcService& operator=(const IpcService&) = delete;
  IpcService(IpcService&&) = delete;
  IpcService& operator=(IpcService&&) = delete;
  ~IpcService() override = default;

  [[nodiscard]] IpcResponse handle(const PeerCredentials& peer, const IpcRequest& request) override;

  // LOGS_FOLLOW 的验证面（M6）：owner/admin 授权、Job 存在性与已启动状态
  // （log_path 存在）。error != kNone 的响应由调用方直接写回（无流式委派的
  // 服务器、或流式网关的拒绝路径）；error == kNone 时 logs_follow.job_state
  // 已填，两路 offset 由流式网关在订阅后填入。
  [[nodiscard]] IpcResponse validate_logs_follow(const PeerCredentials& peer,
                                                 const IpcLogsFollowRequest& request);

 private:
  [[nodiscard]] IpcResponse handle_submit(const PeerCredentials& peer,
                                          const IpcSubmitRequest& request);
  [[nodiscard]] IpcResponse handle_ps(const PeerCredentials& peer);
  [[nodiscard]] IpcResponse handle_queue(const PeerCredentials& peer);
  [[nodiscard]] IpcResponse handle_gpu();
  [[nodiscard]] IpcResponse handle_cancel(const PeerCredentials& peer, std::uint64_t job_id);
  [[nodiscard]] IpcResponse handle_logs(const PeerCredentials& peer, const IpcLogsRequest& request);
  [[nodiscard]] IpcResponse handle_tensorboard(const PeerCredentials& peer, std::uint64_t job_id);

  [[nodiscard]] bool is_admin(const PeerCredentials& peer) const noexcept;
  [[nodiscard]] static IpcResponse error_response(IpcRequestKind kind, IpcError error,
                                                  std::string detail);

  IpcServiceConfig config_;
  store::StateStore& store_;
  GpuStatusSource& gpu_source_;
  LogSnapshotReader& log_reader_;
  JobControl& job_control_;
};

}  // namespace yori::ipc
