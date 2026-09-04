# M1：核心域契约与进程内调度闭环

> 状态：In Progress
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M0（[工程骨架与基线](m0-engineering-baseline.md)）
> 建议发布点：无
> 更新日期：2026-09-04

## 目标

冻结 Core 公开契约并交付进程内可测调度闭环：JobSpec 与显式状态机、全局队列与
准入、事件驱动 FIFO 调度、GPU lease 记账，以及 `GpuProvider`/`StateStore` 的
Core 接口（伪 GPU 与内存 StateStore 实现）。本里程碑同时把 pinned Executor
接入构建与运行时，作为 M1 起所有并发工作的唯一基础设施。

## 范围与非目标

范围：

- Executor 构建集成（`third_party/executor` 以 `add_subdirectory` 方式接入，
  只依赖其公共 facade），并用编译测试锁住"公共头不暴露第三方类型"的边界。
- `JobSpec`、JobId 与 Job 显式状态机（`QUEUED/STARTING/RUNNING/STOPPING/
  FINISHED/FAILED/CANCELLED/LOST`）公开契约与实现；终态幂等，迟到结果不得使
  终态 Job 复活。
- `GpuProvider` 与 `StateStore` Core 接口冻结；提供伪 GpuProvider（进程内
  可控 GPU 状态）与内存 StateStore（测试用）实现。
- 全局队列与准入：容量上限显式，拒绝转化为明确结果与事件。
- FIFO 调度器与 GPU lease 记账：事件驱动（提交、退出、取消、GPU 状态变化），
  不依赖高频轮询；lease 为调度事实，与 GPU 观测解耦。
- 并发承载按总计划 `EXEC-06`/`EXEC-09` 落地：有限任务 `submit_auto()`、排队
  取消 `submit_cancellable` + StopToken、按语义选用 `MpscChannel`/
  `LatestMailbox`/`DoubleBuffer`/`PhaseGate`。
- 六场景测试（正常完成、任务异常、提交拒绝、执行中取消、超时、shutdown）与
  状态机/终态幂等/队列上限负向测试。

非目标：

- NVML、SQLite、UDS 等真实依赖接入（M3/M4/M5）。
- 训练进程 spawn 与守护（M2）；本里程碑 Job 的"执行"以受控伪结果推进。
- IPC、CLI 命令、日志观察面（M5/M6）。
- 多 GPU Job、优先级/配额（`POST-01`/`POST-02`）。

## 设计与决策依据

- Job 模型与状态机：[设计文档](../design/yori-project-design.md)第 6 节。
- GPU 资源模型与 lease：设计第 7 节；调度流程：设计第 9 节。
- 持久化接口（内存实现仅测试用）：设计第 12 节。
- Executor 集成边界与文档路由：设计第 14 节、[AGENTS.md](../../AGENTS.md)、
  [工程规范](../project/project-standards.md)第 9 节；集成实现前先按
  `third_party/executor/docs/skill/executor-integration/SKILL.md` 的路由表
  加载对应 router 与 capability card。
- 并发承载与关闭顺序：总计划第 4 节 `EXEC-06`、`EXEC-09`。
- 调度策略暂定默认值（全局 FIFO）冻结：总计划第 6 节，最迟冻结里程碑 M1。

## 工作项

- [x] `M1-01` 将 pinned Executor 接入构建（公共 facade、按语义只编译所需
  组件），提供"公共头不包含第三方类型"的编译边界测试；daemon/CLI 的 Executor
  owner 语义在构建层可见。
- [x] `M1-02` 冻结并实现 `JobSpec`、JobId 与 Job 显式状态机公开契约：有界
  推进步骤、终态幂等、迟到结果丢弃可观测。
- [x] `M1-03` 冻结 `GpuProvider` 与 `StateStore` Core 接口，交付伪 GpuProvider
  与内存 StateStore 实现（GPU 逻辑状态 `FREE/ALLOCATED/EXTERNAL_BUSY/
  UNAVAILABLE` 观测事实与 lease 事实分列）。
- [x] `M1-04` 实现服务器级全局队列与准入：容量上限显式、提交拒绝为明确结果
  与事件，队列恢复语义与内存 StateStore 一致。
- [x] `M1-05` 实现事件驱动 FIFO 调度器与 GPU lease 记账（`EXEC-06`：有限
  任务 + 句柄持有 + 取消路径；调度触发事件可注入测试）。
- [ ] `M1-06` 落地状态推进通信（`EXEC-09`）：Job 状态更新、GPU/调度快照、
  启动 `PhaseGate` 按语义选型 `executor::comm` 组件，不自建队列。
- [ ] `M1-07` 交付进程内调度闭环集成测试：六场景 + 状态机转换矩阵 + 终态
  幂等 + 队列拒绝/上限；ASAN/UBSAN/TSAN 通过。
- [x] `M1-08` 冻结调度策略暂定默认值（全局 FIFO）：实现前确认或另立决策
  记录，同步设计与总计划第 6 节。

## 风险与阻塞

- Executor 公开能力与 Yori 语义的匹配需要先按集成 skill 路由阅读能力卡片，
  避免选型错误误登记为能力缺口（工程规范 9.4 第 1 步）；负责人：
  Linductor-alkaid。
- Core 契约冻结后的变更成本高（影响 M2+ 全部消费方），评审需按工程规范第
  14 节顺序执行。

## 测试与退出条件

- [ ] 进程内闭环测试通过：提交 -> 排队 -> 调度 -> lease 建立 -> 伪执行推进
  -> 终态 -> lease 释放 -> 下一轮调度，全程在 Executor 任务视图内可观测。
- [ ] 新增并发路径覆盖六场景（正常完成、任务异常、提交拒绝、执行中取消、
  超时、shutdown）。
- [x] 状态机转换矩阵与终态幂等测试通过；非法转换被拒绝且可观测。
- [x] 队列容量与拒绝路径负向测试通过；无静默丢弃。
- [x] "公共头不暴露第三方类型"编译边界测试进入常规 ctest。
- [ ] `debug`/`release`/`asan`/`ubsan`/`tsan` 预设全部通过；CI（GCC/Clang
  矩阵）全绿。
- [ ] 设计文档（状态机、调度、接口清单）与总计划 `EXEC` 条目同步更新。

## 验证记录

### 2026-09-04：M1-01/M1-02 基础契约、M1-08 FIFO 冻结

- 范围：工作树 `codex/feat-m1-foundation`。pinned Executor 通过
  `add_subdirectory` 接入，关闭其测试、示例和 GPU 后端；新增进程私有
  `yori_runtime` owner、finite task/future/shutdown 生命周期测试和公共头编译
  边界测试。冻结非零 `JobId`、有界 `JobSpec` 与显式状态机契约；新增 64 组合
  转换矩阵、终态幂等、迟到结果和输入边界负向测试。新增并接受
  [DEC-005](../decisions/DEC-005-global-fifo-scheduling.md)，冻结严格全局 FIFO。
- 依据：设计第 7、9、14、16.1 节，`DEC-001`、`DEC-005`，总计划
  `EXEC-01`/`EXEC-06`，pinned Executor integration skill 的 Quick Start 与
  Tasks And Lifecycle capability card。
- 环境：Linux x86_64，内核 `7.0.0-31-generic`，GCC 13.3.0，Executor
  `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`；GPU/CUDA/OpenCL 关闭。
- 通过：`cmake --preset <debug|release|asan|ubsan> -DYORI_FETCH_DEPENDENCIES=OFF &&
  cmake --build --preset <preset> -j2 && ctest --preset <preset>
  --output-on-failure`；每套 13 个用例为 7 passed、6 个既有环境/后续里程碑
  占位用例 skipped。TSAN 依 CI 规定执行 `cmake --preset tsan
  -DYORI_FETCH_DEPENDENCIES=OFF && cmake --build --preset tsan -j2 && setarch -R
  ctest --preset tsan --output-on-failure`，结果相同且无 race 报告。直接运行 TSAN
  会因宿主高熵 ASLR 报 `unexpected memory mapping`，不作为规定门禁。
  `cmake --install build/debug --prefix build/m1-install` 后，仅安装 `yori_core`、
  Yori 公开头和两个程序；安装产物的 `tests/consumer` configure/build/run 通过并
  实际创建 Job。`public_header_boundary_test` 的编译命令只有 Yori 源码与生成头
  include path，无 Executor include path。
- 限制：本机没有 `clang-tidy-18` 和 Clang C++ 编译器，远端 CI 尚未触发；因此
  `M1-01`、`M1-02` 保持未完成。负责人：Linductor-alkaid；补跑条件：在安装
  clang-tidy-18 的环境按 `.github/workflows/ci.yml` 对全部自研 `.cpp` 执行静态
  检查，并由 PR CI 完成 GCC 13/Clang 18 的 Debug/Release 矩阵；全绿后勾选。
- 同步：已更新设计第 6/16.1 节、安全威胁模型、安装 consumer、总计划当前状态/
  决策表/文档地图、M1 状态与验证记录；未修改 pinned Executor，也未登记能力
  缺口。

### 2026-09-04：M1-03 GPU 与状态存储契约

- 范围：基于 `da97f0d` 的工作树。冻结 `GpuProvider::observe()`、有界 GPU
  observation snapshot、Core `GpuLease`/逻辑状态投影，以及 `StateStore::load()`/
  `apply()` revision 一致性与原子 mutation 契约；交付测试私有的
  `FakeGpuProvider` 和容量显式的 `InMemoryStateStore`，均为单 owner 且不隐藏
  线程、锁、队列或重试。
- 不变量：Provider 不产生 `ALLOCATED` 观测；有 lease 时由 Core 投影为
  `ALLOCATED`。StateStore mutation 最多 64 条，JobSpec 创建后不可变，Job
  revision 恰加一；`STARTING/RUNNING/STOPPING` Job 恰有一个 lease，`QUEUED`
  与终态无 lease，GPU 与 Job 两侧一对一。失败与 revision 冲突全部显式返回，
  内存实现不留下部分写入。
- 验证：Linux x86_64、GCC 13.3.0、Executor pin `4fd8e6097879`。最终工作树的
  `debug`/`release`/`asan`/`ubsan` 均执行 configure/build/ctest；TSAN 按 CI
  方式以 `setarch -R ctest --preset tsan` 执行。每套 15 个用例为 9 passed、
  6 个既有环境/后续里程碑占位 skipped；新增 Provider 与 StateStore 用例覆盖
  UUID/index/telemetry、容量、后端失败、revision 冲突、非法转换、JobSpec
  篡改、lease 一致性和事务回滚。Release 首轮因测试 helper 裸指针解引用触发
  `-Wnull-dereference -Werror`，改为 fail-fast 引用 helper 后全量通过。
- 安装：`cmake --install build/debug --prefix build/m1-install` 后，产品安装清单
  不含两个测试实现；`tests/consumer` 仅使用安装的 Yori 配置/头/库，成功创建
  Job、计算 GPU 逻辑状态并实例化 StateMutation。
- 限制：本机仍无 clang-tidy-18/Clang，PR CI 尚未触发，故 `M1-03` 保持未
  勾选。负责人：Linductor-alkaid；补跑条件与上一记录相同：clang-tidy-18
  检查及 GCC 13/Clang 18 Debug/Release PR CI 全绿后勾选。
- 同步：已更新设计第 7、12 节、安全威胁模型、总计划当前状态、公开头、安装
  consumer、测试与本计划；无 Executor 能力缺口，未修改 `third_party/`。

### 2026-09-04：M1-04 全局队列与准入

- 范围：基于 `637fda3` 的工作树。交付服务器级单 owner `GlobalJobQueue`、有界
  配置、稳定 `(submit_time, JobId)` 排序、准入/移除/恢复结果以及可由上层发布的
  结构化 `QueueEvent`。队列只保存排序键，不复制 JobSpec，也不建立用户私有队列。
- 不变量：默认容量 1024、配置硬上限 4096；无效 Job、重复 Job 与容量耗尽均显式
  拒绝且不改变队列。StateStore 是持久化权威，`restore()` 只恢复
  `QUEUED/revision=0` Job，活动态和终态不重新入队；恢复失败原子保留旧队列。
- 验证：Linux x86_64、GCC 13.3.0、Executor pin `4fd8e6097879`。最终工作树的
  `debug`/`release`/`asan`/`ubsan` 均执行 configure/build/ctest；TSAN 按 CI
  方式以 `setarch -R ctest --preset tsan` 执行。每套 16 个用例为 10 passed、
  6 个既有环境/后续里程碑占位 skipped。新增队列用例覆盖零/超上限配置、跨用户
  稳定排序、同时间 JobId 决胜、重复与容量拒绝事件、无效 ID/Spec/状态/revision、
  移除、StateStore 恢复、活动态/终态过滤和失败回滚。
- 安装：`cmake --install build/debug --prefix build/m1-install` 后，独立 consumer
  仅使用安装的 Yori 配置/头/库，成功创建空队列；公共头编译命令无 Executor
  include path，产品安装清单不含测试支持实现。
- 限制：本机仍无 clang-tidy-18/Clang，PR CI 尚未触发，故 `M1-04` 保持未
  勾选。负责人：Linductor-alkaid；补跑条件与既有记录相同：clang-tidy-18
  检查及 GCC 13/Clang 18 Debug/Release PR CI 全绿后勾选。
- 同步：已更新设计第 9 节（与 16.1 节既有决策一致）、安全威胁模型、总计划
  设计版本、公开头、安装 consumer、测试与本计划；本工作项不新增并发路径，无
  Executor 能力缺口，未修改 `third_party/`。

### 2026-09-04：M1-05 事件驱动 FIFO 调度与 lease

- 范围：基于 `581cbc2` 的工作树。交付公开的单 owner `FifoScheduler` 与进程私有
  `SchedulerTaskRunner`。每个注入触发最多调度一个队首 Job；Core 只依赖
  `GlobalJobQueue`/`StateStore`/GPU observation，Executor 类型不进入公开头。
- 不变量：严格队首、不 backfill；只选观测为 `FREE` 且无现有 lease 的 GPU，多
  候选按 physical index 确定性选择但持久化 UUID。`QUEUED -> STARTING` 与 lease
  在一个 StateStore mutation 中提交；显式写失败会把暂时移除的队首恢复到原排序
  位置，不静默重试。队列/存储分歧、无效 GPU 快照和后端失败均结构化返回。
- Executor 路径：依 pinned integration skill 的 Scenarios router 与 Tasks And
  Lifecycle capability card，使用 `submit_cancellable()`、保留 handle/future、
  `request_task_cancel()` 协作取消。Runner 同时只接纳一个任务；未消费、停止生产、
  Executor 总量准入拒绝、取消与任务异常均显式可观察，析构仅作取消并消费兜底，
  正常关闭仍要求外部 owner 先停生产者并显式消费结果。
- 验证：Linux x86_64、GCC 13.3.0、Executor pin `4fd8e6097879`。最终工作树的
  `debug`/`release`/`asan`/`ubsan` 均执行 configure/build/ctest；TSAN 按 CI
  方式以 `setarch -R ctest --preset tsan` 执行。每套 18 个用例为 12 passed、
  6 个既有环境/后续里程碑占位 skipped。新增 Core/Runtime 用例覆盖队列空闲、
  队首阻塞、稳定 FIFO、确定性 GPU 选择、已有 lease 排除、原子状态/lease 提交、
  StateStore load/write 失败、队列回滚、QUEUED 数量/全局最早键分歧、任务 BUSY、
  排队取消、future 消费及 Executor `max_in_flight_tasks` 拒绝。Debug 首轮因新
  runtime 构造参数遮蔽成员触发
  `-Wshadow -Werror`，修正命名后 Debug/Release 严格告警构建通过。
- 安装：`cmake --install build/debug --prefix build/m1-install` 后，独立 consumer
  configure/build/run 通过；安装的 scheduler 公共头及其编译边界无 Executor
  include path，进程私有 Runner 与测试支持实现不进入安装产物。
- 限制：本机仍无 clang-tidy-18/Clang，PR CI 尚未触发，故 `M1-05` 保持未
  勾选。负责人：Linductor-alkaid；补跑条件与既有记录相同：clang-tidy-18
  检查及 GCC 13/Clang 18 Debug/Release PR CI 全绿后勾选。
- 同步：已更新设计第 9 节、安全威胁模型、总计划设计版本、公开/私有边界、测试
  与本计划；未修改 `third_party/`，未发现 Executor 能力缺口。

### 2026-09-04：M1-01～M1-05 PR CI 收尾

- 范围：PR [#2](https://github.com/Linductor-alkaid/yori/pull/2)，提交
  `da97f0d`、`637fda3`、`581cbc2`、`3138f81` 及 CI 修复 `444c5ef`；目标
  `master` 为 `394e5f6`，创建 PR 前已确认与 `origin/master` 同步且无冲突。
- 首轮 CI run
  [33850665778](https://github.com/Linductor-alkaid/yori/actions/runs/33850665778)
  的 8 个 job 中 7 个通过，`clang-tidy` 暴露 5 个 warnings-as-errors。修复
  `StopToken` 不必要值复制、无效 `std::move`、测试入口异常逃逸，并为故意构造
  非法枚举值的 adapter 边界测试增加精确单行分析器豁免；未改变产品行为。
- 最终 CI run
  [33851194487](https://github.com/Linductor-alkaid/yori/actions/runs/33851194487)
  8/8 全绿：clang-format 18、clang-tidy 18、GCC 13/Clang 18 的
  Debug/Release configure/build/test/install/consumer、ASAN/UBSAN/TSAN，以及依赖
  pin 篡改/恢复门禁全部通过。每套 ctest 仍为 12 passed、6 个后续里程碑或外部
  环境占位用例显式 skipped；这些 skip 不构成对应能力已验证。
- 结论：`M1-01`～`M1-05` 的实现、文档与适用门禁证据完整，工作项勾选完成。
  `M1-06`、`M1-07` 未实现且保持未勾选；按 2026-09-04 的范围决定，本次不继续
  推进后续 M1 工作，M1 里程碑整体仍为 `In Progress`。
