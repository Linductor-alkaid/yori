#include <yori/ipc/ipc_transport.hpp>

namespace yori::ipc {

const char* to_string(IpcTransportStartCode code) noexcept {
  switch (code) {
    case IpcTransportStartCode::kStarted:
      return "started";
    case IpcTransportStartCode::kAlreadyStarted:
      return "already started";
    case IpcTransportStartCode::kInvalidConfig:
      return "invalid config";
    case IpcTransportStartCode::kEndpointRejected:
      return "endpoint rejected";
    case IpcTransportStartCode::kExecutorRejected:
      return "executor rejected";
  }
  return "unknown";
}

const char* to_string(IpcClientError error) noexcept {
  switch (error) {
    case IpcClientError::kNone:
      return "none";
    case IpcClientError::kConnectFailed:
      return "connect failed";
    case IpcClientError::kTimeout:
      return "timeout";
    case IpcClientError::kClosed:
      return "closed by server";
    case IpcClientError::kProtocol:
      return "protocol error";
  }
  return "unknown";
}

}  // namespace yori::ipc
