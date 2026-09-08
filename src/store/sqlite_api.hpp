#pragma once

// 内部最小 SQLite3 C API 声明（M4-02）。只声明 SqliteStateStore 实际绑定的
// 符号与常量，取值以 SQLite 公开文档的稳定子集为准（3.x 起未变化）。该头
// 不安装、不得被公共头包含；适配器经 dlopen + dlsym 绑定真实
// libsqlite3.so.0，构建不依赖 libsqlite3-dev。

extern "C" {

using sqlite3 = struct sqlite3;
using sqlite3_stmt = struct sqlite3_stmt;
using sqlite3_destructor_type = void (*)(void*);
using sqlite3_int64 = long long;

// 结果码子集（数值以 SQLite 发布 ABI 为准）。
enum sqlite3_result {
  SQLITE_OK = 0,
  SQLITE_BUSY = 5,
  SQLITE_LOCKED = 6,
  SQLITE_ROW = 100,
  SQLITE_DONE = 101,
};

// sqlite3_open_v2 的 flags 子集。
enum sqlite3_open_flags {
  SQLITE_OPEN_READWRITE = 0x00000002,
  SQLITE_OPEN_CREATE = 0x00000004,
};

// bind 析构器：TRANSIENT 表示返回前复制绑定内容。
#define YORI_SQLITE_TRANSIENT ((sqlite3_destructor_type) - 1)

sqlite3_int64 sqlite3_libversion_number(void);

int sqlite3_open_v2(const char* filename, sqlite3** db, int flags, const char* z_vfs);
int sqlite3_close_v2(sqlite3* db);
int sqlite3_busy_timeout(sqlite3* db, int milliseconds);
const char* sqlite3_errmsg(sqlite3* db);
int sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**),
                 void* arg, char** errmsg);

int sqlite3_prepare_v2(sqlite3* db, const char* sql, int n_byte, sqlite3_stmt** stmt,
                       const char** tail);
int sqlite3_step(sqlite3_stmt* stmt);
int sqlite3_finalize(sqlite3_stmt* stmt);
int sqlite3_reset(sqlite3_stmt* stmt);

int sqlite3_bind_int64(sqlite3_stmt* stmt, int index, sqlite3_int64 value);
int sqlite3_bind_null(sqlite3_stmt* stmt, int index);
int sqlite3_bind_text(sqlite3_stmt* stmt, int index, const char* value, int bytes,
                      sqlite3_destructor_type destructor);
int sqlite3_bind_blob(sqlite3_stmt* stmt, int index, const void* value, int bytes,
                      sqlite3_destructor_type destructor);

sqlite3_int64 sqlite3_column_int64(sqlite3_stmt* stmt, int column);
const unsigned char* sqlite3_column_text(sqlite3_stmt* stmt, int column);
const void* sqlite3_column_blob(sqlite3_stmt* stmt, int column);
int sqlite3_column_bytes(sqlite3_stmt* stmt, int column);

}  // extern "C"
