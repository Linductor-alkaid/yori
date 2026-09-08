# M4：持久化与恢复

> 状态：In Progress
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M2（[进程守护与启动适配](m2-process-supervision.md)）；与 M3 无相互依赖
> 建议发布点：无
> 更新日期：2026-09-09

## 目标

交付 Yori 的持久化与恢复闭环：`SqliteStateStore`（生产 `StateStore` 实现，`dlopen`
绑定系统 SQLite，事务保证 mutation 原子性）、`StateStore` 契约的执行记录扩展
（进程身份、起止时间、退出状态、失败原因、日志路径）与 daemon 重启恢复 Core
（快照载入 -> 进程身份核验 -> 采纳或 `LOST` -> 队列重建 -> 调度触发），并交付
`EXEC-08` 的 Executor 载载（`StoreTaskRunner`）。恢复绝不无条件重启数据库中的
RUNNING Job（`RULE-06`）。

## 范围与非目标

范围：

- 契约扩展（M1 SPI 的向后兼容增量）：`StoredJob` 增加执行记录字段（`ProcessIdentity`
  pid/pgid/start_ticks、start/end 时间、退出状态、`failure_reason`、`log_path`），
  新增对应结构校验规则与稳定错误码；mutation 验证逻辑提取到 Core 共享函数，
  内存实现与 SQLite 实现保持同一语义。
- `SqliteStateStore` 公开契约与实现：显式 `open()`（`dlopen` + `dlsym` +
  `sqlite3_open_v2` + schema 初始化），`load()` 返回同一 revision 的 Job/lease
  一致快照（含执行记录），`apply()` 在单事务内完成"读 revision -> 内存验证 ->
  写入 -> revision 递增"，任何失败回滚且不留部分写入；现有 revision/原子性/
  lease 矩阵语义与 `InMemoryStateStore` 完全一致。
- 持久化安全基线（威胁模型基线 8/13 的 M4 部分）：数据库文件为符号链接时拒绝
  打开；行数据被篡改（非法状态/revision 不一致）时 `load()` 显式失败，不静默
  跳过。
- 恢复 Core（`yori/recovery/`）：输入 StateStore 快照，对每个活动 Job
  （`STARTING/RUNNING/STOPPING`）以 `/proc/<pid>/stat` 的 PGID 与启动 ticks 核验
  身份；核验通过则采纳（`STARTING` 提升为 `RUNNING`，其余保留，lease 不变），
  无法核验（进程消失、启动时间/PGID 不符、身份缺失）则转 `LOST` 并释放 lease；
  `QUEUED` Job 经 `GlobalJobQueue::restore()` 重建派生队列；结果以显式
  mutation + 队列事件 + `kRecoveryCompleted` 触发收敛。
- `StoreTaskRunner`（`EXEC-08` 载载，`yori_runtime`）：单在飞 mutation 的
  `submit_cancellable` 串行载载，前一结果未消费时显式 `BUSY`，停止生产后
  `NOT_ACCEPTING`，取消/异常/关闭全部为显式结果。
- 测试：SQLite adapter 单测（真实 `libsqlite3.so.0`，含故障注入：只读文件、
  外部写锁、损坏文件、篡改行、符号链接、库缺失）；恢复决策单测；daemon 重启
  集成闭环（真实子进程 + SQLite 重开 + 调度触发，替换 `example.recovery.restart`
  占位，`recovery` 标签）。
- 文档：设计第 6.2/12/18 节、[DEC-009](../decisions/DEC-009-sqlite-state-store.md)、
  威胁模型（基线 8/13 落地 + 恢复/PID reuse 条目）、总计划第 1/5/6/11 节与本计划。

非目标：

- daemon 总装（IPC、`PhaseGate` 启动编排、JobManager 与守护进程生命周期）——
  M5 起落地；M4 的恢复以 Core 服务 + 集成测试验证。
- 恢复后的日志续捕与 offset 语义（DEC-008 已冻结"重启窗口输出丢失"；观察面
  细节 M6 复核）。
- STOPPING Job 恢复后的取消重发动作（重发 SIGTERM + 宽限重建需要进程句柄持有方，
  属 daemon 守护层；M4 的恢复结果显式标记"需要重发取消"，动作随 M5 总装接入）。
- 数据库 schema 迁移（首版 schema；格式变更兼容策略随未来需求决策）。
- SQLite 网络文件系统、多进程并发写者（Yori 单 authoritative daemon，单连接；
  busy 处理仅用于故障注入显式化）。
- `yori ps`/`yori queue` 等历史查询命令（M5 IPC/CLI）。

## 设计与决策依据

- 持久化范围与 SPI 语义：[设计文档](../design/yori-project-design.md)第 12 节
  （`load()`/`apply()`、mutation 上限、revision 契约、lease 矩阵；"SQLite 用于
  daemon 重启恢复与审计，不是多 scheduler 协调"）。
- 恢复流程与 `LOST` 语义：设计第 6.2 节、`RULE-06`（核验 PID/PGID/启动时间；
  无法确认进入 `LOST`；绝不能因 daemon 重启就重启 RUNNING Job）、
  [DEC-008](../decisions/DEC-008-daemon-restart-log-continuity.md) 第 2 条
  （核验通过后继续标记 `RUNNING`，重启窗口日志丢失不回退 offset）。
- 进程身份三元组：M2 冻结的 `ProcessIdentity`（`/proc/<pid>/stat` 启动 ticks +
  PGID，设计第 10.2 节），PID reuse 核验依据。
- Executor 承载：总计划 `EXEC-08`（SQLite 写入与恢复读取为有限任务 `submit_auto()`
  串行载载、写路径有界、拒绝显式化、关闭阶段 ⑦ 等待终态落盘）；adapter 本身
  同步、不隐藏线程或写队列（设计第 12 节）。恢复读取在 daemon 启动序列执行
  （`EXEC-09` 启动阶段 `PhaseGate`：恢复 -> GPU 观察 -> 调度开启；`PhaseGate`
  总装随 M5 落地，M4 以 Core 服务先行）。
- 平台解耦：`RULE-02`（SQLite 只经 `StateStore` 接入，公开 API 不暴露 SQLite
  类型）。构建与运行不依赖 libsqlite3-dev：沿用 M3 NVML 的 `dlopen` + 内部最小
  C API 声明模式（开发机与 CI 仅有运行时 `libsqlite3.so.0`；SQLite C API 自
  3.x 起稳定）；库路径只接受管理员配置或测试注入，不接受用户输入（威胁模型
  基线 20 同款纪律）。
- 持久化实现决策冻结：总计划第 6 节"SQLite 为唯一 `StateStore` 实现，内存实现
  仅测试用"——本里程碑落为
  [DEC-009](../decisions/DEC-009-sqlite-state-store.md)（含 schema 与 argv/env
  编码格式的兼容性承诺）。
- 孤儿进程兜底：`STARTING` 且无执行记录的 Job 在崩溃窗口内可能已启动进程但未
  落盘身份，恢复按 `RULE-06` 转 `LOST` 并释放 lease；残留进程由 M3 外部占用检测
  将对应 GPU 标记 `EXTERNAL_BUSY` 兜底（不接管、不终止，基线 10），不会导致
  双重分配。

## 工作项

- [ ] `M4-01` 扩展 `StateStore` 契约：`StoredJob` 执行记录（进程身份、起止时间、
  退出状态、失败原因、日志路径）与结构校验规则、稳定错误码；mutation 验证核心
  提取到 Core 共享函数，`InMemoryStateStore` 改为复用并保持既有语义与测试兼容。
- [ ] `M4-02` 实现 `SqliteStateStore`：`dlopen` 绑定与显式 `open()`（含符号链接
  拒绝、库缺失/符号缺失显式失败）、schema 初始化、`load()`（含行级篡改校验）、
  `apply()` 单事务原子写入；与内存实现同语义（revision 冲突、lease 矩阵、容量、
  条目上限），SQLite 类型不出现在公开头。
- [ ] `M4-03` 实现恢复 Core（`yori/recovery/`）：快照驱动、`/proc` 身份核验、
  采纳（`STARTING`->`RUNNING`）/`LOST` + lease 释放决策、队列重建与显式恢复
  结果（含每 Job 原因）；对快照异常数据显式失败。
- [ ] `M4-04` 实现 `StoreTaskRunner`（`EXEC-08`）：单在飞 mutation 的
  `submit_cancellable` 载载，`BUSY`/`NOT_ACCEPTING`/取消/异常/shutdown 显式结果，
  handle 与 future 全程保留消费。
- [ ] `M4-05` 交付测试与文档：SQLite 单测（含故障注入）、恢复单测（含 PID reuse
  模拟）、daemon 重启集成闭环（`recovery` 标签，替换占位用例）、`StoreTaskRunner`
  六场景；设计/DEC-009/威胁模型/总计划/本计划同步；五预设与 PR CI 通过。

## 风险与阻塞

- SQLite C API 绑定保真：以内部最小声明（`sqlite3_open_v2`/`prepare_v2`/`bind_*`/
  `step`/`finalize`/`exec`/`close_v2` 等）+ 只消费稳定返回码与标量列；本机与 CI
  均有真实 `libsqlite3.so.0` 可加载，单测直接驱动真实库（无需 stub）。极端版本
  行为差异（busy 语义、损坏文件返回码）以故障注入用例在真实库上验证。
- 契约扩展对既有测试的影响：执行记录默认空值保持既有 `StoredJob` 语义；新校验
  规则（RUNNING/STOPPING 必须携带身份等）需要同步调度器测试的构造路径，逐项
  显式更新而非放宽校验。
- 恢复与孤儿进程/GPU 状态交互：`LOST` 释放 lease 后残留进程的 GPU 由
  `EXTERNAL_BUSY` 兜底；集成测试覆盖"恢复 -> lease 释放 -> 调度可分配"与
  "核验通过 -> lease 保留"两翼。
- 本机无 clang-tidy-18 与 Clang 编译器；TSAN 需 `setarch -R`（既有环境限制），
  以 PR CI 为最终门禁。

## 测试与退出条件

- [ ] SQLite adapter 单测通过（真实库）：open 幂等/库缺失/符号链接拒绝；create/
  update/lease 全错误码与内存实现一致；执行记录字段 roundtrip；revision 冲突与
  单调；容量与条目上限；重开数据库后快照一致（持久性）；故障注入（只读文件
  apply 拒绝且数据不变、外部写锁 busy 显式失败、损坏文件 load 失败、篡改行
  load 显式失败、空文件/新文件初始化）。
- [ ] 恢复单测通过：QUEUED 重新入队；RUNNING/STOPPING 存活采纳且 lease 保留；
  STARTING 存活提升 RUNNING；身份缺失/进程消失/启动 ticks 不符/PGID 不符 ->
  `LOST` + lease 释放（PID reuse 防错误接管）；终态不动；store/queue 失败显式。
- [ ] daemon 重启集成闭环通过（`recovery` 标签）：SQLite 持久化 -> 关闭重开 ->
  存活进程采纳（lease 保留）+ QUEUED 入队 -> 进程退出后再次恢复 -> `LOST` +
  lease 释放 -> `kRecoveryCompleted` 触发 `FifoScheduler` 将 GPU 分配给排队 Job；
  `example.recovery.restart` 占位用例被真实用例替换。
- [ ] `StoreTaskRunner` 六场景通过：正常完成、任务异常（apply 失败/异常）、提交
  拒绝（`BUSY`/Executor 已关闭）、执行中取消、stop join 截止（超时映射）、
  shutdown 先后顺序。
- [ ] `debug`/`release`/`asan`/`ubsan`/`tsan` 预设全部通过；PR CI（GCC 13/
  Clang 18 的 Debug/Release、clang-format 18、clang-tidy 18、sanitizers、依赖
  门禁）全绿。
- [ ] 设计（第 6.2/12/18 节）、DEC-009、威胁模型、总计划（第 1/5/6/11 节）与
  本计划同步更新。

## 验证记录

### 2026-09-09：M4-01～M4-05 实现与本地验证（PR 前）

- 范围：工作树 `feat/m4-persistence-recovery`。交付 `StoredJob` 执行记录扩展
  （`JobExecutionRecord` + `validate_execution` + `kInvalidExecutionRecord`，
  mutation 验证核心提取至 `src/store/mutation_core.cpp` 供两后端共享）、
  `SqliteStateStore`（dlopen 绑定 `libsqlite3.so.0`、schema 1、符号链接拒绝、
  `BEGIN IMMEDIATE` 单事务原子 apply、行级篡改校验的 load）、`JobRecovery`
  （决策表 + 分块 mutation + 队列重建 + `verify_identity_via_proc` 三态核验）、
  `StoreTaskRunner`（EXEC-08 单在飞串行载载）、4 个测试目标
  （`m4.unit.sqlite-state-store`、`m4.unit.job-recovery`、
  `m4.unit.store-task-runner`、`m4.integration.recovery-restart`，后者替换
  M0 的 `example.recovery.restart` 占位并挂 `recovery` 标签）与既有测试的
  执行记录校验适配（`advanced()` 辅助为 RUNNING/STOPPING 补身份 + 新增
  执行记录正负用例）。
- 测试：SQLite 单测直接驱动真实 `libsqlite3.so.0`（open 幂等/无效配置/库缺失/
  符号链接拒绝；持久化 roundtrip 含执行记录全字段；与内存实现一致的
  revision 冲突/重复/容量/lease 矩阵/非法转换/无身份 RUNNING 拒绝；故障注入
  ——目录只读 apply 显式失败且无部分写入、外部写锁 busy、损坏文件、篡改
  state=99、篡改 lease 矩阵、篡改 argv blob）。恢复单测覆盖决策矩阵（采纳/
  STARTING 提升/STOPPING 需重取消/QUEUED 重建/LOST 三因/终态不动）、
  mutation 组装与 lease 释放、重入幂等、store/queue 失败显式。
  `StoreTaskRunner` 六场景（正常完成+BUSY/NOT_ACCEPTING、任务异常=
  抛异常 kFailed 与结构化失败 kCompleted、排队期取消进入 Executor 视图且
  store 未被触碰、Executor 已关闭拒绝、shutdown 顺序与析构兜底不挂起）。
  集成闭环：SQLite 写入 -> 销毁重开（模拟 daemon 重启，训练进程存活）->
  采纳（lease 保留）+ QUEUED 重建 -> `kRecoveryCompleted` 触发调度且
  lease 优先于 FREE 观测（RULE-05）-> 进程退出后再次恢复 -> `LOST` +
  lease 释放 -> 释放 GPU 分配给排队 Job；PID reuse 场景（进程存活但
  start_ticks 不符）-> `LOST` 且进程未被接管/终止。
- 验证：Linux x86_64、内核 `7.0.0-31-generic`、GCC 13.3.0、Executor pin
  `4fd8e6097879`、系统 `libsqlite3.so.0`（3.45.1）。`debug`/`release`/
  `asan`/`ubsan`/`tsan` 五预设全部执行 configure/build/ctest（TSAN 按 CI
  规定以 `setarch -R ctest --preset tsan` 执行），每套 31 个用例为 24 passed、
  7 个环境/后续里程碑占位用例 skipped（GPU、multi-user 两个、IPC、fuzz、
  performance；recovery 占位已被真实用例替换），无失败、无 race 报告。
  clang-format 18.1.8 全量格式检查（含新增文件）通过。
  `cmake --install build/debug --prefix build/m4-install` 后 `tests/consumer`
  仅使用安装产物配置/编译/运行通过（`libyori_sqlite.a` 与
  `sqlite_state_store.hpp`、`job_recovery.hpp` 进入安装集）；
  `public_header_boundary_test` 覆盖新公共头（含 `state_store.hpp` 新增的
  `process` 头依赖），无 SQLite/Executor 类型泄漏。
- 实现期发现并处置的问题：内部 SQLite 声明头首版把 `SQLITE_OPEN_READWRITE`
  误写为 `0x1`（READONLY 的值），导致 open 返回 SQLITE_MISUSE——单测首轮即
  拦截并修正为 `0x2`；这验证了"单测直接驱动真实库"的必要性。release 构建
  的 `-Wnull-dereference` 拦截了测试中对 `find_job(...)->` 的潜在空指针
  解引用，逐处改为捕获指针 + 守卫模式（源代码与测试均无放松）。
- 限制：本机无 clang-tidy-18 与 Clang 编译器，PR CI 尚未触发；`M4-01`～
  `M4-05` 保持未勾选。负责人：Linductor-alkaid；补跑条件：PR CI 的
  GCC 13/Clang 18 Debug/Release 矩阵、clang-format/clang-tidy 18 与
  sanitizers 全绿后勾选。
- 同步：设计（v0.8：第 6.2 节恢复决策表、第 12 节执行记录与 SQLite 适配
  语义、第 18 节目录）、[DEC-009](../decisions/DEC-009-sqlite-state-store.md)、
  威胁模型（基线 8/13 更新、新增基线 21、待完成项 M4 条目）、总计划
  （第 1/5/6/11 节，M4 状态待 CI 后收口）、本计划。未修改 `third_party/`，
  未发现 Executor 能力缺口（`StoreTaskRunner` 完全复用 M1 已验证的
  `submit_cancellable` + `TaskCancelled` 语义）。
