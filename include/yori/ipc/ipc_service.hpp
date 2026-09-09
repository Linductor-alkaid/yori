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
  //（daemon 启动时从系统组数据库解析的成员集）。
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

  [[nodiscard]] virtual LogTailResult read_tail(const std::string& path, std::uint32_t max_bytes) = 0;
};

// 文件实现：从文件末尾有界读取（O_NOFOLLOW，基线 8/23）。大于 max_bytes 时
// 返回尾部并置 truncated。
[[nodiscard]] std::unique_ptr<LogSnapshotReader> file_log_snapshot_reader();

class IpcService final : public IpcRequestHandler {
 public:
  IpcService(IpcServiceConfig config, queue::GlobalJobQueue& queue, store::StateStore& store,
             GpuStatusSource& gpu_source, LogSnapshotReader& log_reader);

  IpcService(const IpcService&) = delete;
  IpcService& operator=(const IpcService&) = delete;
  IpcService(IpcService&&) = delete;
  IpcService& operator=(IpcService&&) = delete;
  ~IpcService() override = default;

  [[nodiscard]] IpcResponse handle(const PeerCredentials& peer,
                                   const IpcRequest& request) override;

 private:
  [[nodiscard]] IpcResponse handle_submit(const PeerCredentials& peer,
                                          const IpcSubmitRequest& request);
  [[nodiscard]] IpcResponse handle_ps(const PeerCredentials& peer);
  [[nodiscard]] IpcResponse handle_queue(const PeerCredentials& peer);
  [[nodiscard]] IpcResponse handle_gpu();
  [[nodiscard]] IpcResponse handle_cancel(const PeerCredentials& peer, std::uint64_t job_id);
  [[nodiscard]] IpcResponse handle_logs(const PeerCredentials& peer,
                                        const IpcLogsRequest& request);

  [[nodiscard]] bool is_admin(const PeerCredentials& peer) const noexcept;
  [[nodiscard]] static IpcResponse error_response(IpcRequestKind kind, IpcError error,
                                                  std::string detail);

  IpcServiceConfig config_;
  queue::GlobalJobQueue& queue_;
  store::StateStore& store_;
  GpuStatusSource& gpu_source_;
  LogSnapshotReader& log_reader_;
};

}  // namespace yori::ipc
