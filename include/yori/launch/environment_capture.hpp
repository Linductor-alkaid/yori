#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <yori/job/job.hpp>
#include <yori/launch/launch_adapter.hpp>

namespace yori::launch {

// ---------------------------------------------------------------------------
// 提交时执行上下文捕获与解析（DEC-011 决策 2/3）。
//
// 纯 Core 组件：无副作用、无线程；文件系统探测（access）与解释器探测
// （probe_python_version 的子进程）由 CLI 在提交路径同步调用。daemon 侧
// 不重复捕获——捕获事实已随 JobSpec 持久化。
// ---------------------------------------------------------------------------

// 默认捕获白名单（DEC-011 决策 2）：存在才捕获，值来自 CLI 进程环境。
// 代理三键的小写形式显式列入（http_proxy 等由 curl/requests 等工具读取）。
// GPU 管理键（CUDA_VISIBLE_DEVICES/CUDA_DEVICE_ORDER）与 YORI_* 前缀不捕获：
// 它们属于调度器输出，捕获用户提交时刻的值没有意义。
struct EnvironmentCapturePolicy final {
  static const EnvironmentCapturePolicy& defaults() noexcept;

  // 配置级扩展键（exact 匹配，附加在默认白名单之后；M8 以 CLI --capture-env
  // 为运行时扩展点，daemon 配置文件机制出现后再收敛）。
  std::vector<std::string> extra_keys;
  // --inherit-env：显式 opt-in 捕获全部环境（仍过滤保留键，受 JobSpec env
  // 总量上限约束）。
  bool inherit_all{false};

  // name 是否被本策略捕获。保留键（含 YORI_* 前缀）永远不捕获。
  [[nodiscard]] bool captures(const std::string& name) const;
};

enum class EnvironmentCaptureErrorCode {
  kNone,
  kInvalidName,      // 环境源中存在无法成为合法变量名的条目（含 '=' 或 NUL）
  kTooManyVariables, // 超出 JobSpecLimits::kMaxEnvironmentVariables
  kValueTooLong,     // 单值超出 kMaxEnvironmentValueBytes
  kTooLarge,         // 总量超出 kMaxEnvironmentBytes
};

[[nodiscard]] const char* to_string(EnvironmentCaptureErrorCode code) noexcept;

struct EnvironmentCaptureResult final {
  EnvironmentCaptureErrorCode code{EnvironmentCaptureErrorCode::kNone};
  // 触发容量失败的变量名（仅容量类错误有效；用于用户可读报错）。
  std::string offending_name;
  // 捕获结果（按键序）；失败时不再使用，调用方以 code 判定。
  std::map<std::string, std::string> env;

  [[nodiscard]] bool ok() const noexcept {
    return code == EnvironmentCaptureErrorCode::kNone;
  }
  explicit operator bool() const noexcept { return ok(); }
};

// 从环境源（CLI 传入 environ 的展开）按策略捕获。非法名条目被跳过（防御
// environ 中的畸形条目）；容量超限显式失败，调用方必须向用户报错而非截断。
[[nodiscard]] EnvironmentCaptureResult capture_environment(
    const std::vector<EnvironmentEntry>& source, const EnvironmentCapturePolicy& policy);

// ---------------------------------------------------------------------------
// executable 提交时解析（DEC-011 决策 3，fail-fast）。
// ---------------------------------------------------------------------------

enum class ExecutableResolveErrorCode {
  kNone,
  kEmptyName,
  kInvalidName,        // 含 NUL 或超长
  kRelativeWithoutCwd, // 相对 argv[0] 但未提供绝对 cwd
  kNotFound,           // 裸名不在 PATH 任何段，或绝对/相对路径不存在
  kNotExecutable,      // 存在但不可执行（EACCES）
};

[[nodiscard]] const char* to_string(ExecutableResolveErrorCode code) noexcept;

struct ExecutableResolveResult final {
  ExecutableResolveErrorCode code{ExecutableResolveErrorCode::kNotFound};
  // 解析出的绝对路径（不 canonicalize：符号链接保持原样，与用户直接执行
  // 等价）。
  std::string executable;
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == ExecutableResolveErrorCode::kNone; }
  explicit operator bool() const noexcept { return ok(); }
};

// 以捕获后的 PATH 解析 argv[0]：含 '/' 的相对 cwd 解析；裸名按 PATH 逐段
// 解析（PATH 缺省回退 /usr/bin:/bin，与 daemon 子进程搜索语义一致）。
// 目标必须存在且可执行（access X_OK），否则拒绝提交。
[[nodiscard]] ExecutableResolveResult resolve_executable(
    const std::string& argv0, const std::string& cwd,
    const std::map<std::string, std::string>& captured_env);

// ---------------------------------------------------------------------------
// 环境来源元数据（DEC-011 决策 1）。
// ---------------------------------------------------------------------------

// 由捕获结果判定 environment_type：CONDA_PREFIX 优先于 VIRTUAL_ENV（两者
// 同时存在时取 conda）。
[[nodiscard]] job::EnvSource detect_environment_source(
    const std::map<std::string, std::string>& captured_env);

// 解释器版本探测（best-effort）：basename 含 "python" 时执行
// `<executable> --version`（stdout/stderr 合并解析 "Python X.Y[.Z]"），有界
// 等待（超时击杀，返回空）；非 Python 解释器或任何失败均返回空，不失败提交。
[[nodiscard]] std::optional<std::string> probe_python_version(const std::string& executable);

}  // namespace yori::launch
