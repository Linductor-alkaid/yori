#include <executor/executor.hpp>
#include <string>

#include "runtime/executor_runtime.hpp"
#include "yori_test.hpp"

int main() {
  yori::runtime::ExecutorRuntime runtime;
  std::string error_message;

  YORI_CHECK(runtime.initialize({}, error_message));
  YORI_CHECK(error_message.empty());
  YORI_CHECK(runtime.is_initialized());

  auto answer = runtime.executor().submit_auto([] { return 42; });
  YORI_CHECK(answer.get() == 42);

  auto worker_shutdown = runtime.executor().submit_auto([&runtime] { return runtime.shutdown(); });
  YORI_CHECK(worker_shutdown.get() ==
             yori::runtime::ExecutorRuntimeShutdownResult::kRequestedFromWorker);
  YORI_CHECK(runtime.is_initialized());

  // worker 只能请求停止；必须由外部 owner 完成 teardown。
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  YORI_CHECK(!runtime.is_initialized());

  // 外部 owner 的显式 shutdown 必须可重复，不依赖析构时机。
  YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kNotInitialized);
  return yori::testing::failure_count == 0 ? 0 : 1;
}
