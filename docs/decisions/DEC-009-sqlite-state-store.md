# DEC-009：SQLite StateStore 采用与 dlopen 绑定

> 状态：Accepted
> 日期：2026-09-09
> 负责人：Linductor-alkaid
> 冻结里程碑：M4
> 替代/被替代：无

## 背景与问题

总计划第 6 节将"持久化实现"列为暂定默认值：SQLite 为唯一 `StateStore` 实现，
内存实现仅测试用，最迟 M4 冻结。M4 需要落定三件事：生产实现选型、SQLite 的
接入方式（构建期链接还是运行期绑定）、以及持久化格式（schema 与编码）的
兼容性承诺——后者属于公开契约，变更必须受控。

## 决策

1. **SQLite 是唯一生产 `StateStore` 实现**（`SqliteStateStore`，
   `src/store/sqlite_state_store.cpp`）；`InMemoryStateStore` 保留为测试实现。
   两者的 mutation 接受/拒绝决策由共享验证核心（`src/store/mutation_core.cpp`）
   保证同语义，SQLite 侧以单事务（`BEGIN IMMEDIATE`）完成
   "读 revision -> 内存验证 -> 写入 -> revision 递增"，失败显式回滚。
2. **运行期 `dlopen` 绑定系统 `libsqlite3.so.0`**，不做构建期链接：沿用 M3
   NVML 的内部最小 C API 声明模式（`src/store/sqlite_api.hpp`，不安装、不进
   公共头）。库路径只接受管理员配置或测试注入，不接受用户输入。构建与测试
   环境仅需运行时库（本机与 CI 均无 libsqlite3-dev 但均有 `libsqlite3.so.0`）。
3. **持久化格式（首版 schema 1）**：
   - `yori_meta(key, value)`：`schema_version`（当前 1）与全局 `revision`；
   - `yori_jobs`：25 列，`job_id` 主键；argv/env 以 length-prefixed（LE32）
     blob 编码；时间以 Unix epoch 纳秒 int64 编码；可空字段（launch_profile、
     tensorboard_logdir、failure_reason、log_path）用 SQL NULL，可空时间与
     退出状态用 `has_*` 标志列；
   - `yori_leases(gpu_uuid, job_id)`：一对一唯一约束。
   - 打开既有数据库时执行幂等 schema 初始化（`CREATE TABLE IF NOT EXISTS` +
     `INSERT OR IGNORE`）；数据库文件为符号链接时拒绝打开（威胁模型基线 8），
   打开后尽力收敛权限为 `0600`（失败不阻断）。
4. **行为映射**：SQLite busy/locked、损坏文件、SQL 失败统一映射为
   `kBackendUnavailable`；行数据被篡改（非法状态/revision 不一致/编码越界/
   lease 矩阵破坏）时 `load()` 以 `kInvalidJob`/`kInvalidExecutionRecord`/
   `kInvalidLease` 显式失败，不静默跳过。store revision 以 SQLite INTEGER
   （int64）上限为渐进上限，超出即按无效 mutation 拒绝。
5. **并发承载**：adapter 保持同步、单 owner、无内部线程/锁/写队列；运行期
   写路径的 Executor 串行化由 `StoreTaskRunner`（总计划 `EXEC-08`）承载。

## 备选方案

- 构建期链接系统 SQLite（`find_package(SQLite3)`）：需要 libsqlite3-dev，
  开发机与目标最小环境不保证具备且无法离线校验，构建依赖面扩大；否决。
- 仓库内 vendoring sqlite3 amalgamation：约 9 MiB 源码进入仓库，供应链锁定、
  许可证清单与升级审计成本高，且收益仅为去掉 dlopen；作为 dlopen 方案失效时
  的后备（届时需新决策记录）。
- 自定义二进制日志/快照格式：无查询与审计能力，与设计第 12 节（恢复、历史
  查询、审计、故障诊断）目标不符；否决。

## 影响与风险

- 内部声明的 ABI 风险：仅消费 SQLite 3.x 稳定 C API 子集与返回码；真实库
  行为（busy 语义、损坏文件返回码）由直接驱动真实 `libsqlite3.so.0` 的单测
  与故障注入覆盖（`m4.unit.sqlite-state-store`）。
- schema 变更兼容性：首版不做迁移；未来格式变更必须新决策记录并携带迁移
  策略（schema_version 字段已预留判别位）。
- dlopen 绑定与直接链接的行为差异可忽略（同一进程内单连接、单线程使用）；
  daemon 崩溃后的 journal 回滚由 SQLite 自身保证，集成测试覆盖重开一致性。

## 验证方式

M4 测试：open 语义（幂等/库缺失/符号链接拒绝）、持久化 roundtrip（含执行
记录全字段）、与内存实现一致的 mutation 错误码矩阵、故障注入（目录只写丢失
后 apply 显式失败且无部分写入、外部写锁 busy、损坏文件、篡改行/篡改 lease
矩阵/篡改 argv 编码）与 daemon 重启恢复集成闭环
（`m4.integration.recovery-restart`）。五预设与 PR CI 全绿后勾选 M4 工作项。

## 关联文档和工作项

设计第 6.2、12、18 节；总计划第 6 节（持久化实现冻结）、`EXEC-08`、`RULE-02`、
`RULE-06`；[威胁模型](../security/threat-model.md) 基线 8/13；M4 工作项
`M4-01`～`M4-05`（[M4 计划](../plans/m4-persistence-recovery.md)）。
