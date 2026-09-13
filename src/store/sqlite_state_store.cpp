#include "yori/store/sqlite_state_store.hpp"

#include <dlfcn.h>
#include <sys/stat.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "store/mutation_core.hpp"
#include "store/sqlite_api.hpp"

namespace yori::store {
namespace {

// load()/open() 完成后只读的符号表。
struct SqliteSymbols final {
  sqlite3_int64 (*libversion_number)() = nullptr;
  int (*open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
  int (*close_v2)(sqlite3*) = nullptr;
  int (*busy_timeout)(sqlite3*, int) = nullptr;
  const char* (*errmsg)(sqlite3*) = nullptr;
  int (*exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**) = nullptr;
  int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
  int (*step)(sqlite3_stmt*) = nullptr;
  int (*finalize)(sqlite3_stmt*) = nullptr;
  int (*reset)(sqlite3_stmt*) = nullptr;
  int (*bind_int64)(sqlite3_stmt*, int, sqlite3_int64) = nullptr;
  int (*bind_null)(sqlite3_stmt*, int) = nullptr;
  int (*bind_text)(sqlite3_stmt*, int, const char*, int, sqlite3_destructor_type) = nullptr;
  int (*bind_blob)(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type) = nullptr;
  sqlite3_int64 (*column_int64)(sqlite3_stmt*, int) = nullptr;
  const unsigned char* (*column_text)(sqlite3_stmt*, int) = nullptr;
  const void* (*column_blob)(sqlite3_stmt*, int) = nullptr;
  int (*column_bytes)(sqlite3_stmt*, int) = nullptr;
};

// detail 截断上限：errmsg/dlerror 只做诊断摘要，不进入结构化错误。
constexpr std::size_t kDetailLimit = 200;

std::string truncate_detail(const char* raw) {
  if (raw == nullptr) {
    return {};
  }
  std::string detail(raw);
  if (detail.size() > kDetailLimit) {
    detail.resize(kDetailLimit);
  }
  return detail;
}

// schema v3（DEC-012 的持久化格式承诺；兼容性变更需决策记录）。argv/env 以
// length-prefixed blob 编码，时间以 Unix epoch 纳秒编码，可空字段以独立
// has_* 标志或 SQL NULL 表示。v2 新增执行上下文列（executable/
// environment_type/python_version，均 NULL = 未捕获）；v3 新增 GPU placement
// 列（gpu_placement_mode：NULL/'any' = kAny、'required' = kRequired；
// gpu_placement_device：仅 required 时非空 UUID）；v1/v2 库打开时按单事务
// 增量迁移链补齐（migrate_schema_v1_to_v2 -> v2_to_v3）。
constexpr const char* kSchemaSql =
    "BEGIN IMMEDIATE;"
    "CREATE TABLE IF NOT EXISTS yori_meta("
    "  key TEXT PRIMARY KEY,"
    "  value INTEGER NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE IF NOT EXISTS yori_jobs("
    "  job_id INTEGER PRIMARY KEY,"
    "  state INTEGER NOT NULL,"
    "  revision INTEGER NOT NULL,"
    "  owner_uid INTEGER NOT NULL,"
    "  owner_gid INTEGER NOT NULL,"
    "  argv BLOB NOT NULL,"
    "  cwd TEXT NOT NULL,"
    "  env BLOB NOT NULL,"
    "  gpu_request INTEGER NOT NULL,"
    "  launch_profile TEXT,"
    "  tensorboard_logdir TEXT,"
    "  submit_time_nanos INTEGER NOT NULL,"
    "  pid INTEGER NOT NULL,"
    "  pgid INTEGER NOT NULL,"
    "  start_ticks INTEGER NOT NULL,"
    "  has_start_time INTEGER NOT NULL,"
    "  start_time_nanos INTEGER NOT NULL,"
    "  has_end_time INTEGER NOT NULL,"
    "  end_time_nanos INTEGER NOT NULL,"
    "  has_exit INTEGER NOT NULL,"
    "  exit_reason INTEGER NOT NULL,"
    "  exit_code INTEGER NOT NULL,"
    "  exit_signal INTEGER NOT NULL,"
    "  failure_reason TEXT,"
    "  log_path TEXT,"
    "  executable TEXT,"
    "  environment_type TEXT,"
    "  python_version TEXT,"
    "  gpu_placement_mode TEXT,"
    "  gpu_placement_device TEXT);"
    "CREATE TABLE IF NOT EXISTS yori_leases("
    "  gpu_uuid TEXT PRIMARY KEY,"
    "  job_id INTEGER NOT NULL UNIQUE) WITHOUT ROWID;"
    "INSERT OR IGNORE INTO yori_meta(key, value) VALUES ('schema_version', 3);"
    "INSERT OR IGNORE INTO yori_meta(key, value) VALUES ('revision', 0);"
    "COMMIT;";

// v1 -> v2 增量迁移（单事务；DEC-009 原子性纪律）：仅追加可空列，v1 行按
// NULL（未捕获）补全。
constexpr const char* kMigrationV1ToV2Sql =
    "BEGIN IMMEDIATE;"
    "ALTER TABLE yori_jobs ADD COLUMN executable TEXT;"
    "ALTER TABLE yori_jobs ADD COLUMN environment_type TEXT;"
    "ALTER TABLE yori_jobs ADD COLUMN python_version TEXT;"
    "UPDATE yori_meta SET value = 2 WHERE key = 'schema_version';"
    "COMMIT;";

// v2 -> v3 增量迁移（单事务；DEC-012）：追加 placement 可空列，v2 行按 NULL
// （kAny）补全。
constexpr const char* kMigrationV2ToV3Sql =
    "BEGIN IMMEDIATE;"
    "ALTER TABLE yori_jobs ADD COLUMN gpu_placement_mode TEXT;"
    "ALTER TABLE yori_jobs ADD COLUMN gpu_placement_device TEXT;"
    "UPDATE yori_meta SET value = 3 WHERE key = 'schema_version';"
    "COMMIT;";

void put_u32_le(std::string& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

bool take_u32_le(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint32_t& out) {
  if (static_cast<std::size_t>(end - cursor) < 4) {
    return false;
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(cursor[index]) << (8 * index);
  }
  cursor += 4;
  out = value;
  return true;
}

// 字符串列表编码：count(LE32) + [len(LE32) + bytes]...。解码对 count、长度与
// 总量做严格边界检查，任何越界都判失败（篡改/损坏数据不得静默截断）。
constexpr std::size_t kMaxEncodedItems = std::size_t{2} * 256;  // env 以 name/value 对展开

std::string encode_string_list(const std::vector<std::string>& items) {
  std::string blob;
  put_u32_le(blob, static_cast<std::uint32_t>(items.size()));
  for (const auto& item : items) {
    put_u32_le(blob, static_cast<std::uint32_t>(item.size()));
    blob.append(item);
  }
  return blob;
}

bool decode_string_list(const void* data, int byte_count, std::vector<std::string>& out) {
  out.clear();
  if (data == nullptr || byte_count < 4) {
    return false;
  }
  const auto* cursor = static_cast<const std::uint8_t*>(data);
  const auto* end = cursor + byte_count;
  std::uint32_t count = 0;
  if (!take_u32_le(cursor, end, count) || count > kMaxEncodedItems) {
    return false;
  }
  out.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint32_t length = 0;
    if (!take_u32_le(cursor, end, length) || static_cast<std::size_t>(end - cursor) < length) {
      return false;
    }
    out.emplace_back(reinterpret_cast<const char*>(cursor), length);
    cursor += length;
  }
  return cursor == end;
}

sqlite3_int64 encode_time(std::chrono::system_clock::time_point time) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch());
  return static_cast<sqlite3_int64>(nanos.count());
}

std::chrono::system_clock::time_point decode_time(sqlite3_int64 nanos) {
  return std::chrono::system_clock::time_point{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds{nanos})};
}

// 绑定辅助：text 用 TRANSIENT 复制，保证语句生命周期内不引用调用方内存。
bool bind_text(const SqliteSymbols& symbols, sqlite3_stmt* stmt, int index,
               const std::string& value) {
  return symbols.bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                           yori_sqlite_transient()) == SQLITE_OK;
}

bool bind_blob(const SqliteSymbols& symbols, sqlite3_stmt* stmt, int index,
               const std::string& value) {
  return symbols.bind_blob(stmt, index, value.data(), static_cast<int>(value.size()),
                           yori_sqlite_transient()) == SQLITE_OK;
}

// 语句 RAII：任何路径都 finalize，不留泄漏语句。
class StmtGuard final {
 public:
  StmtGuard(const SqliteSymbols& symbols, sqlite3* db, const char* sql) : symbols_(symbols) {
    prepared_ = symbols.prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK;
  }
  ~StmtGuard() {
    if (stmt_ != nullptr) {
      symbols_.finalize(stmt_);
    }
  }

  StmtGuard(const StmtGuard&) = delete;
  StmtGuard& operator=(const StmtGuard&) = delete;
  StmtGuard(StmtGuard&&) = delete;
  StmtGuard& operator=(StmtGuard&&) = delete;

  [[nodiscard]] bool prepared() const noexcept { return prepared_; }
  [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

 private:
  const SqliteSymbols& symbols_;
  sqlite3_stmt* stmt_{nullptr};
  bool prepared_{false};
};

std::optional<std::string> nullable_text(const SqliteSymbols& symbols, sqlite3_stmt* stmt,
                                         int column) {
  const auto* text = symbols.column_text(stmt, column);
  const int bytes = symbols.column_bytes(stmt, column);
  if (text == nullptr && bytes == 0) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

// 装载后的完整存储状态（验证通过）。
struct DecodedState final {
  std::uint64_t revision{0};
  std::map<job::JobId, StoredJob> jobs{};
  std::map<gpu::GpuUuid, gpu::GpuLease> leases{};
};

bool leaseable_state(job::JobState state) noexcept {
  return state == job::JobState::kStarting || state == job::JobState::kRunning ||
         state == job::JobState::kStopping;
}

}  // namespace

const char* to_string(SqliteStoreOpenCode code) noexcept {
  switch (code) {
    case SqliteStoreOpenCode::kOpened:
      return "OPENED";
    case SqliteStoreOpenCode::kAlreadyOpen:
      return "ALREADY_OPEN";
    case SqliteStoreOpenCode::kInvalidConfig:
      return "INVALID_CONFIG";
    case SqliteStoreOpenCode::kSymlinkedDatabasePath:
      return "SYMLINKED_DATABASE_PATH";
    case SqliteStoreOpenCode::kLibraryOpenFailed:
      return "LIBRARY_OPEN_FAILED";
    case SqliteStoreOpenCode::kSymbolMissing:
      return "SYMBOL_MISSING";
    case SqliteStoreOpenCode::kDatabaseOpenFailed:
      return "DATABASE_OPEN_FAILED";
    case SqliteStoreOpenCode::kSchemaInitFailed:
      return "SCHEMA_INIT_FAILED";
  }
  return "UNKNOWN";
}

class SqliteStateStore::Impl final {
 public:
  explicit Impl(SqliteStateStoreConfig config_value) : config(std::move(config_value)) {}

  ~Impl() { close(); }

  SqliteStoreOpenResult open();
  [[nodiscard]] bool is_open() const noexcept { return db != nullptr; }
  [[nodiscard]] StateStoreLoadResult load();
  [[nodiscard]] StateStoreWriteResult apply(const StateMutation& mutation);

 private:
  [[nodiscard]] std::string describe_error() const;
  bool exec_simple(const char* sql);
  [[nodiscard]] bool read_revision(std::uint64_t& revision);
  [[nodiscard]] bool decode_state(DecodedState& state, StateStoreErrorCode& error);
  [[nodiscard]] bool decode_jobs(DecodedState& state, StateStoreErrorCode& error);
  [[nodiscard]] bool decode_leases(DecodedState& state, StateStoreErrorCode& error);
  bool write_job_row(const StoredJob& record);
  bool delete_lease_row(const gpu::GpuUuid& uuid);
  bool insert_lease_row(const gpu::GpuLease& lease);
  bool write_revision(std::uint64_t revision);
  void close() noexcept;

  SqliteStateStoreConfig config;
  SqliteSymbols symbols{};
  sqlite3* db{nullptr};
};

SqliteStoreOpenResult SqliteStateStore::Impl::open() {
  if (db != nullptr) {
    return {SqliteStoreOpenCode::kAlreadyOpen, {}};
  }
  if (config.database_path.empty() || config.max_jobs == 0 || config.max_leases == 0 ||
      config.busy_timeout_ms < 0) {
    return {SqliteStoreOpenCode::kInvalidConfig,
            "database_path must be non-empty and limits must be positive"};
  }

  // 持久化目录防符号链接攻击的第一层（威胁模型基线 8）：数据库文件本身不得是
  // 符号链接；父目录链属主校验属打包/部署层（M7）。
  struct ::stat path_stat {};
  if (::lstat(config.database_path.c_str(), &path_stat) == 0) {
    if (S_ISLNK(path_stat.st_mode)) {
      return {SqliteStoreOpenCode::kSymlinkedDatabasePath, config.database_path};
    }
  }

  void* library = ::dlopen(config.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    return {SqliteStoreOpenCode::kLibraryOpenFailed, truncate_detail(::dlerror())};
  }

  SqliteSymbols resolved{};
  const std::pair<const char*, void**> bindings[] = {
      {"sqlite3_libversion_number", reinterpret_cast<void**>(&resolved.libversion_number)},
      {"sqlite3_open_v2", reinterpret_cast<void**>(&resolved.open_v2)},
      {"sqlite3_close_v2", reinterpret_cast<void**>(&resolved.close_v2)},
      {"sqlite3_busy_timeout", reinterpret_cast<void**>(&resolved.busy_timeout)},
      {"sqlite3_errmsg", reinterpret_cast<void**>(&resolved.errmsg)},
      {"sqlite3_exec", reinterpret_cast<void**>(&resolved.exec)},
      {"sqlite3_prepare_v2", reinterpret_cast<void**>(&resolved.prepare_v2)},
      {"sqlite3_step", reinterpret_cast<void**>(&resolved.step)},
      {"sqlite3_finalize", reinterpret_cast<void**>(&resolved.finalize)},
      {"sqlite3_reset", reinterpret_cast<void**>(&resolved.reset)},
      {"sqlite3_bind_int64", reinterpret_cast<void**>(&resolved.bind_int64)},
      {"sqlite3_bind_null", reinterpret_cast<void**>(&resolved.bind_null)},
      {"sqlite3_bind_text", reinterpret_cast<void**>(&resolved.bind_text)},
      {"sqlite3_bind_blob", reinterpret_cast<void**>(&resolved.bind_blob)},
      {"sqlite3_column_int64", reinterpret_cast<void**>(&resolved.column_int64)},
      {"sqlite3_column_text", reinterpret_cast<void**>(&resolved.column_text)},
      {"sqlite3_column_blob", reinterpret_cast<void**>(&resolved.column_blob)},
      {"sqlite3_column_bytes", reinterpret_cast<void**>(&resolved.column_bytes)},
  };
  for (const auto& [name, target] : bindings) {
    *target = ::dlsym(library, name);
    if (*target == nullptr) {
      const std::string detail = std::string("missing symbol ") + name;
      static_cast<void>(::dlclose(library));
      return {SqliteStoreOpenCode::kSymbolMissing, detail};
    }
  }

  sqlite3* opened = nullptr;
  if (resolved.open_v2(config.database_path.c_str(), &opened,
                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    const std::string detail = opened != nullptr && resolved.errmsg != nullptr
                                   ? truncate_detail(resolved.errmsg(opened))
                                   : "sqlite3_open_v2 failed";
    if (opened != nullptr) {
      static_cast<void>(resolved.close_v2(opened));
    }
    static_cast<void>(::dlclose(library));
    return {SqliteStoreOpenCode::kDatabaseOpenFailed, detail};
  }

  symbols = resolved;
  db = opened;
  static_cast<void>(symbols.busy_timeout(db, config.busy_timeout_ms));

  if (!exec_simple(kSchemaSql)) {
    const std::string detail = describe_error();
    close();
    return {SqliteStoreOpenCode::kSchemaInitFailed, detail};
  }

  // schema 版本核验与增量迁移（DEC-011/DEC-012）：v1 -> v2 -> v3 链式单事务
  // 追加可空列；更高版本（来自更新 daemon 的库）显式拒绝，不静默误读。
  {
    StmtGuard version_stmt(symbols, db,
                           "SELECT value FROM yori_meta WHERE key = 'schema_version';");
    if (!version_stmt.prepared() || symbols.step(version_stmt.get()) != SQLITE_ROW) {
      const std::string detail = describe_error();
      close();
      return {SqliteStoreOpenCode::kSchemaInitFailed, detail};
    }
    sqlite3_int64 schema_version = symbols.column_int64(version_stmt.get(), 0);
    if (schema_version == 1) {
      if (!exec_simple(kMigrationV1ToV2Sql)) {
        const std::string detail = describe_error();
        close();
        return {SqliteStoreOpenCode::kSchemaInitFailed, "v1->v2 migration failed: " + detail};
      }
      schema_version = 2;
    }
    if (schema_version == 2) {
      if (!exec_simple(kMigrationV2ToV3Sql)) {
        const std::string detail = describe_error();
        close();
        return {SqliteStoreOpenCode::kSchemaInitFailed, "v2->v3 migration failed: " + detail};
      }
      schema_version = 3;
    }
    if (schema_version != 3) {
      close();
      return {SqliteStoreOpenCode::kSchemaInitFailed,
              "unsupported schema version " + std::to_string(schema_version)};
    }
  }

  // 状态文件仅 daemon（root）读写：尽力收敛权限，失败不阻断打开（记录于
  // DEC-009 的部署约束）。
  static_cast<void>(::chmod(config.database_path.c_str(), 0600));
  return {SqliteStoreOpenCode::kOpened, {}};
}

std::string SqliteStateStore::Impl::describe_error() const {
  if (db != nullptr && symbols.errmsg != nullptr) {
    return truncate_detail(symbols.errmsg(db));
  }
  return "sqlite error";
}

bool SqliteStateStore::Impl::exec_simple(const char* sql) {
  return symbols.exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void SqliteStateStore::Impl::close() noexcept {
  if (db != nullptr) {
    static_cast<void>(symbols.close_v2(db));
    db = nullptr;
  }
}

bool SqliteStateStore::Impl::read_revision(std::uint64_t& revision) {
  StmtGuard stmt(symbols, db, "SELECT value FROM yori_meta WHERE key = 'revision';");
  if (!stmt.prepared()) {
    return false;
  }
  const int step = symbols.step(stmt.get());
  if (step != SQLITE_ROW) {
    return false;
  }
  const sqlite3_int64 value = symbols.column_int64(stmt.get(), 0);
  if (value < 0) {
    return false;
  }
  revision = static_cast<std::uint64_t>(value);
  return true;
}

bool SqliteStateStore::Impl::decode_jobs(DecodedState& state, StateStoreErrorCode& error) {
  error = StateStoreErrorCode::kNone;
  StmtGuard stmt(symbols, db,
                 "SELECT job_id, state, revision, owner_uid, owner_gid, argv,"
                 " cwd, env, gpu_request, launch_profile, tensorboard_logdir,"
                 " submit_time_nanos, pid, pgid, start_ticks, has_start_time,"
                 " start_time_nanos, has_end_time, end_time_nanos, has_exit,"
                 " exit_reason, exit_code, exit_signal, failure_reason, log_path,"
                 " executable, environment_type, python_version,"
                 " gpu_placement_mode, gpu_placement_device"
                 " FROM yori_jobs ORDER BY job_id;");
  if (!stmt.prepared()) {
    error = StateStoreErrorCode::kBackendUnavailable;
    return false;
  }

  int step = symbols.step(stmt.get());
  while (step == SQLITE_ROW) {
    StoredJob record;
    record.id = job::JobId{static_cast<std::uint64_t>(symbols.column_int64(stmt.get(), 0))};

    const sqlite3_int64 state_value = symbols.column_int64(stmt.get(), 1);
    if (state_value < 0 || state_value > 7) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    record.state = static_cast<job::JobState>(state_value);

    const sqlite3_int64 revision_value = symbols.column_int64(stmt.get(), 2);
    if (revision_value < 0) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    record.revision = static_cast<std::uint64_t>(revision_value);

    const sqlite3_int64 owner_uid = symbols.column_int64(stmt.get(), 3);
    const sqlite3_int64 owner_gid = symbols.column_int64(stmt.get(), 4);
    if (owner_uid < 0 || owner_uid > 0xFFFFFFFF || owner_gid < 0 || owner_gid > 0xFFFFFFFF) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    record.spec.owner_uid = static_cast<std::uint32_t>(owner_uid);
    record.spec.owner_gid = static_cast<std::uint32_t>(owner_gid);

    std::vector<std::string> argv;
    if (!decode_string_list(symbols.column_blob(stmt.get(), 5), symbols.column_bytes(stmt.get(), 5),
                            argv)) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    record.spec.argv = std::move(argv);

    if (const auto cwd = nullable_text(symbols, stmt.get(), 6)) {
      record.spec.cwd = *cwd;
    }

    std::vector<std::string> env_items;
    if (!decode_string_list(symbols.column_blob(stmt.get(), 7), symbols.column_bytes(stmt.get(), 7),
                            env_items) ||
        env_items.size() % 2 != 0) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    for (std::size_t index = 0; index < env_items.size(); index += 2) {
      record.spec.env.emplace(std::move(env_items[index]), std::move(env_items[index + 1]));
    }

    const sqlite3_int64 gpu_request = symbols.column_int64(stmt.get(), 8);
    if (gpu_request < 0 || gpu_request > 0xFFFFFFFF) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    record.spec.gpu_request = static_cast<std::uint32_t>(gpu_request);
    record.spec.launch_profile = nullable_text(symbols, stmt.get(), 9);
    record.spec.tensorboard_logdir = nullable_text(symbols, stmt.get(), 10);
    record.spec.submit_time = decode_time(symbols.column_int64(stmt.get(), 11));

    const sqlite3_int64 pid = symbols.column_int64(stmt.get(), 12);
    const sqlite3_int64 pgid = symbols.column_int64(stmt.get(), 13);
    const sqlite3_int64 start_ticks = symbols.column_int64(stmt.get(), 14);
    if (pid < 0 || pgid < 0 || start_ticks < 0) {
      error = StateStoreErrorCode::kInvalidExecutionRecord;
      return false;
    }
    record.execution.identity.pid = pid;
    record.execution.identity.pgid = pgid;
    record.execution.identity.start_ticks = static_cast<std::uint64_t>(start_ticks);

    const sqlite3_int64 has_start_time = symbols.column_int64(stmt.get(), 15);
    const sqlite3_int64 has_end_time = symbols.column_int64(stmt.get(), 17);
    const sqlite3_int64 has_exit = symbols.column_int64(stmt.get(), 19);
    if ((has_start_time != 0 && has_start_time != 1) || (has_end_time != 0 && has_end_time != 1) ||
        (has_exit != 0 && has_exit != 1)) {
      error = StateStoreErrorCode::kInvalidExecutionRecord;
      return false;
    }
    if (has_start_time == 1) {
      record.execution.start_time = decode_time(symbols.column_int64(stmt.get(), 16));
    }
    if (has_end_time == 1) {
      record.execution.end_time = decode_time(symbols.column_int64(stmt.get(), 18));
    }
    if (has_exit == 1) {
      const sqlite3_int64 reason = symbols.column_int64(stmt.get(), 20);
      const sqlite3_int64 code = symbols.column_int64(stmt.get(), 21);
      const sqlite3_int64 signal_number = symbols.column_int64(stmt.get(), 22);
      if (reason < 0 || reason > 2 || code < 0 || code > 255 || signal_number < 0 ||
          signal_number > 64) {
        error = StateStoreErrorCode::kInvalidExecutionRecord;
        return false;
      }
      record.execution.exit =
          process::ExitStatus{static_cast<process::ExitReason>(reason), static_cast<int>(code),
                              static_cast<int>(signal_number)};
    }
    record.execution.failure_reason = nullable_text(symbols, stmt.get(), 23);
    record.execution.log_path = nullable_text(symbols, stmt.get(), 24);

    // v2 执行上下文列（DEC-011）：environment_type 取值受控；python_version
    // 仅在 environment_type 存在时合法（篡改/损坏数据显式失败）。
    record.spec.executable = nullable_text(symbols, stmt.get(), 25);
    if (const auto environment_type = nullable_text(symbols, stmt.get(), 26)) {
      job::EnvMetadata metadata;
      if (*environment_type == "conda") {
        metadata.source = job::EnvSource::kConda;
      } else if (*environment_type == "venv") {
        metadata.source = job::EnvSource::kVenv;
      } else if (*environment_type == "none") {
        metadata.source = job::EnvSource::kNone;
      } else {
        error = StateStoreErrorCode::kInvalidJob;
        return false;
      }
      metadata.python_version = nullable_text(symbols, stmt.get(), 27);
      record.spec.env_metadata = std::move(metadata);
    } else if (nullable_text(symbols, stmt.get(), 27).has_value()) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }

    // v3 placement 列（DEC-012）：NULL/'any' = kAny（v2 行补全语义）；
    // 'required' 必须携带合法 UUID；device 不得脱离 required 单独出现。
    // 篡改/损坏数据显式失败，不静默降级为 kAny。
    const auto placement_mode = nullable_text(symbols, stmt.get(), 28);
    const auto placement_device = nullable_text(symbols, stmt.get(), 29);
    if (!placement_mode || *placement_mode == "any") {
      if (placement_device) {
        error = StateStoreErrorCode::kInvalidJob;
        return false;
      }
      record.spec.gpu_placement.mode = job::GpuPlacementMode::kAny;
    } else if (*placement_mode == "required") {
      if (!placement_device || !gpu::GpuUuid{*placement_device}.valid()) {
        error = StateStoreErrorCode::kInvalidJob;
        return false;
      }
      record.spec.gpu_placement.mode = job::GpuPlacementMode::kRequired;
      record.spec.gpu_placement.devices.push_back(gpu::GpuUuid{*placement_device});
    } else {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }

    if (!record.id.valid() || !job::validate(record.spec)) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    if ((record.state == job::JobState::kQueued) != (record.revision == 0)) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    if (!validate_execution(record.execution, record.state, record.spec)) {
      error = StateStoreErrorCode::kInvalidExecutionRecord;
      return false;
    }
    if (!state.jobs.emplace(record.id, record).second) {
      error = StateStoreErrorCode::kInvalidJob;
      return false;
    }
    step = symbols.step(stmt.get());
  }
  if (step != SQLITE_DONE) {
    error = StateStoreErrorCode::kBackendUnavailable;
    return false;
  }
  return true;
}

bool SqliteStateStore::Impl::decode_leases(DecodedState& state, StateStoreErrorCode& error) {
  error = StateStoreErrorCode::kNone;
  StmtGuard stmt(symbols, db, "SELECT gpu_uuid, job_id FROM yori_leases;");
  if (!stmt.prepared()) {
    error = StateStoreErrorCode::kBackendUnavailable;
    return false;
  }

  int step = symbols.step(stmt.get());
  while (step == SQLITE_ROW) {
    gpu::GpuLease lease;
    lease.gpu_uuid = gpu::GpuUuid{nullable_text(symbols, stmt.get(), 0).value_or(std::string{})};
    lease.job_id = job::JobId{static_cast<std::uint64_t>(symbols.column_int64(stmt.get(), 1))};
    if (!lease.gpu_uuid.valid() || !lease.job_id.valid()) {
      error = StateStoreErrorCode::kInvalidLease;
      return false;
    }
    if (!state.jobs.contains(lease.job_id)) {
      error = StateStoreErrorCode::kInvalidLease;
      return false;
    }
    if (!state.leases.emplace(lease.gpu_uuid, lease).second) {
      error = StateStoreErrorCode::kInvalidLease;
      return false;
    }
    step = symbols.step(stmt.get());
  }
  if (step != SQLITE_DONE) {
    error = StateStoreErrorCode::kBackendUnavailable;
    return false;
  }

  // lease 矩阵整体校验（设计第 12 节）：leaseable 状态恰一个 lease，其余为零。
  std::map<job::JobId, std::size_t> counts;
  for (const auto& [uuid, lease] : state.leases) {
    static_cast<void>(uuid);
    ++counts[lease.job_id];
  }
  for (const auto& [id, record] : state.jobs) {
    const auto count = counts.find(id);
    const std::size_t held = count == counts.end() ? 0 : count->second;
    if ((leaseable_state(record.state) && held != 1) ||
        (!leaseable_state(record.state) && held != 0)) {
      error = StateStoreErrorCode::kInvalidLease;
      return false;
    }
  }
  return true;
}

bool SqliteStateStore::Impl::decode_state(DecodedState& state, StateStoreErrorCode& error) {
  if (!read_revision(state.revision) || !decode_jobs(state, error) ||
      !decode_leases(state, error)) {
    if (error == StateStoreErrorCode::kNone) {
      error = StateStoreErrorCode::kBackendUnavailable;
    }
    return false;
  }
  return true;
}

bool SqliteStateStore::Impl::write_job_row(const StoredJob& record) {
  StmtGuard stmt(symbols, db,
                 "INSERT OR REPLACE INTO yori_jobs("
                 " job_id, state, revision, owner_uid, owner_gid, argv, cwd, env,"
                 " gpu_request, launch_profile, tensorboard_logdir, submit_time_nanos,"
                 " pid, pgid, start_ticks, has_start_time, start_time_nanos,"
                 " has_end_time, end_time_nanos, has_exit, exit_reason, exit_code,"
                 " exit_signal, failure_reason, log_path, executable,"
                 " environment_type, python_version, gpu_placement_mode,"
                 " gpu_placement_device)"
                 " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,"
                 " ?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30);");
  if (!stmt.prepared()) {
    return false;
  }

  std::vector<std::string> env_items;
  env_items.reserve(2 * record.spec.env.size());
  for (const auto& [name, value] : record.spec.env) {
    env_items.push_back(name);
    env_items.push_back(value);
  }

  // 绑定辅助：任何一步失败都立即终止，不静默丢错误。
  const auto bind_i64 = [&](int index, sqlite3_int64 value) {
    return symbols.bind_int64(stmt.get(), index, value) == SQLITE_OK;
  };
  const auto bind_nullable_text = [&](int index, const std::optional<std::string>& value) {
    if (!value.has_value()) {
      return symbols.bind_null(stmt.get(), index) == SQLITE_OK;
    }
    return bind_text(symbols, stmt.get(), index, *value);
  };

  if (!bind_i64(1, static_cast<sqlite3_int64>(record.id.value())) ||
      !bind_i64(2, static_cast<sqlite3_int64>(record.state)) ||
      !bind_i64(3, static_cast<sqlite3_int64>(record.revision)) ||
      !bind_i64(4, record.spec.owner_uid) || !bind_i64(5, record.spec.owner_gid) ||
      !bind_blob(symbols, stmt.get(), 6, encode_string_list(record.spec.argv)) ||
      !bind_text(symbols, stmt.get(), 7, record.spec.cwd) ||
      !bind_blob(symbols, stmt.get(), 8, encode_string_list(env_items)) ||
      !bind_i64(9, record.spec.gpu_request) ||
      !bind_nullable_text(10, record.spec.launch_profile) ||
      !bind_nullable_text(11, record.spec.tensorboard_logdir) ||
      !bind_i64(12, encode_time(record.spec.submit_time)) ||
      !bind_i64(13, record.execution.identity.pid) ||
      !bind_i64(14, record.execution.identity.pgid) ||
      !bind_i64(15, static_cast<sqlite3_int64>(record.execution.identity.start_ticks)) ||
      !bind_i64(16, record.execution.start_time.has_value() ? 1 : 0) ||
      !bind_i64(17, record.execution.start_time.has_value()
                        ? encode_time(*record.execution.start_time)
                        : 0) ||
      !bind_i64(18, record.execution.end_time.has_value() ? 1 : 0) ||
      !bind_i64(19, record.execution.end_time.has_value() ? encode_time(*record.execution.end_time)
                                                          : 0) ||
      !bind_i64(20, record.execution.exit.has_value() ? 1 : 0) ||
      !bind_i64(21, record.execution.exit.has_value()
                        ? static_cast<sqlite3_int64>(record.execution.exit->reason)
                        : 0) ||
      !bind_i64(22, record.execution.exit.has_value() ? record.execution.exit->exit_code : 0) ||
      !bind_i64(23, record.execution.exit.has_value() ? record.execution.exit->signal_number : 0) ||
      !bind_nullable_text(24, record.execution.failure_reason) ||
      !bind_nullable_text(25, record.execution.log_path) ||
      !bind_nullable_text(26, record.spec.executable) ||
      !bind_nullable_text(27, record.spec.env_metadata ? std::optional<std::string>{job::to_string(
                                                             record.spec.env_metadata->source)}
                                                       : std::nullopt) ||
      !bind_nullable_text(
          28, record.spec.env_metadata ? record.spec.env_metadata->python_version : std::nullopt) ||
      !bind_nullable_text(29, record.spec.gpu_placement.mode == job::GpuPlacementMode::kRequired
                                  ? std::optional<std::string>{"required"}
                                  : std::nullopt) ||
      !bind_nullable_text(
          30, record.spec.gpu_placement.mode == job::GpuPlacementMode::kRequired
                  ? std::optional<std::string>{record.spec.gpu_placement.devices.front().value()}
                  : std::nullopt)) {
    return false;
  }

  return symbols.step(stmt.get()) == SQLITE_DONE;
}

bool SqliteStateStore::Impl::delete_lease_row(const gpu::GpuUuid& uuid) {
  StmtGuard stmt(symbols, db, "DELETE FROM yori_leases WHERE gpu_uuid = ?1;");
  if (!stmt.prepared() || !bind_text(symbols, stmt.get(), 1, uuid.value())) {
    return false;
  }
  return symbols.step(stmt.get()) == SQLITE_DONE;
}

bool SqliteStateStore::Impl::insert_lease_row(const gpu::GpuLease& lease) {
  StmtGuard stmt(symbols, db, "INSERT INTO yori_leases(gpu_uuid, job_id) VALUES (?1, ?2);");
  if (!stmt.prepared() || !bind_text(symbols, stmt.get(), 1, lease.gpu_uuid.value())) {
    return false;
  }
  if (symbols.bind_int64(stmt.get(), 2, static_cast<sqlite3_int64>(lease.job_id.value())) !=
      SQLITE_OK) {
    return false;
  }
  return symbols.step(stmt.get()) == SQLITE_DONE;
}

bool SqliteStateStore::Impl::write_revision(std::uint64_t revision) {
  StmtGuard stmt(symbols, db, "UPDATE yori_meta SET value = ?1 WHERE key = 'revision';");
  if (!stmt.prepared() ||
      symbols.bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(revision)) != SQLITE_OK) {
    return false;
  }
  return symbols.step(stmt.get()) == SQLITE_DONE;
}

StateStoreLoadResult SqliteStateStore::Impl::load() {
  if (db == nullptr) {
    return {StateStoreErrorCode::kBackendUnavailable, {}};
  }
  DecodedState state;
  StateStoreErrorCode error = StateStoreErrorCode::kNone;
  if (!decode_state(state, error)) {
    return {error, {}};
  }

  StateSnapshot snapshot;
  snapshot.revision = state.revision;
  snapshot.jobs.reserve(state.jobs.size());
  snapshot.leases.reserve(state.leases.size());
  for (const auto& [id, record] : state.jobs) {
    static_cast<void>(id);
    snapshot.jobs.push_back(record);
  }
  for (const auto& [uuid, lease] : state.leases) {
    static_cast<void>(uuid);
    snapshot.leases.push_back(lease);
  }
  return {StateStoreErrorCode::kNone, std::move(snapshot)};
}

StateStoreWriteResult SqliteStateStore::Impl::apply(const StateMutation& mutation) {
  if (db == nullptr) {
    return {StateStoreErrorCode::kBackendUnavailable, 0};
  }
  if (!exec_simple("BEGIN IMMEDIATE;")) {
    return {StateStoreErrorCode::kBackendUnavailable, 0};
  }

  DecodedState state;
  StateStoreErrorCode error = StateStoreErrorCode::kNone;
  if (!decode_state(state, error)) {
    static_cast<void>(exec_simple("ROLLBACK;"));
    return {error, state.revision};
  }
  const std::uint64_t current_revision = state.revision;

  const auto outcome = mutation_core::apply_mutation(
      state.jobs, state.leases, current_revision, {config.max_jobs, config.max_leases}, mutation);
  if (!outcome.ok()) {
    static_cast<void>(exec_simple("ROLLBACK;"));
    return {outcome.code, current_revision};
  }

  // revision 必须可用 int64 表示（SQLite INTEGER 上限）；到达该上限的存储已
  // 不可延续，按无效 mutation 拒绝。
  if (outcome.next_revision > static_cast<std::uint64_t>(INT64_MAX)) {
    static_cast<void>(exec_simple("ROLLBACK;"));
    return {StateStoreErrorCode::kInvalidMutation, current_revision};
  }

  const auto backend_failure = [&]() {
    static_cast<void>(exec_simple("ROLLBACK;"));
    return StateStoreWriteResult{StateStoreErrorCode::kBackendUnavailable, current_revision};
  };

  for (const auto& record : mutation.create_jobs) {
    if (!write_job_row(record)) {
      return backend_failure();
    }
  }
  for (const auto& record : mutation.update_jobs) {
    if (!write_job_row(record)) {
      return backend_failure();
    }
  }
  for (const auto& uuid : mutation.release_leases) {
    if (!delete_lease_row(uuid)) {
      return backend_failure();
    }
  }
  for (const auto& lease : mutation.acquire_leases) {
    if (!insert_lease_row(lease)) {
      return backend_failure();
    }
  }
  if (!write_revision(outcome.next_revision) || !exec_simple("COMMIT;")) {
    return backend_failure();
  }
  return {StateStoreErrorCode::kNone, outcome.next_revision};
}

SqliteStateStore::SqliteStateStore(SqliteStateStoreConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

SqliteStateStore::~SqliteStateStore() = default;

SqliteStoreOpenResult SqliteStateStore::open() { return impl_->open(); }

bool SqliteStateStore::is_open() const noexcept { return impl_->is_open(); }

StateStoreLoadResult SqliteStateStore::load() { return impl_->load(); }

StateStoreWriteResult SqliteStateStore::apply(const StateMutation& mutation) {
  return impl_->apply(mutation);
}

}  // namespace yori::store
