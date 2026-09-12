#include <unistd.h>

#include <map>
#include <string>
#include <vector>
#include <yori/job/job.hpp>
#include <yori/launch/environment_capture.hpp>

#include "yori_test.hpp"

namespace {

using namespace yori::launch;
using EnvironmentEntry = yori::launch::EnvironmentEntry;

const std::vector<EnvironmentEntry>& activated_conda_source() {
  static const std::vector<EnvironmentEntry> source{
      {"PATH", "/opt/conda/envs/train/bin:/usr/bin:/bin"},
      {"PYTHONPATH", "/workspace/lib"},
      {"LD_LIBRARY_PATH", "/opt/conda/envs/train/lib"},
      {"CONDA_PREFIX", "/opt/conda/envs/train"},
      {"CONDA_DEFAULT_ENV", "train"},
      {"VIRTUAL_ENV", "/old/venv"},
      {"CUDA_HOME", "/usr/local/cuda"},
      {"OMP_NUM_THREADS", "8"},
      {"HTTP_PROXY", "http://proxy:7890"},
      {"https_proxy", "http://proxy:7890"},
      {"no_proxy", "localhost,127.0.0.1"},
      // 不捕获的键。
      {"CUDA_VISIBLE_DEVICES", "1,2"},
      {"CUDA_DEVICE_ORDER", "FASTEST_FIRST"},
      {"YORI_JOB_ID", "7"},
      {"YORI_SOCKET", "/run/yori/yori.sock"},
      {"HOME", "/home/trainer"},
      {"USER", "trainer"},
      {"LD_PRELOAD", "/tmp/evil.so"},
      {"SECRET_TOKEN", "leak"},
      {"TERM", "xterm"},
      {"PS1", "$ "},
  };
  return source;
}

}  // namespace

int main() {
  // ---- 默认白名单：只捕获列出的键，保留键与 GPU 管理键排除 ------------------
  {
    const auto result = capture_environment(activated_conda_source(),
                                            EnvironmentCapturePolicy::defaults());
    YORI_CHECK(result);
    if (result) {
      const auto has = [&result](const char* name) {
        return result.env.find(name) != result.env.end();
      };
      YORI_CHECK(has("PATH"));
      YORI_CHECK(has("PYTHONPATH"));
      YORI_CHECK(has("LD_LIBRARY_PATH"));
      YORI_CHECK(has("CONDA_PREFIX"));
      YORI_CHECK(has("CONDA_DEFAULT_ENV"));
      YORI_CHECK(has("CUDA_HOME"));
      YORI_CHECK(has("OMP_NUM_THREADS"));
      YORI_CHECK(has("HTTP_PROXY"));
      YORI_CHECK(has("https_proxy"));
      YORI_CHECK(has("no_proxy"));
      YORI_CHECK(result.env.size() == 11);
      // 不捕获：GPU 管理键、YORI_*、身份、其余环境。
      YORI_CHECK(!has("CUDA_VISIBLE_DEVICES"));
      YORI_CHECK(!has("CUDA_DEVICE_ORDER"));
      YORI_CHECK(!has("YORI_JOB_ID"));
      YORI_CHECK(!has("YORI_SOCKET"));
      YORI_CHECK(!has("HOME"));
      YORI_CHECK(!has("USER"));
      YORI_CHECK(!has("LD_PRELOAD"));
      YORI_CHECK(!has("SECRET_TOKEN"));
      YORI_CHECK(!has("TERM"));
      YORI_CHECK(!has("PS1"));
      // 捕获结果可直接通过 JobSpec 环境校验（含保留键不出现）。
      yori::job::JobSpec spec;
      spec.env = result.env;
      spec.owner_uid = 1000;
      spec.owner_gid = 1000;
      spec.argv = {"python", "train.py"};
      spec.cwd = "/srv";
      spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
      YORI_CHECK(yori::job::validate(spec));
    }
  }

  // ---- --inherit-env：全量捕获（仍过滤保留键）------------------------------
  {
    EnvironmentCapturePolicy policy;
    policy.inherit_all = true;
    const auto result = capture_environment(activated_conda_source(), policy);
    YORI_CHECK(result);
    if (result) {
      // 21 条源 - 7 条不捕获（2 GPU 管理键 + 2 YORI_* + 身份 2 + LD_PRELOAD）。
      YORI_CHECK(result.env.size() == 14);
      YORI_CHECK(result.env.contains("SECRET_TOKEN"));
      YORI_CHECK(result.env.contains("VIRTUAL_ENV"));
      YORI_CHECK(result.env.contains("TERM"));
      YORI_CHECK(!result.env.contains("CUDA_VISIBLE_DEVICES"));
      YORI_CHECK(!result.env.contains("YORI_JOB_ID"));
      YORI_CHECK(!result.env.contains("HOME"));
      YORI_CHECK(!result.env.contains("LD_PRELOAD"));
    }
  }

  // ---- 配置级扩展键 ---------------------------------------------------------
  {
    EnvironmentCapturePolicy policy;
    policy.extra_keys = {"JAX_PLATFORMS", "TORCH_HOME"};
    const auto result = capture_environment(
        {{"JAX_PLATFORMS", "cpu"}, {"TORCH_HOME", "/cache"}, {"OTHER", "x"}}, policy);
    YORI_CHECK(result);
    YORI_CHECK(result.env.size() == 2);
  }

  // ---- 容量上限：变量数、单值、总量显式失败 --------------------------------
  {
    std::vector<EnvironmentEntry> many;
    for (std::size_t i = 0;
         i <= yori::job::JobSpecLimits::kMaxEnvironmentVariables + 1; ++i) {
      many.push_back({"VAR_" + std::to_string(i), "v"});
    }
    EnvironmentCapturePolicy policy;
    policy.inherit_all = true;
    const auto result = capture_environment(many, policy);
    YORI_CHECK(!result);
    YORI_CHECK(result.code == EnvironmentCaptureErrorCode::kTooManyVariables);
    YORI_CHECK(result.offending_name == "VAR_" +
                                           std::to_string(
                                               yori::job::JobSpecLimits::kMaxEnvironmentVariables));
  }
  {
    EnvironmentCapturePolicy policy;
    policy.inherit_all = true;
    const auto result = capture_environment(
        {{"BIG", std::string(yori::job::JobSpecLimits::kMaxEnvironmentValueBytes + 1, 'x')}},
        policy);
    YORI_CHECK(!result);
    YORI_CHECK(result.code == EnvironmentCaptureErrorCode::kValueTooLong);
    YORI_CHECK(result.offending_name == "BIG");
  }
  {
    std::vector<EnvironmentEntry> large;
    std::size_t total = 0;
    while (total <= yori::job::JobSpecLimits::kMaxEnvironmentBytes) {
      const std::string name = "FILLER_" + std::to_string(large.size());
      const std::string value(1024, 'x');
      total += name.size() + value.size();
      large.emplace_back(name, value);
    }
    EnvironmentCapturePolicy policy;
    policy.inherit_all = true;
    const auto result = capture_environment(large, policy);
    YORI_CHECK(!result);
    YORI_CHECK(result.code == EnvironmentCaptureErrorCode::kTooLarge);
    YORI_CHECK(!result.offending_name.empty());
  }
  {
    // 畸形名条目被跳过，不失败捕获（GOOD_VAR 经扩展键接纳）。
    EnvironmentCapturePolicy policy;
    policy.extra_keys = {"GOOD_VAR"};
    const auto result = capture_environment({{"BAD NAME", "x"},
                                             {"ALSO=BAD", "y"},
                                             {"GOOD_VAR", "z"},
                                             {std::string("NUL\0X", 5), "w"}},
                                            policy);
    YORI_CHECK(result);
    YORI_CHECK(result.env.size() == 1);
    YORI_CHECK(result.env.contains("GOOD_VAR"));
  }

  // ---- executable 解析（fail-fast）-----------------------------------------
  {
    // 绝对路径存在且可执行。
    const auto result = resolve_executable("/bin/sh", "/tmp", {});
    YORI_CHECK(result);
    YORI_CHECK(result.executable == "/bin/sh");

    // 裸名按捕获后的 PATH 解析：假目录在前也不影响命中真段。
    const auto resolved = resolve_executable(
        "sh", "/tmp", {{"PATH", "/nonexistent-dir:/bin"}});
    YORI_CHECK(resolved);
    YORI_CHECK(resolved.executable == "/bin/sh");

    // 捕获的 PATH 缺失时回退 /usr/bin:/bin。
    const auto fallback = resolve_executable("sh", "/tmp", {});
    YORI_CHECK(fallback);

    // 相对路径相对 cwd 解析（不 canonicalize，./ 原样保留）。
    const auto relative = resolve_executable("./sh", "/bin", {});
    YORI_CHECK(relative);
    YORI_CHECK(relative.executable == "/bin/./sh");

    // 失败路径：不存在、不可执行、相对但无 cwd、空名。
    auto failed = resolve_executable("/nonexistent/binary", "/tmp", {});
    YORI_CHECK(!failed);
    YORI_CHECK(failed.code == ExecutableResolveErrorCode::kNotFound);

    failed = resolve_executable("definitely-not-on-path-xyz", "/tmp", {{"PATH", "/bin"}});
    YORI_CHECK(!failed);
    YORI_CHECK(failed.code == ExecutableResolveErrorCode::kNotFound);

    failed = resolve_executable("/etc/hostname", "/tmp", {});
    YORI_CHECK(!failed);
    YORI_CHECK(failed.code == ExecutableResolveErrorCode::kNotExecutable);

    failed = resolve_executable("./train.py", "", {});
    YORI_CHECK(!failed);
    YORI_CHECK(failed.code == ExecutableResolveErrorCode::kRelativeWithoutCwd);

    failed = resolve_executable("./train.py", "relative", {});
    YORI_CHECK(!failed);
    YORI_CHECK(failed.code == ExecutableResolveErrorCode::kRelativeWithoutCwd);

    failed = resolve_executable("", "/tmp", {});
    YORI_CHECK(!failed);
    YORI_CHECK(failed.code == ExecutableResolveErrorCode::kEmptyName);
  }

  // ---- environment_type 判定 ------------------------------------------------
  {
    YORI_CHECK(detect_environment_source({{"CONDA_PREFIX", "/opt/conda"}}) ==
               yori::job::EnvSource::kConda);
    YORI_CHECK(detect_environment_source({{"VIRTUAL_ENV", "/venv"}}) ==
               yori::job::EnvSource::kVenv);
    // 同时存在取 conda。
    YORI_CHECK(detect_environment_source(
                   {{"CONDA_PREFIX", "/opt/conda"}, {"VIRTUAL_ENV", "/venv"}}) ==
               yori::job::EnvSource::kConda);
    YORI_CHECK(detect_environment_source({{"PATH", "/bin"}}) == yori::job::EnvSource::kNone);
  }

  // ---- python 版本探测（best-effort；CI 提供 python3，否则仅验证跳过路径）--
  {
    // 非 Python 解释器：直接跳过。
    YORI_CHECK(!probe_python_version("/bin/sh").has_value());

    // 实际解释器。
    const char* candidates[] = {"/usr/bin/python3", "/usr/bin/python"};
    bool probed = false;
    for (const char* candidate : candidates) {
      if (::access(candidate, X_OK) != 0) {
        continue;
      }
      const auto version = probe_python_version(candidate);
      YORI_CHECK(version.has_value());
      if (version) {
        YORI_CHECK(!version->empty());
        YORI_CHECK(version->find('.') != std::string::npos);
        probed = true;
      }
      break;
    }
    if (!probed) {
      std::fprintf(stderr, "m8.unit.environment-capture: no python interpreter for probe test\n");
    }

    // 不存在的解释器：空结果不失败。
    YORI_CHECK(!probe_python_version("/nonexistent/python3.99").has_value());
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
