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

- [ ] `M1-01` 将 pinned Executor 接入构建（公共 facade、按语义只编译所需
  组件），提供"公共头不包含第三方类型"的编译边界测试；daemon/CLI 的 Executor
  owner 语义在构建层可见。
- [ ] `M1-02` 冻结并实现 `JobSpec`、JobId 与 Job 显式状态机公开契约：有界
  推进步骤、终态幂等、迟到结果丢弃可观测。
- [ ] `M1-03` 冻结 `GpuProvider` 与 `StateStore` Core 接口，交付伪 GpuProvider
  与内存 StateStore 实现（GPU 逻辑状态 `FREE/ALLOCATED/EXTERNAL_BUSY/
  UNAVAILABLE` 观测事实与 lease 事实分列）。
- [ ] `M1-04` 实现服务器级全局队列与准入：容量上限显式、提交拒绝为明确结果
  与事件，队列恢复语义与内存 StateStore 一致。
- [ ] `M1-05` 实现事件驱动 FIFO 调度器与 GPU lease 记账（`EXEC-06`：有限
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
- [ ] 队列容量与拒绝路径负向测试通过；无静默丢弃。
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
