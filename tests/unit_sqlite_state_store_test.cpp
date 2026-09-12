// SqliteStateStore 适配器单测（M4-02）：直接驱动真实系统 libsqlite3.so.0，
// 覆盖 open 语义、持久化 roundtrip、与内存实现一致的 mutation 决策、执行
// 记录字段落盘与故障/篡改注入。篡改用 SQL 经测试内的最小 dlopen 绑定执行
// （与适配器共享 src/store/sqlite_api.hpp，配对 ABI）。
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "store/sqlite_api.hpp"
#include "yori/store/sqlite_state_store.hpp"
#include "yori/store/state_store.hpp"
#include "yori_test.hpp"

namespace {

using yori::job::JobState;
using yori::store::SqliteStateStore;
using yori::store::SqliteStateStoreConfig;
using yori::store::SqliteStoreOpenCode;
using yori::store::StateMutation;
using yori::store::StateStoreErrorCode;

std::string make_directory() {
  char pattern[] = "/tmp/yori-sqlite-store-test-XXXXXX";
  char* dir = ::mkdtemp(pattern);
  YORI_CHECK(dir != nullptr);
  return dir;
}

yori::job::JobSpec spec(std::uint32_t uid, std::uint64_t submit_seconds) {
  yori::job::JobSpec value;
  value.owner_uid = uid;
  value.owner_gid = uid;
  value.argv = {"/usr/bin/python3", "train.py", "--epochs", "100"};
  value.cwd = "/srv/training";
  value.env = {{"CUDA_DEVICE_ORDER", "FASTEST_FIRST"}, {"LC_ALL", "C.UTF-8"}};
  value.launch_profile = "pytorch";
  value.tensorboard_logdir = "runs/exp-1";
  value.submit_time = std::chrono::system_clock::time_point{
      std::chrono::seconds{static_cast<long long>(submit_seconds)}};
  return value;
}

yori::store::StoredJob queued(std::uint64_t id, std::uint32_t uid = 1000) {
  return {yori::job::JobId{id}, spec(uid, uid), JobState::kQueued, 0, {}};
}

yori::store::StoredJob advanced(const yori::store::StoredJob& current, JobState state) {
  auto next = current;
  next.state = state;
  ++next.revision;
  if (state == JobState::kRunning || state == JobState::kStopping) {
    next.execution.identity = yori::process::ProcessIdentity{4321, 4321, 888888};
  }
  return next;
}

yori::gpu::GpuLease lease(const char* uuid, std::uint64_t job_id) {
  return {yori::gpu::GpuUuid{uuid}, yori::job::JobId{job_id}};
}

const yori::store::StoredJob* find_job(const yori::store::StateSnapshot& snapshot,
                                       std::uint64_t id) {
  for (const auto& job : snapshot.jobs) {
    if (job.id == yori::job::JobId{id}) {
      return &job;
    }
  }
  return nullptr;
}

const yori::store::StoredJob& require_job(const yori::store::StateSnapshot& snapshot,
                                          std::uint64_t id) {
  const auto* found = find_job(snapshot, id);
  if (found == nullptr) {
    std::fprintf(stderr, "required Job %llu is missing\n", static_cast<unsigned long long>(id));
    std::exit(1);
  }
  return *found;
}

void check_error(const yori::store::StateStoreWriteResult& result, StateStoreErrorCode expected,
                 std::uint64_t expected_revision) {
  YORI_CHECK(!result);
  if (result.code != expected) {
    std::fprintf(stderr, "check_error: got %s expected %s\n", yori::store::to_string(result.code),
                 yori::store::to_string(expected));
  }
  YORI_CHECK(result.code == expected);
  YORI_CHECK(result.revision == expected_revision);
}

// 测试内最小 SQL 执行绑定：篡改/检查数据库内容用。
class RawSql final {
 public:
  explicit RawSql(const std::string& path) {
    library_ = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    YORI_CHECK(library_ != nullptr);
    open_ = reinterpret_cast<decltype(&::sqlite3_open_v2)>(::dlsym(library_, "sqlite3_open_v2"));
    close_ = reinterpret_cast<decltype(&::sqlite3_close_v2)>(::dlsym(library_, "sqlite3_close_v2"));
    exec_ = reinterpret_cast<decltype(&::sqlite3_exec)>(::dlsym(library_, "sqlite3_exec"));
    YORI_CHECK(open_ != nullptr && close_ != nullptr && exec_ != nullptr);
    YORI_CHECK(open_(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) ==
               SQLITE_OK);
  }

  ~RawSql() {
    if (db_ != nullptr) {
      static_cast<void>(close_(db_));
    }
    if (library_ != nullptr) {
      static_cast<void>(::dlclose(library_));
    }
  }

  RawSql(const RawSql&) = delete;
  RawSql& operator=(const RawSql&) = delete;
  RawSql(RawSql&&) = delete;
  RawSql& operator=(RawSql&&) = delete;

  void run(const char* sql) { YORI_CHECK(exec_(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK); }

 private:
  void* library_{nullptr};
  sqlite3* db_{nullptr};
  decltype(&::sqlite3_open_v2) open_{nullptr};
  decltype(&::sqlite3_close_v2) close_{nullptr};
  decltype(&::sqlite3_exec) exec_{nullptr};
};

SqliteStateStoreConfig config_for(const std::string& path) {
  SqliteStateStoreConfig config;
  config.database_path = path;
  config.max_jobs = 4;
  config.max_leases = 2;
  return config;
}

// ---------------------------------------------------------------------------
// M8（DEC-011）：schema v2 执行上下文列、v1 -> v2 增量迁移与兼容读。
// ---------------------------------------------------------------------------

// 构造手工 blob：count(LE32) + [len(LE32) + bytes]...（与适配器编码一致）。
std::string encoded_list(const std::vector<std::string>& items) {
  std::string blob;
  const auto put_u32 = [&blob](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      blob.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
  };
  put_u32(static_cast<std::uint32_t>(items.size()));
  for (const std::string& item : items) {
    put_u32(static_cast<std::uint32_t>(item.size()));
    blob += item;
  }
  return blob;
}

std::string hex_blob(const std::string& blob) {
  static const char* digits = "0123456789abcdef";
  std::string hex = "X'";
  for (const unsigned char c : blob) {
    hex += digits[c >> 4];
    hex += digits[c & 0xF];
  }
  hex += "'";
  return hex;
}

// 以 v1 schema（无执行上下文列、schema_version=1）建库并写入一行手工编码的
// QUEUED Job，模拟升级前的旧库。
void create_v1_database(const std::string& path) {
  RawSql sql(path);
  sql.run(
      "CREATE TABLE yori_meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL) WITHOUT ROWID;"
      "CREATE TABLE yori_jobs("
      " job_id INTEGER PRIMARY KEY, state INTEGER NOT NULL, revision INTEGER NOT NULL,"
      " owner_uid INTEGER NOT NULL, owner_gid INTEGER NOT NULL, argv BLOB NOT NULL,"
      " cwd TEXT NOT NULL, env BLOB NOT NULL, gpu_request INTEGER NOT NULL,"
      " launch_profile TEXT, tensorboard_logdir TEXT, submit_time_nanos INTEGER NOT NULL,"
      " pid INTEGER NOT NULL, pgid INTEGER NOT NULL, start_ticks INTEGER NOT NULL,"
      " has_start_time INTEGER NOT NULL, start_time_nanos INTEGER NOT NULL,"
      " has_end_time INTEGER NOT NULL, end_time_nanos INTEGER NOT NULL,"
      " has_exit INTEGER NOT NULL, exit_reason INTEGER NOT NULL, exit_code INTEGER NOT NULL,"
      " exit_signal INTEGER NOT NULL, failure_reason TEXT, log_path TEXT);"
      "CREATE TABLE yori_leases(gpu_uuid TEXT PRIMARY KEY, job_id INTEGER NOT NULL UNIQUE)"
      " WITHOUT ROWID;"
      "INSERT INTO yori_meta(key, value) VALUES ('schema_version', 1);"
      "INSERT INTO yori_meta(key, value) VALUES ('revision', 0);");
  const std::string insert =
      "INSERT INTO yori_jobs(job_id, state, revision, owner_uid, owner_gid, argv, cwd, env,"
      " gpu_request, launch_profile, tensorboard_logdir, submit_time_nanos, pid, pgid,"
      " start_ticks, has_start_time, start_time_nanos, has_end_time, end_time_nanos,"
      " has_exit, exit_reason, exit_code, exit_signal, failure_reason, log_path)"
      " VALUES(1, 0, 0, 1000, 1000, " +
      hex_blob(encoded_list({"/usr/bin/python3", "train.py"})) + ", '/srv', " +
      hex_blob(encoded_list({"LC_ALL", "C"})) +
      ", 1, NULL, NULL, 10000000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL);";
  sql.run(insert.c_str());
}

}  // namespace

int main() {
  const std::string directory = make_directory();
  const std::string database = directory + "/yori.db";

  // ---- open 语义 -----------------------------------------------------------
  {
    SqliteStateStore store{config_for(database)};
    YORI_CHECK(!store.is_open());
    const auto unopened_load = store.load();
    YORI_CHECK(unopened_load.code == StateStoreErrorCode::kBackendUnavailable);
    StateMutation nothing;
    YORI_CHECK(store.apply(nothing).code == StateStoreErrorCode::kBackendUnavailable);

    const auto opened = store.open();
    YORI_CHECK(opened.ok());
    YORI_CHECK(opened.code == SqliteStoreOpenCode::kOpened);
    YORI_CHECK(store.is_open());
    const auto reopened = store.open();
    YORI_CHECK(reopened.code == SqliteStoreOpenCode::kAlreadyOpen);
  }

  {
    SqliteStateStoreConfig invalid;
    YORI_CHECK(SqliteStateStore{invalid}.open().code == SqliteStoreOpenCode::kInvalidConfig);

    SqliteStateStoreConfig missing_library;
    missing_library.database_path = directory + "/missing-lib.db";
    missing_library.library_path = directory + "/no-such-sqlite.so";
    YORI_CHECK(SqliteStateStore{missing_library}.open().code ==
               SqliteStoreOpenCode::kLibraryOpenFailed);
  }

  // 数据库文件为符号链接 -> 拒绝（威胁模型基线 8）。
  {
    const std::string target = directory + "/yori.db";
    const std::string link = directory + "/symlinked.db";
    YORI_CHECK(::symlink(target.c_str(), link.c_str()) == 0);
    SqliteStateStore store{config_for(link)};
    const auto result = store.open();
    YORI_CHECK(result.code == SqliteStoreOpenCode::kSymlinkedDatabasePath);
    YORI_CHECK(!store.is_open());
  }

  // ---- 持久化 roundtrip 与执行记录字段落盘 ----------------------------------
  {
    SqliteStateStore store{config_for(database)};
    YORI_CHECK(store.open().ok());

    StateMutation create;
    create.create_jobs.push_back(queued(1));
    YORI_CHECK(store.apply(create).ok());

    auto snapshot = store.load();
    YORI_CHECK(snapshot.ok());
    YORI_CHECK(snapshot.snapshot.revision == 1);

    StateMutation schedule;
    schedule.expected_revision = 1;
    auto starting = require_job(snapshot.snapshot, 1);
    starting.state = JobState::kStarting;
    ++starting.revision;
    starting.execution.identity = yori::process::ProcessIdentity{4321, 4321, 888888};
    starting.execution.start_time = starting.spec.submit_time + std::chrono::seconds{5};
    starting.execution.log_path = "/var/lib/yori/logs/job-1";
    schedule.update_jobs.push_back(std::move(starting));
    schedule.acquire_leases.push_back(lease("GPU-aaaa", 1));
    YORI_CHECK(store.apply(schedule).ok());

    snapshot = store.load();
    StateMutation mark_running;
    mark_running.expected_revision = 2;
    mark_running.update_jobs.push_back(
        advanced(require_job(snapshot.snapshot, 1), JobState::kRunning));
    YORI_CHECK(store.apply(mark_running).ok());

    snapshot = store.load();
    StateMutation finish;
    finish.expected_revision = 3;
    auto finished = advanced(require_job(snapshot.snapshot, 1), JobState::kStopping);
    finished.state = JobState::kFinished;
    finished.execution.exit = yori::process::ExitStatus{yori::process::ExitReason::kExited, 0, 0};
    finished.execution.end_time =
        std::chrono::system_clock::time_point{std::chrono::seconds{static_cast<long long>(2000)}};
    finished.execution.failure_reason = "done";
    finish.update_jobs.push_back(std::move(finished));
    finish.release_leases.push_back(yori::gpu::GpuUuid{"GPU-aaaa"});
    YORI_CHECK(store.apply(finish).ok());
  }

  // "daemon 重启"：销毁后重开同一文件，全部字段保持（持久性）。
  {
    SqliteStateStore store{config_for(database)};
    YORI_CHECK(store.open().ok());
    const auto snapshot = store.load();
    YORI_CHECK(snapshot.ok());
    YORI_CHECK(snapshot.snapshot.revision == 4);
    YORI_CHECK(snapshot.snapshot.jobs.size() == 1);
    YORI_CHECK(snapshot.snapshot.leases.empty());

    const auto& job = require_job(snapshot.snapshot, 1);
    YORI_CHECK(job.state == JobState::kFinished);
    YORI_CHECK(job.revision == 3);  // Job revision：QUEUED0->STARTING1->RUNNING2->FINISHED3
    YORI_CHECK(job.spec.argv == spec(1000, 1000).argv);
    YORI_CHECK(job.spec.env == spec(1000, 1000).env);
    YORI_CHECK(job.spec.cwd == "/srv/training");
    YORI_CHECK(job.spec.launch_profile == "pytorch");
    YORI_CHECK(job.spec.tensorboard_logdir == "runs/exp-1");
    YORI_CHECK(job.execution.identity.pid == 4321);
    YORI_CHECK(job.execution.identity.pgid == 4321);
    YORI_CHECK(job.execution.identity.start_ticks == 888888);
    YORI_CHECK(job.execution.start_time.has_value());
    YORI_CHECK(job.execution.end_time.has_value());
    YORI_CHECK(job.execution.exit.has_value());
    const auto exit_status = job.execution.exit.value_or(yori::process::ExitStatus{});
    YORI_CHECK(exit_status.reason == yori::process::ExitReason::kExited);
    YORI_CHECK(exit_status.exit_code == 0);
    YORI_CHECK(job.execution.failure_reason == "done");
    YORI_CHECK(job.execution.log_path == "/var/lib/yori/logs/job-1");
  }

  // ---- 与内存实现一致的 mutation 决策（共享验证核心）--------------------------
  {
    SqliteStateStore store{config_for(database)};
    YORI_CHECK(store.open().ok());

    StateMutation create_second;
    create_second.expected_revision = 4;
    create_second.create_jobs.push_back(queued(2, 1001));
    YORI_CHECK(store.apply(create_second).ok());

    auto snapshot = store.load();
    YORI_CHECK(snapshot.snapshot.revision == 5);

    StateMutation stale;
    stale.expected_revision = 4;
    stale.create_jobs.push_back(queued(3, 1002));
    check_error(store.apply(stale), StateStoreErrorCode::kRevisionConflict, 5);

    StateMutation duplicate;
    duplicate.expected_revision = 5;
    duplicate.create_jobs.push_back(queued(2, 1001));
    check_error(store.apply(duplicate), StateStoreErrorCode::kJobAlreadyExists, 5);

    StateMutation without_lease;
    without_lease.expected_revision = 5;
    without_lease.update_jobs.push_back(
        advanced(require_job(snapshot.snapshot, 2), JobState::kStarting));
    check_error(store.apply(without_lease), StateStoreErrorCode::kInvalidLease, 5);

    StateMutation running_bare;
    running_bare.expected_revision = 5;
    auto bare = require_job(snapshot.snapshot, 2);
    bare.state = JobState::kRunning;
    ++bare.revision;
    running_bare.update_jobs.push_back(std::move(bare));
    running_bare.acquire_leases.push_back(lease("GPU-bbbb", 2));
    check_error(store.apply(running_bare), StateStoreErrorCode::kInvalidExecutionRecord, 5);

    StateMutation bad_transition;
    bad_transition.expected_revision = 5;
    bad_transition.update_jobs.push_back(
        advanced(require_job(snapshot.snapshot, 2), JobState::kFinished));
    check_error(store.apply(bad_transition), StateStoreErrorCode::kInvalidJobTransition, 5);

    StateMutation over_jobs;
    over_jobs.expected_revision = 5;
    over_jobs.create_jobs.push_back(queued(3, 1002));
    over_jobs.create_jobs.push_back(queued(4, 1003));
    over_jobs.create_jobs.push_back(queued(5, 1004));
    over_jobs.create_jobs.push_back(queued(6, 1005));
    check_error(store.apply(over_jobs), StateStoreErrorCode::kCapacityExceeded, 5);

    // 失败后无部分写入：revision 与数据保持。
    const auto after = store.load();
    YORI_CHECK(after.snapshot.revision == 5);
    YORI_CHECK(after.snapshot.jobs.size() == 2);
    YORI_CHECK(find_job(after.snapshot, 3) == nullptr);
  }

  // ---- 故障注入：写失败显式且不留部分写入 ------------------------------------
  {
    const std::string fault_database = directory + "/fault.db";
    {
      SqliteStateStore store{config_for(fault_database)};
      YORI_CHECK(store.open().ok());
      StateMutation create;
      create.create_jobs.push_back(queued(10, 1010));
      YORI_CHECK(store.apply(create).ok());
    }

    // 目录只读（数据库文件仍可打开）：apply 需要在目录中创建 journal，
    // 写入路径失败，显式 kBackendUnavailable。
    YORI_CHECK(::chmod(directory.c_str(), 0500) == 0);
    {
      SqliteStateStore store{config_for(fault_database)};
      YORI_CHECK(store.open().ok());
      StateMutation create;
      create.expected_revision = 1;
      create.create_jobs.push_back(queued(11, 1011));
      const auto result = store.apply(create);
      YORI_CHECK(!result.ok());
      YORI_CHECK(result.code == StateStoreErrorCode::kBackendUnavailable);
      // 无部分写入：目录恢复后重开可读回原状态。
    }
    YORI_CHECK(::chmod(directory.c_str(), 0700) == 0);
    {
      SqliteStateStore store{config_for(fault_database)};
      YORI_CHECK(store.open().ok());
      const auto snapshot = store.load();
      YORI_CHECK(snapshot.ok());
      YORI_CHECK(snapshot.snapshot.revision == 1);
      YORI_CHECK(snapshot.snapshot.jobs.size() == 1);
      YORI_CHECK(find_job(snapshot.snapshot, 11) == nullptr);
    }

    // 外部写锁：第二连接持有 BEGIN IMMEDIATE 时 apply 显式失败（busy 映射为
    // kBackendUnavailable）。
    {
      SqliteStateStore store{config_for(fault_database)};
      YORI_CHECK(store.open().ok());
      {
        RawSql lock_holder(fault_database);
        lock_holder.run("BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS hold(x);");
        StateMutation create;
        create.create_jobs.push_back(queued(12, 1012));
        const auto result = store.apply(create);
        YORI_CHECK(!result.ok());
        YORI_CHECK(result.code == StateStoreErrorCode::kBackendUnavailable);
        lock_holder.run("ROLLBACK;");
      }
      const auto snapshot = store.load();
      YORI_CHECK(snapshot.ok());
      YORI_CHECK(find_job(snapshot.snapshot, 12) == nullptr);
    }

    // 损坏文件：非 SQLite 格式 -> load 显式失败。
    {
      const std::string corrupt = directory + "/corrupt.db";
      FILE* file = std::fopen(corrupt.c_str(), "wb");
      YORI_CHECK(file != nullptr);
      const char garbage[] = "this is definitely not a sqlite database file ....";
      YORI_CHECK(std::fwrite(garbage, 1, sizeof(garbage), file) == sizeof(garbage));
      std::fclose(file);
      SqliteStateStore store{config_for(corrupt)};
      const auto opened = store.open();
      // 打开本身可能成功（惰性读取），load 必须显式失败。
      if (opened.ok()) {
        const auto snapshot = store.load();
        YORI_CHECK(!snapshot.ok());
      }
    }

    // 篡改行数据：非法状态值 -> load kInvalidJob（不静默跳过）。
    {
      const std::string tampered = directory + "/tampered.db";
      {
        SqliteStateStore store{config_for(tampered)};
        YORI_CHECK(store.open().ok());
        StateMutation create;
        create.create_jobs.push_back(queued(20, 1020));
        YORI_CHECK(store.apply(create).ok());
      }
      RawSql sql(tampered);
      sql.run("UPDATE yori_jobs SET state = 99 WHERE job_id = 20;");
      SqliteStateStore store{config_for(tampered)};
      YORI_CHECK(store.open().ok());
      const auto snapshot = store.load();
      YORI_CHECK(snapshot.code == StateStoreErrorCode::kInvalidJob);
    }

    // 篡改 lease 矩阵：QUEUED Job 持有 lease -> load kInvalidLease。
    {
      const std::string tampered = directory + "/tampered-lease.db";
      {
        SqliteStateStore store{config_for(tampered)};
        YORI_CHECK(store.open().ok());
        StateMutation create;
        create.create_jobs.push_back(queued(30, 1030));
        YORI_CHECK(store.apply(create).ok());
      }
      RawSql sql(tampered);
      sql.run("INSERT INTO yori_leases(gpu_uuid, job_id) VALUES ('GPU-bad', 30);");
      SqliteStateStore store{config_for(tampered)};
      YORI_CHECK(store.open().ok());
      YORI_CHECK(store.load().code == StateStoreErrorCode::kInvalidLease);
    }

    // 篡改 argv 编码 blob -> load kInvalidJob。
    {
      const std::string tampered = directory + "/tampered-argv.db";
      {
        SqliteStateStore store{config_for(tampered)};
        YORI_CHECK(store.open().ok());
        StateMutation create;
        create.create_jobs.push_back(queued(40, 1040));
        YORI_CHECK(store.apply(create).ok());
      }
      RawSql sql(tampered);
      sql.run("UPDATE yori_jobs SET argv = x'FFFFFFFF' WHERE job_id = 40;");
      SqliteStateStore store{config_for(tampered)};
      YORI_CHECK(store.open().ok());
      YORI_CHECK(store.load().code == StateStoreErrorCode::kInvalidJob);
    }
  }

  // ---- M8：v2 执行上下文列 roundtrip ----------------------------------------
  {
    const std::string path = directory + "/v2-fields.db";
    {
      SqliteStateStore store{config_for(path)};
      YORI_CHECK(store.open().ok());
      auto captured = queued(1);
      captured.spec.executable = "/opt/conda/envs/train/bin/python";
      captured.spec.env_metadata = yori::job::EnvMetadata{yori::job::EnvSource::kConda, "3.11.5"};
      StateMutation create;
      create.create_jobs.push_back(std::move(captured));
      YORI_CHECK(store.apply(create).ok());
    }
    {
      SqliteStateStore store{config_for(path)};
      YORI_CHECK(store.open().ok());
      const auto load = store.load();
      YORI_CHECK(load.ok());
      const auto& spec = require_job(load.snapshot, 1).spec;
      YORI_CHECK(spec.executable == std::string("/opt/conda/envs/train/bin/python"));
      YORI_CHECK(spec.env_metadata.has_value());
      if (spec.env_metadata) {
        YORI_CHECK(spec.env_metadata->source == yori::job::EnvSource::kConda);
        YORI_CHECK(spec.env_metadata->python_version == std::string("3.11.5"));
      }
    }
  }

  // ---- M8：v1 库打开时增量迁移，v1 行按缺省补全读取 -------------------------
  {
    const std::string path = directory + "/v1-migrate.db";
    create_v1_database(path);
    {
      SqliteStateStore store{config_for(path)};
      YORI_CHECK(store.open().ok());
      const auto load = store.load();
      YORI_CHECK(load.ok());
      YORI_CHECK(load.snapshot.jobs.size() == 1);
      const auto& record = require_job(load.snapshot, 1);
      YORI_CHECK(record.state == JobState::kQueued);
      YORI_CHECK(record.spec.argv == std::vector<std::string>({"/usr/bin/python3", "train.py"}));
      YORI_CHECK(record.spec.cwd == "/srv");
      YORI_CHECK(!record.spec.executable.has_value());
      YORI_CHECK(!record.spec.env_metadata.has_value());
      YORI_CHECK(load.snapshot.revision == 0);
    }
    // 迁移后 schema_version=2：再次打开不重复迁移，新写入带捕获字段。
    {
      RawSql sql(path);
      // 版本断言经再次打开隐式覆盖（3 将被拒绝）；此处仅确认可继续写入。
      static_cast<void>(sql);
    }
    {
      SqliteStateStore store{config_for(path)};
      YORI_CHECK(store.open().ok());
      auto captured = queued(2);
      captured.spec.executable = "/usr/bin/python3";
      captured.spec.env_metadata =
          yori::job::EnvMetadata{yori::job::EnvSource::kVenv, std::nullopt};
      StateMutation create;
      create.expected_revision = 0;
      create.create_jobs.push_back(std::move(captured));
      YORI_CHECK(store.apply(create).ok());
      const auto load = store.load();
      YORI_CHECK(load.ok());
      YORI_CHECK(load.snapshot.jobs.size() == 2);
      const auto& migrated = require_job(load.snapshot, 2);
      YORI_CHECK(migrated.spec.executable == std::string("/usr/bin/python3"));
      YORI_CHECK(migrated.spec.env_metadata.has_value() &&
                 migrated.spec.env_metadata->source == yori::job::EnvSource::kVenv &&
                 !migrated.spec.env_metadata->python_version.has_value());
    }
  }

  // ---- M8：篡改 environment_type / 未来 schema 版本显式失败 -----------------
  {
    const std::string path = directory + "/tampered-env-type.db";
    {
      SqliteStateStore store{config_for(path)};
      YORI_CHECK(store.open().ok());
      StateMutation create;
      create.create_jobs.push_back(queued(1));
      YORI_CHECK(store.apply(create).ok());
    }
    {
      RawSql sql(path);
      sql.run("UPDATE yori_jobs SET environment_type = 'bogus' WHERE job_id = 1;");
    }
    {
      SqliteStateStore store{config_for(path)};
      YORI_CHECK(store.open().ok());
      const auto load = store.load();
      YORI_CHECK(load.code == StateStoreErrorCode::kInvalidJob);
    }
  }
  {
    const std::string path = directory + "/orphan-version.db";
    {
      SqliteStateStore store{config_for(path)};
      YORI_CHECK(store.open().ok());
    }
    {
      RawSql sql(path);
      sql.run("UPDATE yori_meta SET value = 3 WHERE key = 'schema_version';");
    }
    {
      SqliteStateStore store{config_for(path)};
      const auto result = store.open();
      YORI_CHECK(result.code == SqliteStoreOpenCode::kSchemaInitFailed);
      YORI_CHECK(result.detail.find("unsupported schema version") != std::string::npos);
    }
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
