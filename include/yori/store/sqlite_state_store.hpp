#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <yori/store/state_store.hpp>

namespace yori::store {

// SQLite 适配配置（DEC-009）。library_path 与 database_path 只能来自管理员配置
// 或测试注入，不得接受用户输入（威胁模型基线 20 同款纪律）；database_path 指向
// 的既有文件不得是符号链接（基线 8）。
struct SqliteStateStoreConfig final {
  std::string library_path{"libsqlite3.so.0"};
  std::string database_path;
  std::size_t max_jobs{1024};
  std::size_t max_leases{128};
  int busy_timeout_ms{5000};
};

enum class SqliteStoreOpenCode {
  kOpened,
  kAlreadyOpen,
  kInvalidConfig,
  kSymlinkedDatabasePath,
  kLibraryOpenFailed,
  kSymbolMissing,
  kDatabaseOpenFailed,
  kSchemaInitFailed,
};

[[nodiscard]] const char* to_string(SqliteStoreOpenCode code) noexcept;

struct SqliteStoreOpenResult final {
  SqliteStoreOpenCode code{SqliteStoreOpenCode::kOpened};
  std::string detail;

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == SqliteStoreOpenCode::kOpened || code == SqliteStoreOpenCode::kAlreadyOpen;
  }
  explicit constexpr operator bool() const noexcept { return ok(); }
};

// StateStore 的 SQLite 适配（Adapter 层，RULE-02）：实现 M1 冻结的 load()/apply()
// SPI，语义与 InMemoryStateStore 完全一致（共享 mutation 验证核心）。运行期
// dlopen 绑定系统 libsqlite3；apply 以 BEGIN IMMEDIATE 单事务完成
// "读 revision -> 内存验证 -> 写入 -> revision 递增"，任何失败显式回滚，不留
// 部分写入；load 对每行做结构校验，篡改数据（非法状态/revision/编码）显式
// 失败而非静默跳过。
//
// 生命周期纪律：open() 由 owner 调用一次；本类同步、单 owner、不隐藏线程、
// 锁或写队列（EXEC-08 的串行载载由外部 StoreTaskRunner/daemon 负责）；析构
// 关闭连接，不等待任何在途操作。
class SqliteStateStore final : public StateStore {
 public:
  explicit SqliteStateStore(SqliteStateStoreConfig config = {});
  ~SqliteStateStore() override;

  SqliteStateStore(const SqliteStateStore&) = delete;
  SqliteStateStore& operator=(const SqliteStateStore&) = delete;
  SqliteStateStore(SqliteStateStore&&) = delete;
  SqliteStateStore& operator=(SqliteStateStore&&) = delete;

  // 显式初始化：dlopen + dlsym + sqlite3_open_v2 + schema 事务。失败时保持
  // 未打开状态，detail 携带 errmsg/dlerror 摘要；未打开的 load()/apply()
  // 返回 kBackendUnavailable。
  [[nodiscard]] SqliteStoreOpenResult open();

  [[nodiscard]] bool is_open() const noexcept;

  [[nodiscard]] StateStoreLoadResult load() override;
  [[nodiscard]] StateStoreWriteResult apply(const StateMutation& mutation) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yori::store
