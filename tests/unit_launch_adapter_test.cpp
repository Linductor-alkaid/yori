#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>
#include <yori/launch/launch_adapter.hpp>

#include "yori_test.hpp"

namespace {

using namespace yori::launch;

yori::job::JobSpec valid_spec() {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"python", "train.py"};
  spec.cwd = "/srv/training";
  spec.env = {{"YORI_RUN", "test"}};
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
  return spec;
}

IdentityInfo valid_identity() {
  IdentityInfo identity;
  identity.uid = 1000;
  identity.gid = 1000;
  identity.username = "trainer";
  identity.home = "/home/trainer";
  identity.shell = "/bin/bash";
  identity.supplementary_groups = {1000, 2000};
  return identity;
}

GpuAssignment valid_assignment() { return GpuAssignment{yori::gpu::GpuUuid{"GPU-abcdef"}, 3, 0}; }

const EnvironmentEntry* find_entry(const LaunchPlan& plan, const std::string& name) {
  const auto iter =
      std::find_if(plan.env.begin(), plan.env.end(),
                   [&name](const EnvironmentEntry& entry) { return entry.first == name; });
  return iter != plan.env.end() ? &*iter : nullptr;
}

void check_prepare_error(LaunchPrepareErrorCode expected, const yori::job::JobSpec& spec,
                         const GpuAssignment& assignment, const LaunchProfile& profile,
                         const IdentityInfo& identity) {
  DefaultLaunchAdapter adapter;
  const auto result = adapter.prepare(spec, assignment, profile, identity);
  YORI_CHECK(!result);
  YORI_CHECK(result.code == expected);
}

}  // namespace

int main() {
  // ---- LaunchProfile 校验 --------------------------------------------------
  YORI_CHECK(validate(LaunchProfile{}));
  YORI_CHECK(validate(LaunchProfile{GpuMappingMode::kPhysicalArgument, "--gpu"}));

  LaunchProfile profile = LaunchProfile{GpuMappingMode::kPhysicalArgument, ""};
  YORI_CHECK(!validate(profile));
  YORI_CHECK(validate(profile).code == LaunchProfileErrorCode::kPhysicalArgumentMissing);

  profile = LaunchProfile{GpuMappingMode::kPhysicalArgument, "gpu"};
  YORI_CHECK(validate(profile).code == LaunchProfileErrorCode::kInvalidPhysicalArgument);

  profile = LaunchProfile{GpuMappingMode::kCudaVisibleDevices, "--gpu"};
  YORI_CHECK(validate(profile).code == LaunchProfileErrorCode::kPhysicalArgumentUnexpected);

  profile = LaunchProfile{GpuMappingMode::kPhysicalArgument, std::string(65, '-')};
  YORI_CHECK(validate(profile).code == LaunchProfileErrorCode::kInvalidPhysicalArgument);

  // ---- Identity 校验 --------------------------------------------------------
  YORI_CHECK(validate(valid_identity()));

  IdentityInfo identity = valid_identity();
  identity.uid = 0;
  YORI_CHECK(validate(identity).code == IdentityErrorCode::kRootIdentityNotAllowed);

  identity = valid_identity();
  identity.gid = 0;
  YORI_CHECK(validate(identity).code == IdentityErrorCode::kInvalidGid);

  identity = valid_identity();
  identity.username = std::string(65, 'u');
  YORI_CHECK(validate(identity).code == IdentityErrorCode::kUsernameTooLong);

  identity = valid_identity();
  identity.supplementary_groups.assign(65, 1);
  YORI_CHECK(validate(identity).code == IdentityErrorCode::kTooManySupplementaryGroups);

  // ---- PosixIdentityResolver：当前用户可解析，uid 0 拒绝 -----------------------
  {
    PosixIdentityResolver resolver;
    const auto root = resolver.resolve(0);
    YORI_CHECK(!root);
    YORI_CHECK(root.code == IdentityResolveErrorCode::kNotFound);

    IdentityInfo current = valid_identity();
    current.uid = static_cast<std::uint32_t>(::geteuid());
    current.gid = static_cast<std::uint32_t>(::getegid());
    const auto resolved = resolver.resolve(current.uid);
    YORI_CHECK(resolved.ok() == (::geteuid() != 0));
    if (resolved) {
      YORI_CHECK(!resolved.identity.username.empty());
      YORI_CHECK(resolved.identity.uid == current.uid);
      YORI_CHECK(!resolved.identity.supplementary_groups.empty());
      YORI_CHECK(validate(resolved.identity));
    }
  }

  // ---- prepare：cuda_visible_devices 模式（DEC-006 三层合并）------------------
  {
    DefaultLaunchAdapter adapter;
    adapter.set_daemon_environment({{"PATH", "/usr/bin"},
                                    {"LANG", "C.UTF-8"},
                                    {"LC_ALL", "en_US.UTF-8"},
                                    {"SECRET_TOKEN", "leak"},
                                    {"SUDO_USER", "root"},
                                    {"TERM", "xterm"}});
    const auto result =
        adapter.prepare(valid_spec(), valid_assignment(), LaunchProfile{}, valid_identity());
    YORI_CHECK(result);
    if (result) {
      const LaunchPlan& plan = result.plan;
      YORI_CHECK(plan.argv == valid_spec().argv);
      YORI_CHECK(plan.uid == 1000 && plan.gid == 1000);
      YORI_CHECK(plan.username == "trainer");
      YORI_CHECK(plan.cwd == "/srv/training");
      YORI_CHECK(plan.supplementary_groups == valid_identity().supplementary_groups);

      // 身份块。
      YORI_CHECK(find_entry(plan, "HOME") != nullptr &&
                 find_entry(plan, "HOME")->second == "/home/trainer");
      YORI_CHECK(find_entry(plan, "USER") != nullptr &&
                 find_entry(plan, "USER")->second == "trainer");
      YORI_CHECK(find_entry(plan, "LOGNAME") != nullptr);
      YORI_CHECK(find_entry(plan, "SHELL") != nullptr);
      // 白名单继承。
      YORI_CHECK(find_entry(plan, "PATH") != nullptr);
      YORI_CHECK(find_entry(plan, "LC_ALL") != nullptr);
      YORI_CHECK(find_entry(plan, "TERM") != nullptr);
      // 白名单外不进入。
      YORI_CHECK(find_entry(plan, "SECRET_TOKEN") == nullptr);
      YORI_CHECK(find_entry(plan, "SUDO_USER") == nullptr);
      // GPU 映射块（NVML 索引 + PCI 总线序）。
      YORI_CHECK(find_entry(plan, "CUDA_VISIBLE_DEVICES") != nullptr &&
                 find_entry(plan, "CUDA_VISIBLE_DEVICES")->second == "3");
      YORI_CHECK(find_entry(plan, "CUDA_DEVICE_ORDER") != nullptr &&
                 find_entry(plan, "CUDA_DEVICE_ORDER")->second == "PCI_BUS_ID");
      // 用户变量。
      YORI_CHECK(find_entry(plan, "YORI_RUN") != nullptr);
      // 键唯一且按字典序。
      YORI_CHECK(std::is_sorted(plan.env.begin(), plan.env.end()));
      for (std::size_t i = 1; i < plan.env.size(); ++i) {
        YORI_CHECK(plan.env[i - 1].first != plan.env[i].first);
      }
      YORI_CHECK(validate(plan));
    }
  }

  // ---- prepare：physical_argument 模式追加参数、不设 CUDA 环境变量 ------------
  {
    DefaultLaunchAdapter adapter;
    const auto result = adapter.prepare(valid_spec(), valid_assignment(),
                                        LaunchProfile{GpuMappingMode::kPhysicalArgument, "--gpu"},
                                        valid_identity());
    YORI_CHECK(result);
    if (result) {
      const std::vector<std::string> expected{"python", "train.py", "--gpu", "3"};
      YORI_CHECK(result.plan.argv == expected);
      YORI_CHECK(find_entry(result.plan, "CUDA_VISIBLE_DEVICES") == nullptr);
      YORI_CHECK(find_entry(result.plan, "CUDA_DEVICE_ORDER") == nullptr);
    }
  }

  // ---- prepare：保留键与无效输入拒绝 ----------------------------------------
  {
    yori::job::JobSpec spec = valid_spec();
    spec.env = {{"CUDA_VISIBLE_DEVICES", "0"}};
    check_prepare_error(LaunchPrepareErrorCode::kReservedEnvironmentKey, spec, valid_assignment(),
                        LaunchProfile{}, valid_identity());

    spec = valid_spec();
    spec.env = {{"HOME", "/tmp"}};
    check_prepare_error(LaunchPrepareErrorCode::kReservedEnvironmentKey, spec, valid_assignment(),
                        LaunchProfile{}, valid_identity());

    spec = valid_spec();
    spec.env = {{"LD_PRELOAD", "/tmp/evil.so"}};
    check_prepare_error(LaunchPrepareErrorCode::kReservedEnvironmentKey, spec, valid_assignment(),
                        LaunchProfile{}, valid_identity());

    spec = valid_spec();
    spec.argv.clear();
    check_prepare_error(LaunchPrepareErrorCode::kInvalidSpec, spec, valid_assignment(),
                        LaunchProfile{}, valid_identity());

    IdentityInfo broken = valid_identity();
    broken.uid = 0;
    check_prepare_error(LaunchPrepareErrorCode::kInvalidIdentity, valid_spec(), valid_assignment(),
                        LaunchProfile{}, broken);

    check_prepare_error(LaunchPrepareErrorCode::kInvalidGpuAssignment, valid_spec(),
                        GpuAssignment{yori::gpu::GpuUuid{""}, 0, 0}, LaunchProfile{},
                        valid_identity());
  }

  // ---- 自定义白名单：收紧后 daemon 变量不再进入 ------------------------------
  {
    EnvironmentPolicy policy;
    policy.exact_keys = {"PATH"};
    DefaultLaunchAdapter adapter(std::move(policy));
    adapter.set_daemon_environment({{"PATH", "/usr/bin"}, {"TERM", "xterm"}});
    const auto result =
        adapter.prepare(valid_spec(), valid_assignment(), LaunchProfile{}, valid_identity());
    YORI_CHECK(result);
    if (result) {
      YORI_CHECK(find_entry(result.plan, "PATH") != nullptr);
      YORI_CHECK(find_entry(result.plan, "TERM") == nullptr);
    }
  }

  // ---- LaunchPlan 独立复验 ---------------------------------------------------
  const auto make_plan = [](std::vector<std::string> argv, std::vector<EnvironmentEntry> env = {},
                            std::string cwd = "") {
    LaunchPlan plan;
    plan.argv = std::move(argv);
    plan.env = std::move(env);
    plan.cwd = std::move(cwd);
    plan.uid = 1000;
    plan.gid = 1000;
    plan.username = "trainer";
    plan.supplementary_groups = {1000};
    return plan;
  };

  LaunchPlan plan = make_plan({"true"});
  YORI_CHECK(validate(plan));

  plan = make_plan({});
  YORI_CHECK(validate(plan).code == LaunchPlanErrorCode::kEmptyArguments);

  plan = make_plan({"true"}, {{"BAD=NAME", "v"}});
  YORI_CHECK(validate(plan).code == LaunchPlanErrorCode::kInvalidEnvironmentName);

  plan = make_plan({"true"}, {}, "relative/path");
  YORI_CHECK(validate(plan).code == LaunchPlanErrorCode::kInvalidWorkingDirectory);

  return yori::testing::failure_count == 0 ? 0 : 1;
}
