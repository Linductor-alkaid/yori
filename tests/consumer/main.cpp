#include <yori/version.h>

#include <chrono>
#include <cstdio>
#include <utility>
#include <yori/job/job.hpp>

// 安装后最小 consumer（M0-05）：验证安装的公共头与导出库可用。
int main() {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};

  yori::job::JobCreationError error;
  auto job = yori::job::Job::create(yori::job::JobId{1}, std::move(spec), error);
  if (!job) {
    std::fprintf(stderr, "consumer failed to create Job (%d)\n", static_cast<int>(error.code));
    return 1;
  }

  std::printf("consumer linked against yori %s\n", yori::version());
  return 0;
}
