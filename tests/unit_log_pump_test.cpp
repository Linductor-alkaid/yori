#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <yori/job/job.hpp>
#include <yori/observe/log_sink.hpp>
#include <yori/process/process_supervisor.hpp>

#include "process_test_support.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/log_pump.hpp"
#include "yori_test.hpp"

namespace {

using namespace yori;
using namespace yori::runtime;
using namespace std::chrono_literals;

std::string make_directory() {
  char pattern[] = "/tmp/yori-log-pump-test-XXXXXX";
  char* dir = ::mkdtemp(pattern);
  YORI_CHECK(dir != nullptr);
  return dir;
}

std::string read_file(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::string data;
  char buffer[4096];
  size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.append(buffer, n);
  }
  std::fclose(file);
  return data;
}

observe::LogSink open_sink(const std::string& directory) {
  observe::LogSink sink;
  observe::LogSinkConfig config;
  config.directory = directory;
  observe::LogSinkOpenOptions options;
  options.owner_uid = static_cast<std::uint32_t>(::geteuid());
  options.owner_gid = static_cast<std::uint32_t>(::getegid());
  std::string error;
  YORI_CHECK(sink.open(config, options, error) == observe::LogSinkErrorCode::kNone);
  return sink;
}

}  // namespace

int main() {
  ExecutorRuntime runtime;
  std::string error;
  YORI_CHECK(runtime.initialize(ExecutorRuntimeConfig{}, error));

  // ---- 正常路径：attach -> 子进程输出 -> EOF -> done 事件 + 落盘 --------------
  {
    LogPump pump(runtime.executor());
    YORI_CHECK(pump.start().ok());

    const std::string directory = make_directory();
    process::ProcessSupervisor supervisor;
    auto spawned = supervisor.spawn(
        yori::testing::self_plan({"/bin/sh", "-c", "echo pump-out; echo pump-err 1>&2"}));
    YORI_CHECK(spawned);
    if (spawned) {
      LogPumpJobInput input;
      input.job = job::JobId{7};
      input.stdout_read = std::move(spawned.stdout_read);
      input.stderr_read = std::move(spawned.stderr_read);
      input.sink = open_sink(directory);
      YORI_CHECK(pump.attach(std::move(input)).ok());

      LogPumpDone done;
      YORI_CHECK(pump.receive_done_for(done, 5s));
      YORI_CHECK(done.job == job::JobId{7});
      YORI_CHECK(done.completed);
      YORI_CHECK(done.error.empty());
      YORI_CHECK(read_file(directory + "/stdout.log") == "pump-out\n");
      YORI_CHECK(read_file(directory + "/stderr.log") == "pump-err\n");
      YORI_CHECK(done.statistics.bytes_accepted == 18);
      YORI_CHECK(yori::testing::wait_for_exit(supervisor).status.is_success());
    }
    pump.stop();
  }

  // ---- 大输出与轮转：有界文件不丢数据（在轮转间分块） ------------------------
  {
    LogPump pump(runtime.executor());
    YORI_CHECK(pump.start().ok());

    const std::string directory = make_directory();
    process::ProcessSupervisor supervisor;
    auto spawned = supervisor.spawn(yori::testing::self_plan(
        {"/bin/sh", "-c", "for i in $(seq 1 600); do echo 0123456789abcdef; done"}));
    YORI_CHECK(spawned);
    if (spawned) {
      observe::LogSinkConfig config;
      config.directory = directory;
      config.max_file_bytes = 4096;
      config.rotation_history = 1;
      observe::LogSinkOpenOptions options;
      options.owner_uid = static_cast<std::uint32_t>(::geteuid());
      options.owner_gid = static_cast<std::uint32_t>(::getegid());
      observe::LogSink sink;
      std::string open_error;
      YORI_CHECK(sink.open(config, options, open_error) == observe::LogSinkErrorCode::kNone);

      LogPumpJobInput input;
      input.job = job::JobId{8};
      input.stdout_read = std::move(spawned.stdout_read);
      input.stderr_read = std::move(spawned.stderr_read);
      input.sink = std::move(sink);
      YORI_CHECK(pump.attach(std::move(input)).ok());

      LogPumpDone done;
      YORI_CHECK(pump.receive_done_for(done, 5s));
      const std::string current = read_file(directory + "/stdout.log");
      const std::string history = read_file(directory + "/stdout.log.1");
      // 600 * 17 = 10200 字节全部接受；保留 1 个历史文件后磁盘上存 6104 字节。
      YORI_CHECK(done.statistics.bytes_accepted == 10200);
      YORI_CHECK(current.size() + history.size() == 6104);
      YORI_CHECK(current.size() == 10200 - 2 * 4096);
      YORI_CHECK(done.statistics.rotations == 2);
    }
    pump.stop();
  }

  // ---- 多 Job 并发排空 --------------------------------------------------------
  {
    LogPump pump(runtime.executor());
    YORI_CHECK(pump.start().ok());

    const std::string dir_a = make_directory();
    const std::string dir_b = make_directory();
    process::ProcessSupervisor first;
    process::ProcessSupervisor second;
    auto spawned_a = first.spawn(yori::testing::self_plan({"/bin/sh", "-c", "echo a"}));
    auto spawned_b = second.spawn(yori::testing::self_plan({"/bin/sh", "-c", "echo b"}));
    YORI_CHECK(spawned_a && spawned_b);
    if (spawned_a && spawned_b) {
      LogPumpJobInput input_a;
      input_a.job = job::JobId{11};
      input_a.stdout_read = std::move(spawned_a.stdout_read);
      input_a.stderr_read = std::move(spawned_a.stderr_read);
      input_a.sink = open_sink(dir_a);
      YORI_CHECK(pump.attach(std::move(input_a)).ok());

      LogPumpJobInput input_b;
      input_b.job = job::JobId{12};
      input_b.stdout_read = std::move(spawned_b.stdout_read);
      input_b.stderr_read = std::move(spawned_b.stderr_read);
      input_b.sink = open_sink(dir_b);
      YORI_CHECK(pump.attach(std::move(input_b)).ok());

      bool got_a = false;
      bool got_b = false;
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while ((!got_a || !got_b) && std::chrono::steady_clock::now() < deadline) {
        LogPumpDone done;
        if (pump.receive_done_for(done, 1s)) {
          got_a = got_a || done.job == job::JobId{11};
          got_b = got_b || done.job == job::JobId{12};
        }
      }
      YORI_CHECK(got_a && got_b);
      YORI_CHECK(read_file(dir_a + "/stdout.log") == "a\n");
      YORI_CHECK(read_file(dir_b + "/stdout.log") == "b\n");
      YORI_CHECK(yori::testing::wait_for_exit(first).status.is_success());
      YORI_CHECK(yori::testing::wait_for_exit(second).status.is_success());
    }
    pump.stop();
  }

  // ---- detach：提前移除、读端关闭、无 done 事件 --------------------------------
  {
    LogPump pump(runtime.executor());
    YORI_CHECK(pump.start().ok());

    const std::string directory = make_directory();
    process::ProcessSupervisor supervisor;
    auto spawned = supervisor.spawn(yori::testing::self_plan({"sleep", "30"}));
    YORI_CHECK(spawned);
    if (spawned) {
      LogPumpJobInput input;
      input.job = job::JobId{21};
      input.stdout_read = std::move(spawned.stdout_read);
      input.stderr_read = std::move(spawned.stderr_read);
      input.sink = open_sink(directory);
      YORI_CHECK(pump.attach(std::move(input)).ok());

      const auto detached = pump.detach(job::JobId{21});
      YORI_CHECK(detached.ok());
      YORI_CHECK(pump.detach(job::JobId{21}).code == LogPumpDetachCode::kNotFound);

      LogPumpDone none;
      YORI_CHECK(!pump.try_receive_done(none));

      // 读端已关闭：子进程继续运行（SIGPIPE 已忽略，DEC-008），正常取消回收。
      YORI_CHECK(supervisor.request_cancel().terminating());
      YORI_CHECK(yori::testing::wait_for_exit(supervisor).status.signaled());
    }
    pump.stop();
  }

  // ---- shutdown：stop 关闭读端、worker join，子进程不受影响 --------------------
  {
    LogPump pump(runtime.executor());
    YORI_CHECK(pump.start().ok());

    const std::string directory = make_directory();
    process::ProcessSupervisor supervisor;
    auto spawned =
        supervisor.spawn(yori::testing::self_plan({"/bin/sh", "-c", "sleep 0.2; echo late"}));
    YORI_CHECK(spawned);
    if (spawned) {
      LogPumpJobInput input;
      input.job = job::JobId{31};
      input.stdout_read = std::move(spawned.stdout_read);
      input.stderr_read = std::move(spawned.stderr_read);
      input.sink = open_sink(directory);
      YORI_CHECK(pump.attach(std::move(input)).ok());

      pump.stop();  // daemon 关闭语义：不再排空，训练不终止。

      // 进程未被我们终止（可能因 EPIPE 报错退出，但绝不是被信号杀死）。
      const auto exit = yori::testing::wait_for_exit(supervisor);
      YORI_CHECK(!exit.status.signaled());
      // 停止后重复 stop 幂等；attach 显式拒绝。
      pump.stop();
      LogPumpJobInput late_input;
      late_input.job = job::JobId{32};
      late_input.sink = open_sink(make_directory());
      YORI_CHECK(pump.attach(std::move(late_input)).code == LogPumpAttachCode::kNotStarted);
    }
  }

  // ---- 无效输入拒绝 ------------------------------------------------------------
  {
    LogPump pump(runtime.executor());
    YORI_CHECK(pump.start().ok());
    LogPumpJobInput invalid;  // 无 fd、无打开 sink。
    YORI_CHECK(pump.attach(std::move(invalid)).code == LogPumpAttachCode::kInvalidInput);
    pump.stop();
  }

  YORI_CHECK(runtime.shutdown() == ExecutorRuntimeShutdownResult::kCompleted);
  return yori::testing::failure_count == 0 ? 0 : 1;
}
