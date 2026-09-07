#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <yori/gpu/gpu_provider.hpp>
#include <yori/job/job.hpp>

namespace yori::launch {

// 调度器决策出的 GPU 事实（lease 侧），LaunchAdapter 的输入之一。physical_index
// 用于 CUDA_VISIBLE_DEVICES / 物理参数映射，logical_index 是进程内逻辑编号（单卡
// MVP 恒为 0，多卡为 POST-01）。
struct GpuAssignment final {
  gpu::GpuUuid uuid;
  std::uint32_t physical_index{0};
  std::uint32_t logical_index{0};

  [[nodiscard]] bool valid() const noexcept { return uuid.valid(); }
};

enum class GpuMappingMode {
  kCudaVisibleDevices,
  kPhysicalArgument,
};

[[nodiscard]] const char* to_string(GpuMappingMode mode) noexcept;

struct LaunchProfileLimits final {
  static constexpr std::size_t kMaxPhysicalArgumentBytes = 64;
};

// 训练命令接收 GPU 分配的方式（设计第 8.1 节）。kPhysicalArgument 模式把
// `<physical_argument> <physical_index>` 追加到 argv；kCudaVisibleDevices 模式只
// 设置环境变量。
struct LaunchProfile final {
  GpuMappingMode mode{GpuMappingMode::kCudaVisibleDevices};
  std::string physical_argument;
};

enum class LaunchProfileErrorCode {
  kNone,
  kPhysicalArgumentMissing,
  kPhysicalArgumentUnexpected,
  kInvalidPhysicalArgument,
  kPhysicalArgumentTooLong,
};

struct LaunchProfileValidationResult final {
  LaunchProfileErrorCode code{LaunchProfileErrorCode::kNone};

  [[nodiscard]] constexpr bool ok() const noexcept { return code == LaunchProfileErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

[[nodiscard]] LaunchProfileValidationResult validate(const LaunchProfile& profile) noexcept;
[[nodiscard]] const char* to_string(LaunchProfileErrorCode code) noexcept;

struct IdentityInfoLimits final {
  static constexpr std::size_t kMaxUsernameBytes = 64;
  static constexpr std::size_t kMaxPathBytes = 4096;
  static constexpr std::size_t kMaxSupplementaryGroups = 64;
};

// 提交用户的身份快照。必须在 fork 前由 IdentityResolver 解析（DEC-006：子进程
// fork-exec 窗口内只执行 syscall 封装，不触碰 NSS）。
struct IdentityInfo final {
  std::uint32_t uid{0};
  std::uint32_t gid{0};
  std::string username;
  std::string home;
  std::string shell;
  std::vector<std::uint32_t> supplementary_groups;
};

enum class IdentityErrorCode {
  kNone,
  kRootIdentityNotAllowed,
  kInvalidUid,
  kInvalidGid,
  kInvalidUsername,
  kUsernameTooLong,
  kInvalidHome,
  kHomeTooLong,
  kInvalidShell,
  kShellTooLong,
  kTooManySupplementaryGroups,
};

struct IdentityValidationResult final {
  IdentityErrorCode code{IdentityErrorCode::kNone};

  [[nodiscard]] constexpr bool ok() const noexcept { return code == IdentityErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

[[nodiscard]] IdentityValidationResult validate(const IdentityInfo& identity) noexcept;
[[nodiscard]] const char* to_string(IdentityErrorCode code) noexcept;

enum class IdentityResolveErrorCode {
  kNone,
  kNotFound,
  kBackendUnavailable,
};

struct IdentityResolveResult final {
  IdentityResolveErrorCode code{IdentityResolveErrorCode::kBackendUnavailable};
  IdentityInfo identity;
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == IdentityResolveErrorCode::kNone; }
  explicit operator bool() const noexcept { return ok(); }
};

// 按 UID 解析提交用户身份的 SPI。实现负责自身线程模型的说明；Core 的默认实现是
// 基于 getpwuid_r/getgrouplist 的同步版本。
class IdentityResolver {
 public:
  virtual ~IdentityResolver() = default;

  [[nodiscard]] virtual IdentityResolveResult resolve(std::uint32_t uid) = 0;
};

// 基于 POSIX passwd/group 数据库的同步 IdentityResolver。NSS 查询可能阻塞，调用方
// 应在 Executor 有限任务或启动路径中执行。
class PosixIdentityResolver final : public IdentityResolver {
 public:
  [[nodiscard]] IdentityResolveResult resolve(std::uint32_t uid) override;
};

struct LaunchPlanLimits final {
  static constexpr std::size_t kMaxArgumentCount = job::JobSpecLimits::kMaxArgumentCount;
  static constexpr std::size_t kMaxArgumentBytes = job::JobSpecLimits::kMaxArgumentBytes;
  static constexpr std::size_t kMaxSingleArgumentBytes =
      job::JobSpecLimits::kMaxSingleArgumentBytes;
  static constexpr std::size_t kMaxWorkingDirectoryBytes =
      job::JobSpecLimits::kMaxWorkingDirectoryBytes;
  static constexpr std::size_t kMaxEnvironmentVariables =
      job::JobSpecLimits::kMaxEnvironmentVariables;
  static constexpr std::size_t kMaxEnvironmentNameBytes =
      job::JobSpecLimits::kMaxEnvironmentNameBytes;
  static constexpr std::size_t kMaxEnvironmentValueBytes =
      job::JobSpecLimits::kMaxEnvironmentValueBytes;
  static constexpr std::size_t kMaxEnvironmentBytes = job::JobSpecLimits::kMaxEnvironmentBytes;
};

using EnvironmentEntry = std::pair<std::string, std::string>;

// spawn 的直接输入：最终 argv、按键字典序唯一的环境条目、cwd 与目标身份。
// cwd 为空表示继承 daemon 当前目录；其余字段由 LaunchAdapter 保证有界。
// supplementary_groups 由 IdentityResolver 在 fork 前解析（DEC-006），子进程仅执行
// setgroups -> setgid -> setuid 三个 syscall 封装。
struct LaunchPlan final {
  std::vector<std::string> argv;
  std::vector<EnvironmentEntry> env;
  std::string cwd;
  std::uint32_t uid{0};
  std::uint32_t gid{0};
  std::string username;
  std::vector<std::uint32_t> supplementary_groups;
};

enum class LaunchPlanErrorCode {
  kNone,
  kEmptyArguments,
  kTooManyArguments,
  kInvalidArgument,
  kArgumentTooLong,
  kArgumentsTooLarge,
  kInvalidWorkingDirectory,
  kWorkingDirectoryTooLong,
  kTooManyEnvironmentVariables,
  kInvalidEnvironmentName,
  kEnvironmentNameTooLong,
  kEnvironmentValueTooLong,
  kEnvironmentTooLarge,
  kInvalidIdentity,
  kInvalidUsername,
};

struct LaunchPlanValidationResult final {
  LaunchPlanErrorCode code{LaunchPlanErrorCode::kNone};
  std::optional<std::size_t> item_index;
  std::optional<std::string> item_name;

  [[nodiscard]] constexpr bool ok() const noexcept { return code == LaunchPlanErrorCode::kNone; }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// spawn 侧的最终防线：即使 JobSpec 已通过校验，LaunchPlan 也独立复验边界。
[[nodiscard]] LaunchPlanValidationResult validate(const LaunchPlan& plan) noexcept;
[[nodiscard]] const char* to_string(LaunchPlanErrorCode code) noexcept;

// 环境变量三层合并策略（DEC-006）。exact/prefix 命中的 daemon 变量才进入训练环境。
struct EnvironmentPolicy final {
  std::vector<std::string> exact_keys;
  std::vector<std::string> prefix_keys;

  [[nodiscard]] static const EnvironmentPolicy& defaults() noexcept;

  // name 是否被白名单接纳。prefix_keys 按 `name.rfind(prefix, 0) == 0` 前缀匹配。
  [[nodiscard]] bool accepts(const std::string& name) const;
};

// 保留键：身份块与 GPU/动态链接器安全键。出现在 JobSpec.env 中即拒绝整个
// LaunchPlan（DEC-006）。
[[nodiscard]] bool is_reserved_environment_key(const std::string& name);

enum class LaunchPrepareErrorCode {
  kNone,
  kInvalidSpec,
  kInvalidProfile,
  kInvalidIdentity,
  kInvalidGpuAssignment,
  kReservedEnvironmentKey,
};

struct LaunchPlanResult final {
  LaunchPrepareErrorCode code{LaunchPrepareErrorCode::kNone};
  LaunchPlan plan;
  std::optional<std::string> reserved_key;
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == LaunchPrepareErrorCode::kNone; }
  explicit operator bool() const noexcept { return ok(); }
};

// 由 LaunchProfile 驱动、把调度结果映射为训练命令环境的 Core 接口（设计第 8 节）。
class LaunchAdapter {
 public:
  virtual ~LaunchAdapter() = default;

  [[nodiscard]] virtual LaunchPlanResult prepare(const job::JobSpec& spec,
                                                 const GpuAssignment& assignment,
                                                 const LaunchProfile& profile,
                                                 const IdentityInfo& identity) = 0;
};

// DEC-006 的默认实现。daemon 环境快照经 set_daemon_environment 注入（通常来自
// environ），未注入时不继承任何 daemon 变量。
class DefaultLaunchAdapter final : public LaunchAdapter {
 public:
  DefaultLaunchAdapter() = default;
  explicit DefaultLaunchAdapter(EnvironmentPolicy policy);

  void set_daemon_environment(std::vector<EnvironmentEntry> environment);
  void set_environment_policy(EnvironmentPolicy policy);

  [[nodiscard]] LaunchPlanResult prepare(const job::JobSpec& spec, const GpuAssignment& assignment,
                                         const LaunchProfile& profile,
                                         const IdentityInfo& identity) override;

 private:
  EnvironmentPolicy policy_ = EnvironmentPolicy::defaults();
  std::vector<EnvironmentEntry> daemon_environment_;
};

}  // namespace yori::launch
