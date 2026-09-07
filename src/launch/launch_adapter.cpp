#include <yori/launch/launch_adapter.hpp>

#include <pwd.h>
#include <grp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>

namespace yori::launch {
namespace {

bool contains_nul(std::string_view text) noexcept {
  return text.find('\0') != std::string_view::npos;
}

bool is_valid_environment_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > LaunchPlanLimits::kMaxEnvironmentNameBytes ||
      contains_nul(name)) {
    return false;
  }
  if (name.front() == '=' || name.find('=') != std::string_view::npos) {
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool add_within_limit(std::size_t bytes, std::size_t& total, std::size_t limit) noexcept {
  if (bytes > limit || total > limit - bytes) {
    return false;
  }
  total += bytes;
  return true;
}

}  // namespace

const char* to_string(GpuMappingMode mode) noexcept {
  switch (mode) {
    case GpuMappingMode::kCudaVisibleDevices:
      return "cuda_visible_devices";
    case GpuMappingMode::kPhysicalArgument:
      return "physical_argument";
  }
  return "unknown";
}

LaunchProfileValidationResult validate(const LaunchProfile& profile) noexcept {
  switch (profile.mode) {
    case GpuMappingMode::kCudaVisibleDevices:
      if (!profile.physical_argument.empty()) {
        return {LaunchProfileErrorCode::kPhysicalArgumentUnexpected};
      }
      return {};
    case GpuMappingMode::kPhysicalArgument: {
      const auto& argument = profile.physical_argument;
      if (argument.empty()) {
        return {LaunchProfileErrorCode::kPhysicalArgumentMissing};
      }
      if (argument.front() != '-' || contains_nul(argument) ||
          argument.find(' ') != std::string::npos ||
          argument.size() > LaunchProfileLimits::kMaxPhysicalArgumentBytes) {
        return {LaunchProfileErrorCode::kInvalidPhysicalArgument};
      }
      return {};
    }
  }
  return {LaunchProfileErrorCode::kInvalidPhysicalArgument};
}

const char* to_string(LaunchProfileErrorCode code) noexcept {
  switch (code) {
    case LaunchProfileErrorCode::kNone:
      return "none";
    case LaunchProfileErrorCode::kPhysicalArgumentMissing:
      return "physical_argument mode requires a physical argument name";
    case LaunchProfileErrorCode::kPhysicalArgumentUnexpected:
      return "physical_argument is not used in cuda_visible_devices mode";
    case LaunchProfileErrorCode::kInvalidPhysicalArgument:
      return "invalid physical argument";
    case LaunchProfileErrorCode::kPhysicalArgumentTooLong:
      return "physical argument too long";
  }
  return "unknown";
}

IdentityValidationResult validate(const IdentityInfo& identity) noexcept {
  if (identity.uid == 0) {
    return {IdentityErrorCode::kRootIdentityNotAllowed};
  }
  if (identity.gid == 0) {
    return {IdentityErrorCode::kInvalidGid};
  }
  if (identity.username.empty() || contains_nul(identity.username)) {
    return {IdentityErrorCode::kInvalidUsername};
  }
  if (identity.username.size() > IdentityInfoLimits::kMaxUsernameBytes) {
    return {IdentityErrorCode::kUsernameTooLong};
  }
  if (!identity.home.empty() && (contains_nul(identity.home) ||
                                 identity.home.size() > IdentityInfoLimits::kMaxPathBytes)) {
    return {IdentityErrorCode::kInvalidHome};
  }
  if (!identity.shell.empty() && (contains_nul(identity.shell) ||
                                  identity.shell.size() > IdentityInfoLimits::kMaxPathBytes)) {
    return {IdentityErrorCode::kInvalidShell};
  }
  if (identity.supplementary_groups.size() > IdentityInfoLimits::kMaxSupplementaryGroups) {
    return {IdentityErrorCode::kTooManySupplementaryGroups};
  }
  return {};
}

const char* to_string(IdentityErrorCode code) noexcept {
  switch (code) {
    case IdentityErrorCode::kNone:
      return "none";
    case IdentityErrorCode::kRootIdentityNotAllowed:
      return "root identity is not allowed";
    case IdentityErrorCode::kInvalidUid:
      return "invalid uid";
    case IdentityErrorCode::kInvalidGid:
      return "invalid gid";
    case IdentityErrorCode::kInvalidUsername:
      return "invalid username";
    case IdentityErrorCode::kUsernameTooLong:
      return "username too long";
    case IdentityErrorCode::kInvalidHome:
      return "invalid home";
    case IdentityErrorCode::kHomeTooLong:
      return "home too long";
    case IdentityErrorCode::kInvalidShell:
      return "invalid shell";
    case IdentityErrorCode::kShellTooLong:
      return "shell too long";
    case IdentityErrorCode::kTooManySupplementaryGroups:
      return "too many supplementary groups";
  }
  return "unknown";
}

IdentityResolveResult PosixIdentityResolver::resolve(std::uint32_t uid) {
  if (uid == 0) {
    return {IdentityResolveErrorCode::kNotFound, {}, "uid 0 is not a valid job owner"};
  }

  std::array<char, 8192> buffer{};
  passwd pwd{};
  passwd* result = nullptr;
  const int rc = ::getpwuid_r(static_cast<uid_t>(uid), &pwd, buffer.data(), buffer.size(), &result);
  if (rc == ENOENT || result == nullptr) {
    return {IdentityResolveErrorCode::kNotFound, {}, "passwd entry not found for uid"};
  }
  if (rc != 0) {
    return {IdentityResolveErrorCode::kBackendUnavailable, {}, "getpwuid_r failed"};
  }

  IdentityInfo identity;
  identity.uid = uid;
  identity.gid = static_cast<std::uint32_t>(pwd.pw_gid);
  identity.username = pwd.pw_name != nullptr ? pwd.pw_name : "";
  identity.home = pwd.pw_dir != nullptr ? pwd.pw_dir : "";
  identity.shell = pwd.pw_shell != nullptr ? pwd.pw_shell : "";

  std::array<gid_t, IdentityInfoLimits::kMaxSupplementaryGroups> groups{};
  int count = static_cast<int>(groups.size());
  const int found = ::getgrouplist(identity.username.c_str(), static_cast<gid_t>(identity.gid),
                                   groups.data(), &count);
  if (found < 0) {
    return {IdentityResolveErrorCode::kBackendUnavailable, {},
            "user belongs to more groups than the supported limit"};
  }
  identity.supplementary_groups.reserve(static_cast<std::size_t>(found));
  for (int i = 0; i < found; ++i) {
    identity.supplementary_groups.push_back(static_cast<std::uint32_t>(groups[static_cast<std::size_t>(i)]));
  }

  if (const IdentityValidationResult check = validate(identity); !check.ok()) {
    return {IdentityResolveErrorCode::kBackendUnavailable, {}, to_string(check.code)};
  }
  return {IdentityResolveErrorCode::kNone, std::move(identity), {}};
}

LaunchPlanValidationResult validate(const LaunchPlan& plan) noexcept {
  if (plan.argv.empty()) {
    return {LaunchPlanErrorCode::kEmptyArguments, std::nullopt, std::nullopt};
  }
  if (plan.argv.size() > LaunchPlanLimits::kMaxArgumentCount) {
    return {LaunchPlanErrorCode::kTooManyArguments, std::nullopt, std::nullopt};
  }
  std::size_t argument_bytes = 0;
  for (std::size_t i = 0; i < plan.argv.size(); ++i) {
    const auto& argument = plan.argv[i];
    if (argument.empty() || contains_nul(argument)) {
      return {LaunchPlanErrorCode::kInvalidArgument, i, std::nullopt};
    }
    if (argument.size() > LaunchPlanLimits::kMaxSingleArgumentBytes) {
      return {LaunchPlanErrorCode::kArgumentTooLong, i, std::nullopt};
    }
    if (!add_within_limit(argument.size(), argument_bytes,
                          LaunchPlanLimits::kMaxArgumentBytes)) {
      return {LaunchPlanErrorCode::kArgumentsTooLarge, i, std::nullopt};
    }
  }
  if (plan.cwd.size() > LaunchPlanLimits::kMaxWorkingDirectoryBytes || contains_nul(plan.cwd)) {
    return {LaunchPlanErrorCode::kInvalidWorkingDirectory, std::nullopt, std::nullopt};
  }
  if (!plan.cwd.empty() && plan.cwd.front() != '/') {
    return {LaunchPlanErrorCode::kInvalidWorkingDirectory, std::nullopt, std::nullopt};
  }
  if (plan.env.size() > LaunchPlanLimits::kMaxEnvironmentVariables) {
    return {LaunchPlanErrorCode::kTooManyEnvironmentVariables, std::nullopt, std::nullopt};
  }
  std::size_t environment_bytes = 0;
  for (std::size_t i = 0; i < plan.env.size(); ++i) {
    const auto& [name, value] = plan.env[i];
    if (!is_valid_environment_name(name)) {
      return {LaunchPlanErrorCode::kInvalidEnvironmentName, i, name};
    }
    if (value.size() > LaunchPlanLimits::kMaxEnvironmentValueBytes || contains_nul(value)) {
      return {LaunchPlanErrorCode::kEnvironmentValueTooLong, i, name};
    }
    if (!add_within_limit(name.size() + value.size(), environment_bytes,
                          LaunchPlanLimits::kMaxEnvironmentBytes)) {
      return {LaunchPlanErrorCode::kEnvironmentTooLarge, i, name};
    }
  }
  if (plan.uid == 0 || plan.gid == 0) {
    return {LaunchPlanErrorCode::kInvalidIdentity, std::nullopt, std::nullopt};
  }
  if (plan.username.empty() || contains_nul(plan.username) ||
      plan.username.size() > IdentityInfoLimits::kMaxUsernameBytes) {
    return {LaunchPlanErrorCode::kInvalidUsername, std::nullopt, std::nullopt};
  }
  if (plan.supplementary_groups.size() > IdentityInfoLimits::kMaxSupplementaryGroups) {
    return {LaunchPlanErrorCode::kInvalidIdentity, std::nullopt, std::nullopt};
  }
  return {};
}

const char* to_string(LaunchPlanErrorCode code) noexcept {
  switch (code) {
    case LaunchPlanErrorCode::kNone:
      return "none";
    case LaunchPlanErrorCode::kEmptyArguments:
      return "empty arguments";
    case LaunchPlanErrorCode::kTooManyArguments:
      return "too many arguments";
    case LaunchPlanErrorCode::kInvalidArgument:
      return "invalid argument";
    case LaunchPlanErrorCode::kArgumentTooLong:
      return "argument too long";
    case LaunchPlanErrorCode::kArgumentsTooLarge:
      return "arguments too large";
    case LaunchPlanErrorCode::kInvalidWorkingDirectory:
      return "invalid working directory";
    case LaunchPlanErrorCode::kWorkingDirectoryTooLong:
      return "working directory too long";
    case LaunchPlanErrorCode::kTooManyEnvironmentVariables:
      return "too many environment variables";
    case LaunchPlanErrorCode::kInvalidEnvironmentName:
      return "invalid environment name";
    case LaunchPlanErrorCode::kEnvironmentNameTooLong:
      return "environment name too long";
    case LaunchPlanErrorCode::kEnvironmentValueTooLong:
      return "environment value too long";
    case LaunchPlanErrorCode::kEnvironmentTooLarge:
      return "environment too large";
    case LaunchPlanErrorCode::kInvalidIdentity:
      return "invalid identity";
    case LaunchPlanErrorCode::kInvalidUsername:
      return "invalid username";
  }
  return "unknown";
}

const EnvironmentPolicy& EnvironmentPolicy::defaults() noexcept {
  static const EnvironmentPolicy policy{
      /*exact_keys=*/{"LANG", "PATH", "TERM", "TZ"},
      /*prefix_keys=*/{"LC_"},
  };
  return policy;
}

bool EnvironmentPolicy::accepts(const std::string& name) const {
  for (const auto& key : exact_keys) {
    if (name == key) {
      return true;
    }
  }
  for (const auto& prefix : prefix_keys) {
    if (name.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

bool is_reserved_environment_key(const std::string& name) {
  static constexpr std::array<std::string_view, 8> kReserved{
      "CUDA_DEVICE_ORDER", "CUDA_VISIBLE_DEVICES", "HOME", "LD_LIBRARY_PATH", "LD_PRELOAD",
      "LOGNAME",            "SHELL",                "USER",
  };
  return std::find(kReserved.begin(), kReserved.end(), name) != kReserved.end();
}

DefaultLaunchAdapter::DefaultLaunchAdapter(EnvironmentPolicy policy)
    : policy_(std::move(policy)) {}

void DefaultLaunchAdapter::set_daemon_environment(std::vector<EnvironmentEntry> environment) {
  daemon_environment_ = std::move(environment);
}

void DefaultLaunchAdapter::set_environment_policy(EnvironmentPolicy policy) {
  policy_ = std::move(policy);
}

LaunchPlanResult DefaultLaunchAdapter::prepare(const job::JobSpec& spec,
                                               const GpuAssignment& assignment,
                                               const LaunchProfile& profile,
                                               const IdentityInfo& identity) {
  if (const job::JobSpecValidationResult spec_check = job::validate(spec); !spec_check.ok()) {
    return {LaunchPrepareErrorCode::kInvalidSpec, {}, {}, job::to_string(spec_check.code)};
  }
  if (const LaunchProfileValidationResult profile_check = validate(profile); !profile_check.ok()) {
    return {LaunchPrepareErrorCode::kInvalidProfile, {}, {}, to_string(profile_check.code)};
  }
  if (const IdentityValidationResult identity_check = validate(identity); !identity_check.ok()) {
    return {LaunchPrepareErrorCode::kInvalidIdentity, {}, {}, to_string(identity_check.code)};
  }
  if (!assignment.valid()) {
    return {LaunchPrepareErrorCode::kInvalidGpuAssignment, {}, {}, "invalid gpu assignment uuid"};
  }

  // DEC-006：保留键出现在用户环境中即拒绝整个 LaunchPlan。
  for (const auto& [name, value] : spec.env) {
    static_cast<void>(value);
    if (is_reserved_environment_key(name)) {
      return {LaunchPrepareErrorCode::kReservedEnvironmentKey, {}, name,
              "job environment may not override reserved key"};
    }
  }

  std::map<std::string, std::string> env;
  env.emplace("HOME", identity.home);
  env.emplace("USER", identity.username);
  env.emplace("LOGNAME", identity.username);
  env.emplace("SHELL", identity.shell);
  for (const auto& [name, value] : daemon_environment_) {
    if (policy_.accepts(name)) {
      env[name] = value;
    }
  }
  std::vector<std::string> argv = spec.argv;
  if (profile.mode == GpuMappingMode::kCudaVisibleDevices) {
    env["CUDA_VISIBLE_DEVICES"] = std::to_string(assignment.physical_index);
    env["CUDA_DEVICE_ORDER"] = "PCI_BUS_ID";
  } else {
    // physical_argument 模式不改环境：物理索引按用户程序的参数语义原样追加，
    // CUDA 变量不设置（设计第 8.1 节，遗留项目自行解释编号）。
    argv.emplace_back(profile.physical_argument);
    argv.emplace_back(std::to_string(assignment.physical_index));
  }
  for (const auto& [name, value] : spec.env) {
    env[name] = value;
  }

  LaunchPlan plan;
  plan.argv = std::move(argv);
  plan.env.assign(env.begin(), env.end());
  plan.cwd = spec.cwd;
  plan.uid = identity.uid;
  plan.gid = identity.gid;
  plan.username = identity.username;
  plan.supplementary_groups = identity.supplementary_groups;

  if (const LaunchPlanValidationResult plan_check = validate(plan); !plan_check.ok()) {
    return {LaunchPrepareErrorCode::kInvalidSpec, {}, {}, to_string(plan_check.code)};
  }
  return {LaunchPrepareErrorCode::kNone, std::move(plan), {}, {}};
}

}  // namespace yori::launch
