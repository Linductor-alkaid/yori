#include <chrono>
#include <cstdint>
#include <executor/executor.hpp>
#include <future>
#include <memory>
#include <vector>
#include <yori/job/job.hpp>
#include <yori/store/state_store.hpp>

#include "runtime/executor_runtime.hpp"
#include "runtime/serial_state_store.hpp"
#include "testing/in_memory_state_store.hpp"
#include "yori_test.hpp"

// M7-02：SerialStateStore 的所有权串行化（读并发 + 单写者路径）。
namespace {

using namespace yori;
using namespace std::chrono_literals;

yori::store::StoredJob make_job(std::uint64_t id) {
  yori::store::StoredJob record;
  record.id = job::JobId{id};
  record.spec.owner_uid = 1000;
  record.spec.owner_gid = 1000;
  record.spec.argv = {"train"};
  record.spec.cwd = "/srv";
  record.spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
  record.state = job::JobState::kQueued;
  record.revision = 0;
  return record;
}

}  // namespace

int main() {
  // ---- 透传语义：结果、错误码与 revision CAS 不被包装改变 -------------------
  {
    auto inner = std::make_unique<yori::testing::InMemoryStateStore>();
    yori::runtime::SerialStateStore store(std::move(inner));

    yori::store::StateMutation create;
    create.expected_revision = 0;
    create.create_jobs.push_back(make_job(1));
    const auto write = store.apply(create);
    YORI_CHECK(write.ok() && write.revision == 1);

    // revision 冲突显式透传。
    const auto conflict = store.apply(create);
    YORI_CHECK(conflict.code == yori::store::StateStoreErrorCode::kRevisionConflict);

    const auto load = store.load();
    YORI_CHECK(load.ok() && load.snapshot.jobs.size() == 1);
    YORI_CHECK(store.serialized_calls() == 3);
  }

  // ---- 并发读 + 串行写：经 Executor 任务并发调用，串行化无交错损坏 ---------
  {
    yori::runtime::ExecutorRuntime runtime;
    std::string error;
    yori::runtime::ExecutorRuntimeConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    YORI_CHECK(runtime.initialize(config, error));

    auto inner = std::make_unique<yori::testing::InMemoryStateStore>();
    yori::runtime::SerialStateStore store(std::move(inner));

    // 预置 32 个 Job。
    {
      yori::store::StateMutation seed;
      seed.expected_revision = 0;
      for (std::uint64_t id = 1; id <= 32; ++id) {
        seed.create_jobs.push_back(make_job(id));
      }
      YORI_CHECK(store.apply(seed).ok());
    }

    // 读任务（并发）与写任务（单写者：状态推进）经 Executor 驱动；读任务在
    // 写进行中并发 load，全部结果一致（同 revision 快照）。
    std::vector<std::future<bool>> readers;
    readers.reserve(8);
    for (int i = 0; i < 8; ++i) {
      readers.push_back(runtime.executor().submit_auto([&store]() {
        for (int round = 0; round < 64; ++round) {
          const auto load = store.load();
          if (!load.ok() || load.snapshot.jobs.size() != 32) {
            return false;
          }
        }
        return true;
      }));
    }

    bool writes_ok = true;
    for (std::uint64_t id = 1; id <= 32 && writes_ok; ++id) {
      const auto load = store.load();
      if (!load.ok()) {
        writes_ok = false;
        break;
      }
      const yori::store::StoredJob* record = nullptr;
      for (const auto& candidate : load.snapshot.jobs) {
        if (candidate.id.value() == id) {
          record = &candidate;
          break;
        }
      }
      if (record == nullptr) {
        writes_ok = false;
        break;
      }
      yori::store::StoredJob updated = *record;
      updated.state = job::JobState::kCancelled;
      updated.revision = record->revision + 1;
      yori::store::StateMutation mutation;
      mutation.expected_revision = load.snapshot.revision;
      mutation.update_jobs.push_back(std::move(updated));
      writes_ok = store.apply(mutation).ok();
    }

    for (auto& reader : readers) {
      YORI_CHECK(reader.get());
    }
    YORI_CHECK(writes_ok);
    YORI_CHECK(store.serialized_calls() > std::uint64_t{8} * 64);

    YORI_CHECK(runtime.shutdown() == yori::runtime::ExecutorRuntimeShutdownResult::kCompleted);
  }

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "serial state store: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("serial state store: all checks passed\n");
  return 0;
}
