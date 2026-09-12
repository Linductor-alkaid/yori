#include "runtime/ipc_server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <executor/blocking_io.hpp>
#include <executor/executor.hpp>
#include <string>
#include <utility>
#include <vector>

#include "ipc/uds_io.hpp"

namespace yori::runtime {
namespace {

using ipc::IpcError;
using ipc::IpcRequestDecodeResult;
using ipc::IpcRequestKind;
using ipc::IpcResponse;
using ipc::PeerCredentials;
using ipc::to_string;

ipc::IpcTransportStartResult start_failure(ipc::IpcTransportStartCode code, std::string message) {
  return ipc::IpcTransportStartResult{code, std::move(message)};
}

IpcResponse error_response(IpcRequestKind kind, IpcError error, const char* detail) {
  IpcResponse response;
  response.kind = kind;
  response.error = error;
  response.detail = detail;
  return response;
}

// 尽力写回一个错误响应帧后关闭；失败即静默断开（客户端看到 EOF）。
void reject_connection(int client_fd, IpcRequestKind kind, const char* detail) {
  IpcResponse response = error_response(kind, IpcError::kProtocol, detail);
  std::vector<std::uint8_t> payload;
  if (!ipc::encode_response_payload(response, payload)) {
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  static_cast<void>(ipc::uds::write_frame(client_fd, payload, deadline));
}

class ServerWorker final : public executor::IBlockingIoWorker {
 public:
  ServerWorker(int listen_fd, int wake_read, ipc::IpcRequestHandler& handler,
               UdsIpcStreamDelegate* stream_delegate, std::chrono::milliseconds request_deadline,
               std::atomic<bool>& stopping)
      : listen_fd_(listen_fd),
        wake_read_(wake_read),
        handler_(handler),
        stream_delegate_(stream_delegate),
        request_deadline_(request_deadline),
        stopping_(stopping) {}

  void run(executor::StopToken stop_token) override {
    while (!stop_token.stop_requested() && !stopping_.load(std::memory_order_relaxed)) {
      accept_once();
    }
  }

  void wakeup() noexcept override {
    const char byte = 1;
    ssize_t written = 0;
    do {
      written = ::write(wake_write_, &byte, 1);
    } while (written < 0 && errno == EINTR);
  }

  void set_wake_write(int fd) noexcept { wake_write_ = fd; }

 private:
  void accept_once() {
    struct pollfd waiters[2] = {};
    waiters[0].fd = listen_fd_;
    waiters[0].events = POLLIN;
    waiters[1].fd = wake_read_;
    waiters[1].events = POLLIN;

    const int ready = ::poll(waiters, 2, 200);
    if (ready <= 0) {
      // 超时（继续循环检查停止）或 EINTR；其余错误退避防忙等。
      if (ready < 0 && errno != EINTR) {
        struct pollfd backoff {};
        backoff.fd = -1;
        ::poll(&backoff, 1, 100);
      }
      return;
    }
    if ((waiters[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      drain_wake_pipe();
      if (stopping_.load(std::memory_order_relaxed)) {
        return;
      }
    }
    if ((waiters[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
      return;
    }

    const int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
        struct pollfd backoff {};
        backoff.fd = -1;
        ::poll(&backoff, 1, 100);
      }
      return;
    }
    serve(client_fd);
  }

  void serve(int client_fd) {
    // 身份获取失败：不接受无法鉴权的连接。
    ucred peer_credential{};
    socklen_t credential_length = sizeof(peer_credential);
    if (::getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &peer_credential, &credential_length) !=
        0) {
      static_cast<void>(::close(client_fd));
      return;
    }
    const PeerCredentials peer{static_cast<std::uint32_t>(peer_credential.uid),
                               static_cast<std::uint32_t>(peer_credential.gid),
                               static_cast<std::uint32_t>(peer_credential.pid)};

    const auto deadline = std::chrono::steady_clock::now() + request_deadline_;
    std::vector<std::uint8_t> payload;
    const ipc::uds::FrameIoError read_error = ipc::uds::read_frame(client_fd, payload, deadline);
    if (read_error == ipc::uds::FrameIoError::kBadLength ||
        read_error == ipc::uds::FrameIoError::kOversize) {
      // 帧界违规：显式 PROTOCOL 错误帧后断开（不静默、不重试）。
      reject_connection(client_fd, IpcRequestKind::kSubmit, "malformed frame");
      static_cast<void>(::close(client_fd));
      return;
    }
    if (read_error != ipc::uds::FrameIoError::kNone) {
      // 超时/EOF/IO：无完整请求可应答，直接断开。
      static_cast<void>(::close(client_fd));
      return;
    }

    const IpcRequestDecodeResult decoded =
        ipc::decode_request_payload(payload.data(), payload.size());
    if (!decoded.ok()) {
      reject_connection(client_fd, IpcRequestKind::kSubmit, to_string(decoded.error));
      static_cast<void>(::close(client_fd));
      return;
    }

    // 流式请求（M6）：委派接管连接（验证 + fd 移交 + 会话 worker 承载）。
    // 委派拒绝时按普通路径写回响应；无委派的构建同样得到显式错误。
    if (decoded.value.kind == IpcRequestKind::kLogsFollow && stream_delegate_ != nullptr) {
      UdsIpcStreamDelegate::Outcome outcome;
      try {
        outcome = stream_delegate_->begin_stream(peer, decoded.value, client_fd);
      } catch (...) {
        outcome.taken_over = false;
        outcome.response =
            error_response(decoded.value.kind, IpcError::kInternal, "internal error");
        outcome.response.version = decoded.value.version;
      }
      if (outcome.taken_over) {
        return;  // fd 所有权已转移，服务器不再触碰。
      }
      write_response_and_close(client_fd, outcome.response, deadline);
      return;
    }

    // handler 异常映射为 INTERNAL 响应（不吞、不崩 daemon；异常被消费为
    // 显式结果而非丢失）。响应回显请求版本（v1 客户端在 v2 daemon 上保持
    // 可用，DEC-011 决策 8）。
    IpcResponse response;
    try {
      response = handler_.handle(peer, decoded.value);
    } catch (...) {
      response = error_response(decoded.value.kind, IpcError::kInternal, "internal error");
    }
    response.version = decoded.value.version;
    write_response_and_close(client_fd, response, deadline);
  }

  void write_response_and_close(int client_fd, const IpcResponse& response,
                                ipc::uds::SteadyTime deadline) {
    std::vector<std::uint8_t> response_payload;
    if (!ipc::encode_response_payload(response, response_payload)) {
      // 响应越界属于 daemon 内部错误；退化为最小 INTERNAL 帧。
      IpcResponse fallback = error_response(response.kind, IpcError::kInternal, "internal error");
      if (!ipc::encode_response_payload(fallback, response_payload)) {
        static_cast<void>(::close(client_fd));
        return;
      }
    }
    static_cast<void>(ipc::uds::write_frame(client_fd, response_payload, deadline));
    static_cast<void>(::close(client_fd));
  }

  void drain_wake_pipe() noexcept {
    char buffer[64];
    while (::read(wake_read_, buffer, sizeof(buffer)) > 0) {
    }
  }

  int listen_fd_{-1};
  int wake_read_{-1};
  int wake_write_{-1};
  ipc::IpcRequestHandler& handler_;
  UdsIpcStreamDelegate* stream_delegate_;
  std::chrono::milliseconds request_deadline_;
  std::atomic<bool>& stopping_;
};

}  // namespace

bool UdsIpcServerConfig::valid(std::string& error) const noexcept {
  if (socket_path.empty() || socket_path.front() != '/' ||
      socket_path.size() > kMaxSocketPathBytes) {
    error = "socket path must be absolute and at most " + std::to_string(kMaxSocketPathBytes) +
            " bytes";
    return false;
  }
  if (socket_mode > 0777) {
    error = "socket mode must be at most 0777";
    return false;
  }
  if (request_deadline <= std::chrono::milliseconds::zero()) {
    error = "request deadline must be positive";
    return false;
  }
  return true;
}

class UdsIpcServer::Impl final {
 public:
  Impl(executor::Executor& executor_ref, ipc::IpcRequestHandler& handler_ref,
       UdsIpcServerConfig config_ref, UdsIpcStreamDelegate* stream_delegate_ref)
      : executor(executor_ref),
        handler(handler_ref),
        config(std::move(config_ref)),
        stream_delegate(stream_delegate_ref) {}

  executor::Executor& executor;
  ipc::IpcRequestHandler& handler;
  UdsIpcServerConfig config;
  UdsIpcStreamDelegate* stream_delegate;
  executor::WorkerHandle handle;
  int listen_fd{-1};
  int wake_read{-1};
  int wake_write{-1};
  std::atomic<bool> worker_stopping{false};
  bool started{false};
  bool stop_requested{false};
};

UdsIpcServer::UdsIpcServer(executor::Executor& executor, ipc::IpcRequestHandler& handler,
                           UdsIpcServerConfig config, UdsIpcStreamDelegate* stream_delegate)
    : impl_(std::make_unique<Impl>(executor, handler, std::move(config), stream_delegate)) {}

UdsIpcServer::~UdsIpcServer() { stop(); }

ipc::IpcTransportStartResult UdsIpcServer::start() {
  if (impl_->started) {
    return start_failure(ipc::IpcTransportStartCode::kAlreadyStarted, "ipc server already started");
  }
  if (impl_->stop_requested) {
    return start_failure(ipc::IpcTransportStartCode::kInvalidConfig,
                         "ipc server was stopped and cannot restart");
  }
  std::string validation_error;
  if (!impl_->config.valid(validation_error)) {
    return start_failure(ipc::IpcTransportStartCode::kInvalidConfig, validation_error);
  }

  // 陈旧端点处理：仅 socket 文件可替换；其余（含缺失目录）显式失败，绝不
  // unlink 任意文件（DEC-010）。
  struct stat endpoint_status {};
  if (::lstat(impl_->config.socket_path.c_str(), &endpoint_status) == 0) {
    if (!S_ISSOCK(endpoint_status.st_mode)) {
      return start_failure(ipc::IpcTransportStartCode::kEndpointRejected,
                           "endpoint exists and is not a socket: " + impl_->config.socket_path);
    }
    if (::unlink(impl_->config.socket_path.c_str()) != 0) {
      return start_failure(ipc::IpcTransportStartCode::kEndpointRejected,
                           "stale endpoint cannot be removed: " + impl_->config.socket_path);
    }
  } else if (errno != ENOENT) {
    return start_failure(ipc::IpcTransportStartCode::kEndpointRejected,
                         "endpoint cannot be inspected: " + impl_->config.socket_path);
  }

  const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) {
    return start_failure(ipc::IpcTransportStartCode::kExecutorRejected, "socket creation failed");
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, impl_->config.socket_path.c_str(),
              impl_->config.socket_path.size() + 1);
  // bind 创建的 socket inode 以 umask 收紧到目标模式之内（Linux 的 fchmod 对
  // socket fd 不生效，不能依赖事后收敛；umask 在单线程启动序内保存/恢复）。
  const mode_t previous_umask = ::umask(0777 & ~impl_->config.socket_mode);
  const int bind_result =
      ::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  static_cast<void>(::umask(previous_umask));
  if (bind_result != 0) {
    static_cast<void>(::close(listen_fd));
    return start_failure(ipc::IpcTransportStartCode::kEndpointRejected,
                         "bind failed: " + impl_->config.socket_path);
  }

  // 权限收敛在 listen 之前完成：显式 chmod/chown 到精确值（路径刚由本次
  // bind 创建，父目录可信）。
  if (::chmod(impl_->config.socket_path.c_str(), impl_->config.socket_mode) != 0) {
    static_cast<void>(::close(listen_fd));
    static_cast<void>(::unlink(impl_->config.socket_path.c_str()));
    return start_failure(ipc::IpcTransportStartCode::kEndpointRejected, "socket chmod failed");
  }
  if (impl_->config.apply_ownership &&
      ::chown(impl_->config.socket_path.c_str(), impl_->config.socket_owner_uid,
              impl_->config.socket_owner_gid) != 0) {
    static_cast<void>(::close(listen_fd));
    static_cast<void>(::unlink(impl_->config.socket_path.c_str()));
    return start_failure(ipc::IpcTransportStartCode::kEndpointRejected,
                         "socket chown failed (requires root or matching owner)");
  }
  if (::listen(listen_fd, 16) != 0) {
    static_cast<void>(::close(listen_fd));
    static_cast<void>(::unlink(impl_->config.socket_path.c_str()));
    return start_failure(ipc::IpcTransportStartCode::kEndpointRejected, "listen failed");
  }

  std::array<int, 2> wake_pipe{};
  if (::pipe2(wake_pipe.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
    static_cast<void>(::close(listen_fd));
    static_cast<void>(::unlink(impl_->config.socket_path.c_str()));
    return start_failure(ipc::IpcTransportStartCode::kExecutorRejected,
                         "wake pipe creation failed");
  }

  auto worker = std::make_unique<ServerWorker>(
      listen_fd, wake_pipe[0], impl_->handler, impl_->stream_delegate,
      impl_->config.request_deadline, impl_->worker_stopping);
  worker->set_wake_write(wake_pipe[1]);

  // blocking worker 名字单次注册不可复用（DuplicateName 语义），实例唯一化。
  static std::atomic<std::uint64_t> instance_counter{0};
  const auto instance = instance_counter.fetch_add(1, std::memory_order_relaxed);

  executor::BlockingWorkerSpec spec;
  spec.name = "yori-ipc-server-" + std::to_string(instance);
  spec.config.thread_name = "yori-ipc";
  spec.worker = std::move(worker);
  impl_->handle = impl_->executor.start_worker(std::move(spec));
  if (!impl_->handle.started()) {
    static_cast<void>(::close(wake_pipe[0]));
    static_cast<void>(::close(wake_pipe[1]));
    static_cast<void>(::close(listen_fd));
    static_cast<void>(::unlink(impl_->config.socket_path.c_str()));
    return start_failure(ipc::IpcTransportStartCode::kExecutorRejected,
                         "executor rejected the blocking worker");
  }

  impl_->listen_fd = listen_fd;
  impl_->wake_read = wake_pipe[0];
  impl_->wake_write = wake_pipe[1];
  impl_->started = true;
  return ipc::IpcTransportStartResult{ipc::IpcTransportStartCode::kStarted, {}};
}

void UdsIpcServer::stop() {
  if (impl_->stop_requested) {
    return;
  }
  impl_->stop_requested = true;
  impl_->worker_stopping.store(true, std::memory_order_relaxed);
  if (!impl_->started) {
    return;
  }

  // handle.stop() 请求停止、唤醒并 join（EXEC-10 ①：停止新连接与请求生产者）。
  impl_->handle.stop();
  impl_->started = false;

  if (impl_->listen_fd >= 0) {
    static_cast<void>(::close(impl_->listen_fd));
    impl_->listen_fd = -1;
  }
  if (impl_->wake_read >= 0) {
    static_cast<void>(::close(impl_->wake_read));
    impl_->wake_read = -1;
  }
  if (impl_->wake_write >= 0) {
    static_cast<void>(::close(impl_->wake_write));
    impl_->wake_write = -1;
  }
  static_cast<void>(::unlink(impl_->config.socket_path.c_str()));
}

}  // namespace yori::runtime
