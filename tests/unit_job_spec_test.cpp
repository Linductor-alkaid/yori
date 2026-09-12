#include <chrono>
#include <string>
#include <string_view>
#include <yori/job/job.hpp>

#include "yori_test.hpp"

namespace {

yori::job::JobSpec valid_spec() {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"python", "train.py"};
  spec.cwd = "/srv/training";
  spec.env = {{"PATH", "/usr/bin"}, {"YORI_RUN", "test"}};
  spec.tensorboard_logdir = "runs/current";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
  return spec;
}

void check_error(const yori::job::JobSpec& spec, yori::job::JobSpecErrorCode expected) {
  const auto result = yori::job::validate(spec);
  YORI_CHECK(!result);
  YORI_CHECK(result.code == expected);
  YORI_CHECK(std::string(yori::job::to_string(result.code)) != "UNKNOWN");
}

}  // namespace

int main() {
  using yori::job::JobSpecErrorCode;
  using yori::job::JobSpecLimits;

  const auto good = valid_spec();
  YORI_CHECK(yori::job::validate(good));

  yori::job::JobCreationError creation_error;
  auto job = yori::job::Job::create(yori::job::JobId{42}, good, creation_error);
  YORI_CHECK(job != nullptr);
  YORI_CHECK(creation_error.code == yori::job::JobCreationErrorCode::kNone);
  YORI_CHECK(job->id().value() == 42);
  YORI_CHECK(job->state() == yori::job::JobState::kQueued);

  auto invalid_id_job = yori::job::Job::create(yori::job::JobId{}, good, creation_error);
  YORI_CHECK(invalid_id_job == nullptr);
  YORI_CHECK(creation_error.code == yori::job::JobCreationErrorCode::kInvalidJobId);

  auto spec = valid_spec();
  spec.owner_uid = 0;
  check_error(spec, JobSpecErrorCode::kRootOwnerNotAllowed);

  spec = valid_spec();
  spec.argv.clear();
  check_error(spec, JobSpecErrorCode::kEmptyArguments);

  spec = valid_spec();
  spec.argv.resize(JobSpecLimits::kMaxArgumentCount + 1, "x");
  check_error(spec, JobSpecErrorCode::kTooManyArguments);

  spec = valid_spec();
  spec.argv[1] = std::string(JobSpecLimits::kMaxSingleArgumentBytes + 1, 'x');
  check_error(spec, JobSpecErrorCode::kArgumentTooLong);

  spec = valid_spec();
  spec.argv = {"python", std::string(JobSpecLimits::kMaxSingleArgumentBytes, 'a'),
               std::string(JobSpecLimits::kMaxSingleArgumentBytes, 'b'),
               std::string(JobSpecLimits::kMaxSingleArgumentBytes, 'c'),
               std::string(JobSpecLimits::kMaxSingleArgumentBytes, 'd')};
  check_error(spec, JobSpecErrorCode::kArgumentsTooLarge);

  spec = valid_spec();
  spec.cwd = "relative/path";
  check_error(spec, JobSpecErrorCode::kInvalidWorkingDirectory);

  spec = valid_spec();
  spec.env.clear();
  for (std::size_t index = 0; index <= JobSpecLimits::kMaxEnvironmentVariables; ++index) {
    spec.env.emplace("VAR_" + std::to_string(index), "value");
  }
  check_error(spec, JobSpecErrorCode::kTooManyEnvironmentVariables);

  spec = valid_spec();
  spec.env = {{"INVALID=NAME", "value"}};
  check_error(spec, JobSpecErrorCode::kInvalidEnvironmentName);

  spec = valid_spec();
  spec.env = {{std::string(JobSpecLimits::kMaxEnvironmentNameBytes + 1, 'N'), "value"}};
  check_error(spec, JobSpecErrorCode::kEnvironmentNameTooLong);

  spec = valid_spec();
  spec.env = {{"VALUE", std::string(JobSpecLimits::kMaxEnvironmentValueBytes + 1, 'x')}};
  check_error(spec, JobSpecErrorCode::kEnvironmentValueTooLong);

  spec = valid_spec();
  spec.env.clear();
  for (std::size_t index = 0; index < 9; ++index) {
    spec.env.emplace("VAR_" + std::to_string(index),
                     std::string(JobSpecLimits::kMaxEnvironmentValueBytes, 'x'));
  }
  check_error(spec, JobSpecErrorCode::kEnvironmentTooLarge);

  spec = valid_spec();
  spec.gpu_request = 2;
  check_error(spec, JobSpecErrorCode::kUnsupportedGpuRequest);

  spec = valid_spec();
  spec.launch_profile = std::string(JobSpecLimits::kMaxLaunchProfileBytes + 1, 'x');
  check_error(spec, JobSpecErrorCode::kLaunchProfileTooLong);

  spec = valid_spec();
  spec.tensorboard_logdir = "../outside";
  check_error(spec, JobSpecErrorCode::kInvalidTensorboardLogdir);

  spec = valid_spec();
  spec.argv[1] = std::string("bad\0argument", 12);
  check_error(spec, JobSpecErrorCode::kInvalidArgument);

  spec = valid_spec();
  spec.submit_time = {};
  check_error(spec, JobSpecErrorCode::kInvalidSubmitTime);

  auto invalid_spec_job = yori::job::Job::create(yori::job::JobId{43}, spec, creation_error);
  YORI_CHECK(invalid_spec_job == nullptr);
  YORI_CHECK(creation_error.code == yori::job::JobCreationErrorCode::kInvalidSpec);
  YORI_CHECK(creation_error.spec_validation.code == JobSpecErrorCode::kInvalidSubmitTime);

  spec = valid_spec();
  spec.cwd = std::string(JobSpecLimits::kMaxWorkingDirectoryBytes, 'x');
  spec.cwd.front() = '/';
  YORI_CHECK(yori::job::validate(spec));

  spec = valid_spec();
  spec.launch_profile = std::string(JobSpecLimits::kMaxLaunchProfileBytes, 'x');
  YORI_CHECK(yori::job::validate(spec));

  // ---- M8：executable 与 env_metadata（DEC-011）------------------------------
  spec = valid_spec();
  spec.executable = "/opt/conda/envs/train/bin/python";
  spec.env_metadata = yori::job::EnvMetadata{yori::job::EnvSource::kConda, "3.11.5"};
  YORI_CHECK(yori::job::validate(spec));

  spec = valid_spec();
  spec.executable = std::string("rel\0ative", 9);
  check_error(spec, JobSpecErrorCode::kInvalidExecutable);

  spec = valid_spec();
  spec.executable = "relative/python";
  check_error(spec, JobSpecErrorCode::kInvalidExecutable);

  spec = valid_spec();
  spec.executable = "";
  check_error(spec, JobSpecErrorCode::kInvalidExecutable);

  spec = valid_spec();
  // 注意构造方式：gcc-12 对 "/" + std::string(... 形态有 -Wrestrict 误报
  // （operator+(const char*, string&&) 的已知缺陷），以 insert 规避。
  std::string oversized(JobSpecLimits::kMaxExecutableBytes, 'x');
  oversized.insert(oversized.begin(), '/');
  spec.executable = oversized;
  check_error(spec, JobSpecErrorCode::kInvalidExecutable);

  spec = valid_spec();
  spec.env_metadata = yori::job::EnvMetadata{yori::job::EnvSource::kVenv, std::nullopt};
  YORI_CHECK(yori::job::validate(spec));

  spec = valid_spec();
  spec.env_metadata = yori::job::EnvMetadata{yori::job::EnvSource::kVenv, ""};
  check_error(spec, JobSpecErrorCode::kInvalidPythonVersion);

  spec = valid_spec();
  spec.env_metadata = yori::job::EnvMetadata{
      yori::job::EnvSource::kNone, std::string(JobSpecLimits::kMaxPythonVersionBytes + 1, '3')};
  check_error(spec, JobSpecErrorCode::kInvalidPythonVersion);

  YORI_CHECK(std::string_view{"conda"} == yori::job::to_string(yori::job::EnvSource::kConda));
  YORI_CHECK(std::string_view{"venv"} == yori::job::to_string(yori::job::EnvSource::kVenv));
  YORI_CHECK(std::string_view{"none"} == yori::job::to_string(yori::job::EnvSource::kNone));

  return yori::testing::failure_count == 0 ? 0 : 1;
}
