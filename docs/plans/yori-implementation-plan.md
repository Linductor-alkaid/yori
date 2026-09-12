# Yori 实施总计划

> 状态：Active
> 版本：1.14
> 更新日期：2026-09-12
> 负责人：Linductor-alkaid
> 设计依据：[Yori 项目设计文档](../design/yori-project-design.md)（v0.5）
> 治理依据：[AGENTS.md](../../AGENTS.md)、[项目管理与工程规范](../project/project-standards.md)

本文是 Yori 交付范围与进度的唯一入口（工程规范第 3.1 节）。里程碑细节与逐轮实施
证据在独立里程碑文档中维护，本文保持概览。

## 1. 当前整体状态

- M0（工程骨架与基线）已完成：CMake 五预设、Executor pin 校验、格式/静态检查、
  Linux GCC/Clang CI 基线与测试标签体系全部落地，CI 全绿（证据见
  [M0 验证记录](m0-engineering-baseline.md)；PR
  [#1](https://github.com/Linductor-alkaid/yori/pull/1) 已合并）。Executor 按
  [DEC-001](../decisions/DEC-001-executor-pinning.md) 以 git submodule +
  `dependencies.lock.json` 锁定（`v0.4.0-82-g4fd8e60`，MIT）。M1-01～M1-05
  已完成：Executor 进程私有 owner、Core 契约、全局队列与事件驱动 FIFO 调度
  已落地，PR [#2](https://github.com/Linductor-alkaid/yori/pull/2) 的最终 CI
  [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/33851194487)；M1-06、
  M1-07 尚未实现，本次收尾后不继续推进。
- M2（进程守护与启动适配）已完成：`LaunchProfile`/`LaunchAdapter`（DEC-006 环境
  白名单）、`ProcessSupervisor` Linux 引擎与取消升级（DEC-007 宽限默认 10s）、
  `LogSink` 落盘/轮转/drop 标记与 Executor 承载（`ProcessExitMonitor`/`LogPump`/
  `GraceEscalation`）全部落地；PR [#3](https://github.com/Linductor-alkaid/yori/pull/3)
  最终 CI [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/34151784269)
  （证据见 [M2 验证记录](m2-process-supervision.md)）。
- M3（NVML 真实 GPU 集成）已完成：`NvmlGpuProvider`（`dlopen` 绑定、发现、
  UUID 身份、遥测、外部占用检测 `EXTERNAL_BUSY`、显式错误映射）与 `GpuManager`
  （`EXEC-05` 周期采样、`EXEC-09` GPU 快照 `DoubleBuffer`、观测状态迁移事件）
  落地，stub NVML 与进程内集成闭环覆盖无 GPU 环境；PR [#4](https://github.com/Linductor-alkaid/yori/pull/4)
  最终 CI [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/34251754105)
  （证据见 [M3 验证记录](m3-nvml-gpu-integration.md)）。
- M4（持久化与恢复）已完成：`StoredJob` 执行记录扩展（进程身份、起止时间、
  退出状态、失败原因、日志路径）、`SqliteStateStore`（DEC-009：dlopen 绑定、
  schema 1、单事务原子 apply、篡改显式失败）、`JobRecovery` 恢复 Core（身份
  核验、`LOST` 语义、队列重建、PID reuse 防护）与 `StoreTaskRunner`
  （`EXEC-08` 载载）落地；PR [#5](https://github.com/Linductor-alkaid/yori/pull/5)
  最终 CI [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/34273077478)
  （证据见 [M4 验证记录](m4-persistence-recovery.md)）。
- M5（IPC 与 CLI）已完成：IPC 协议 v1 Core 契约（帧边界与稳定错误码）、
  UDS 传输（`SO_PEERCRED`、EXEC-02 blocking worker、
  [DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md) 端点治理）、daemon 侧
  请求服务（owner/admin 授权与脱敏、终态幂等）、M5 子集 daemon 总装与
  `yori` 六命令、IPC fuzz 起步落地；PR [#7](https://github.com/Linductor-alkaid/yori/pull/7)
  最终 CI [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/34374222682)
  （证据见 [M5 验证记录](m5-ipc-cli.md)）。
- M6（观察面）已完成：`logs -f` 流式跟随（协议 v1 新 kind `LOGS_FOLLOW`
  与 `LOG_DATA`/`LOG_GAP`/`LOG_BACKPRESSURE`/`LOG_EOF` 帧族、每 Job
  `Topic<LogChunk>` 与内存回看窗口、`LogFollowService` 会话承载
  （EXEC-03/04）、慢客户端 BACKPRESSURE 显式断开）、`yori tensorboard`
  （DEC-003 CLI 拉起 + daemon 只读解析）、日志与跟随上限冻结落地；PR
  [#8](https://github.com/Linductor-alkaid/yori/pull/8) 最终 CI
  [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/34400184509)
  （证据见 [M6 验证记录](m6-observability.md)）。
- M7（打包与 MVP 端到端验收）已完成：守护总装收口（`JobManager` 调度
  触发联动与进程守护、恢复采纳与 STOPPING 重取消、`SerialStateStore`
  所有权串行化、`StoreTaskRunner` 写路径接入、启动 `PhaseGate`、EXEC-10
  完整停止序与 RULE-10 abandon）、systemd unit 与安装打包、MVP §19 验收
  矩阵（CI 项附证据）全部落地；PR [#11](https://github.com/Linductor-alkaid/yori/pull/11)
  最终 CI [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/34446034703)
  （证据见 [M7 验证记录](m7-packaging-acceptance.md)）。
- M1 收口（2026-09-10）：`M1-06`/`M1-07` 按 2026-09-04 范围决定不再独立
  交付；其中 EXEC-09 启动 `PhaseGate` 与六场景闭环集成测试已由 M7 守护总装
  承接（`m7.unit.job-manager`），触发合并未建独立 comm 层（由 JobManager
  命令通道承载）。M1 里程碑随 M7 关闭（见 M1 文档 2026-09-10 收口记录）。
- MVP 后第一批增强立项（2026-09-12）：依据真机使用反馈
  （[#16](https://github.com/Linductor-alkaid/yori/issues/16) 执行上下文、
  [#10](https://github.com/Linductor-alkaid/yori/issues/10) GPU placement）
  立项 M8（提交时执行上下文捕获与恢复，
  [DEC-011](../decisions/DEC-011-execution-context-capture.md)）与 M9（GPU
  placement 亲和调度，[DEC-012](../decisions/DEC-012-gpu-placement-policy.md)）。
  M8 已于 2026-09-12 启动：负责人确认依计划推进，DEC-011 随启动冻结为
  Accepted，里程碑文档
  [m8-execution-context.md](m8-execution-context.md) 已创建；DEC-012 维持
  Proposed，M9 启动时冻结。`v0.1.0` 发布（2026-09-10）：LICENSE
  选定 MIT（第 6 节冻结）、仓库级 `README.md` 与 `CHANGELOG.md`、deb 运行时
  包（`packaging/deb/build-deb.sh`，安装即启用服务）与 Release 流水线
  （`.github/workflows/release.yml`：`v*` tag 构建 deb 并发布）。真机验收
  补跑条件见 M7 验收矩阵。
- M8 已合并（2026-09-12 负责人授权）：PR
  [#18](https://github.com/Linductor-alkaid/yori/pull/18)（merge commit
  `2199546`，12 个实现提交；最终 CI 9/9 全绿，run 34691974501/34698759784），
  工作分支已清理，合并后 master 本地 44/44 复验。随发版 PR 同步收口状态并
  交付 `v0.2.0`。M8 期间修复 M7 遗留丢失唤醒（`yori logs -f` 偶发悬挂：
  LogPump 完成事件不唤醒 JobManager，EOF 汇合永不触发）。
- 当前里程碑：M9（GPU placement 亲和调度，In Progress，
  [m9-gpu-placement.md](m9-gpu-placement.md)，分支
  `feat/m9-gpu-placement`）；DEC-012 已于 2026-09-13 随 M9 启动冻结为
  Accepted（部分修订 DEC-005 队首条款）。
- MVP 端到端验收以设计文档第 19 节判据为准，由 M7 执行并记录证据（见第 10 节）。
- 里程碑文档在各自启动时创建（工程规范第 2 节）；当前实体文件：M0-M7
  （M8/M9 文件在各自启动时创建）。

## 2. 交付边界（SCOPE）

MVP 交付内容（依据设计第 2.1、16.1 节）：

| 编号 | 交付内容 |
| --- | --- |
| `SCOPE-01` | 单服务器、多 NVIDIA GPU；NVML 经 `GpuProvider` 适配接入 |
| `SCOPE-02` | 多 Linux 用户提交；训练以提交用户 UID/GID 身份运行 |
| `SCOPE-03` | 服务器级唯一队列；MVP 全局 FIFO 调度 |
| `SCOPE-04` | GPU lease 与外部占用检测（标记 `EXTERNAL_BUSY`，不接管、不终止外部进程） |
| `SCOPE-05` | systemd 管理的 `yorid` 与 Unix Domain Socket IPC（`SO_PEERCRED` 鉴权）；`yori` CLI：`submit`/`ps`/`queue`/`gpu`/`cancel` |
| `SCOPE-06` | SQLite 持久化（`StateStore`）与 daemon 重启恢复（含 `LOST` 语义） |
| `SCOPE-07` | 进程守护：spawn、独立进程组、取消（`SIGTERM` -> grace -> `SIGKILL`）、退出回收与 lease 释放 |
| `SCOPE-08` | Job 日志：捕获、落盘、轮转；`yori logs` 快照与 `-f` 流式跟随（逻辑 offset 续传、`GAP`/`EOF`/`BACKPRESSURE` 帧）（[DEC-002](../decisions/DEC-002-mvp-observability.md)） |
| `SCOPE-09` | `yori tensorboard` 观察入口（CLI 侧拉起，[DEC-003](../decisions/DEC-003-tensorboard-cli-hosting.md)） |

MVP 明确不交付（依据设计第 2.2 节）：

| 编号 | 排除内容 |
| --- | --- |
| `SCOPE-10` | Kubernetes、Slurm 等通用集群编排的完整能力 |
| `SCOPE-11` | 自动识别并接管任意现存训练进程为 Yori Job |
| `SCOPE-12` | 解析或修改用户训练源码来替换 GPU 编号 |
| `SCOPE-13` | 跨服务器全局调度 |
| `SCOPE-14` | 以 GPU utilization == 0 作为唯一空闲判据 |
| `SCOPE-15` | 要求训练程序链接 Yori、Executor 或 Heyaki |

MVP 后已立项增强（2026-09-12，依据 issue #16/#10）：

| 编号 | 增强内容 | 里程碑 | 决策依据 |
| --- | --- | --- | --- |
| `SCOPE-16` | 提交时执行上下文捕获与恢复：环境捕获白名单 + `--env`/`--inherit-env`、executable 提交时解析、四层环境合并（含 `LD_LIBRARY_PATH` 保留键修订）、`yori inspect` 与 env 脱敏、schema v2 | M8 | [DEC-011](../decisions/DEC-011-execution-context-capture.md)（Accepted） |
| `SCOPE-17` | GPU placement 亲和调度：`ANY`/`REQUIRED`、daemon 侧 index→UUID 解析、候选集过滤、FIFO 有界跳过（修订 DEC-005 队首条款）、`wait_reason` 展示、schema v3 | M9 | [DEC-012](../decisions/DEC-012-gpu-placement-policy.md)（Proposed） |

## 3. 不可破坏架构约束（RULE）

| 编号 | 约束 | 依据 |
| --- | --- | --- |
| `RULE-01` | 一台服务器一个 authoritative scheduler；队列、GPU lease 与 Job 状态仅由 `yorid` 修改；CLI 一律无状态 | 设计 §3、AGENTS.md |
| `RULE-02` | Core 与平台解耦：NVML、SQLite、UDS、systemd 只能经 `GpuProvider`/`StateStore`/`LaunchAdapter`/`IpcTransport` 接口接入；Adapter 依赖 Core 接口，禁止反向；公开 API 不暴露平台与第三方类型 | 设计 §4、AGENTS.md 工程约束 |
| `RULE-03` | 一切并发任务与生命周期由 pinned Executor 管理；禁用 `std::thread`/`std::jthread`/`std::async`、自建线程池、私有定时器与 fire-and-forget | AGENTS.md Executor 条款 |
| `RULE-04` | Job 为显式状态机（`QUEUED/STARTING/RUNNING/STOPPING/FINISHED/FAILED/CANCELLED/LOST`）；终态幂等；迟到结果不得使终态 Job 复活 | AGENTS.md Runtime 与状态模型 |
| `RULE-05` | Yori lease 是调度事实，NVML 是资源观测；不得以瞬时 utilization 释放 GPU | 设计 §7 |
| `RULE-06` | 恢复必须核验进程身份（PID/PGID/启动时间），无法确认进入 `LOST`；不得因 daemon 重启无条件重启 RUNNING Job | 设计 §6.2、AGENTS.md |
| `RULE-07` | 调度器事件驱动，不依赖高频轮询 | 设计 §9 |
| `RULE-08` | 一切有界：队列、IPC 流、日志缓冲、跟随会话数、落盘磁盘预算均有显式上限与溢出策略；拒绝与溢出必须转化为明确结果与事件 | 设计 §11.4、AGENTS.md 工程约束 |
| `RULE-09` | 安全底线：绝不以 root 执行用户命令；身份只信 `SO_PEERCRED`；`exec` 前完成 supplementary groups/`setgid`/`setuid`；授权在 daemon 侧判定 | 设计 §17、[DEC-004](../decisions/DEC-004-privileged-daemon-demotion.md) |
| `RULE-10` | daemon 正常关闭不终止运行中训练进程；关闭与守护语义显式区分 | 设计 §10、AGENTS.md |
| `RULE-11` | 测试底线：新增并发路径覆盖正常完成、任务异常、提交拒绝、执行中取消、超时、shutdown 六场景；Yori 特有场景（重启恢复、PID reuse、外部占用、多用户隔离、慢客户端背压）必有测试；ASAN/UBSAN 常规、daemon 状态与关闭路径 TSAN、恢复故障注入、IPC parser fuzz | AGENTS.md 工程约束 |
| `RULE-12` | 不得为绕过 Executor 能力边界引入第二套并发设施；确认缺口必须登记[反馈台账](../executor_feedback/ledger.md)并被代码引用 | AGENTS.md、工程规范 §9.4 |

## 4. Executor 并发边界（EXEC）

`EXEC-01`（进程内唯一 owner）：`yorid` 的 Executor 由 daemon 主生命周期初始化与
关闭；`yori` CLI 若使用 Executor，owner 为 CLI 进程自身。任何组件不得隐藏全局
Executor 生命周期，依赖经构造参数或显式 context 传递。

| 编号 | 工作类别 | Executor 承载 | 句柄持有者 | 取消 / 解除阻塞 | 关闭阶段 |
| --- | --- | --- | --- | --- | --- |
| `EXEC-02` | IPC 连接接受与请求读取 | blocking worker（`start_worker` + 有界工作通道） | IpcServer | wakeup 解除 accept/read 阻塞 | ① 停止新连接与请求生产者 |
| `EXEC-03` | `logs -f` 会话与日志管道读取（log pump） | blocking worker | LogStreamer | wakeup + 关闭管道读端；订阅者断开不影响落盘主路径 | ② 断开跟随会话（`EOF`/错误帧） |
| `EXEC-04` | 日志块订阅分发 | `executor::comm::Topic<LogChunk>`（每 Job 一个），每订阅者有界队列 | LogStreamer | 队列满即断开订阅并回 `BACKPRESSURE`，不静默丢弃 | ② |
| `EXEC-05` | NVML 遥测采样与外部占用扫描 | `submit_periodic` + `TimerHandle`（允许抖动） | GpuManager | 取消 `TimerHandle` | ③ |
| `EXEC-06` | Job 状态推进与调度触发 | M7 已落地：触发经 JobManager blocking worker 的命令通道汇聚（submit/cancel 命令、退出监视与 GPU 事件监听回调唤醒），调度单元仍由 `SchedulerTaskRunner` 以 `submit_cancellable()` 承载（worker 触发后立即消费结果）；队列变更只发生在该 worker 与启动前恢复路径 | JobManager / Scheduler | 触发事件：新提交、Job 退出、取消、GPU 状态变化、恢复完成、管理员操作 | ④ 停止调度生产者 |
| `EXEC-07` | 进程退出监视与回收（waitpid） | blocking worker 或可取消有限任务 | ProcessSupervisor | wakeup；Job 取消为进程组 `SIGTERM` -> grace -> `SIGKILL`（Yori 外部进程语义，不与 Executor 任务取消混同） | ⑤ |
| `EXEC-08` | SQLite 写入与恢复读取 | M7 已接入：全部运行期 mutation 经 JobManager worker 组合并由 `StoreTaskRunner` 执行（单在飞、逐条 FIFO、revision 组合）；IPC 读路径与写的并发经 `SerialStateStore` 所有权互斥串行化；恢复读取为 daemon 启动序列的有界同步单元 | StoreTaskRunner / JobManager worker / daemon 主生命周期 | 排队期取消 + `TaskCancelled` 显式结果；admission 拒绝显式化 | ⑦ 等待终态落盘完成（JobManager worker 串行路径内闭合） |
| `EXEC-09` | 状态快照、更新与启动协调 | M7 已落地：GPU 快照 `DoubleBuffer`（M3）保持；启动阶段 `PhaseGate`（恢复 -> GPU 观察 -> 调度开启）由 Daemon 持有、JobManager 消费（未达调度阶段不消费调度事件）；Job 状态更新的独立 comm 通道未建（M1-06 范围决定，由 JobManager 命令通道承载） | 对应组件 | 快照无取消语义，随组件回收 | ⑥ |

`EXEC-10`（daemon 关闭顺序，依据 AGENTS.md 第 7 条与设计 §11.7）：
① 停止 IPC 生产者与新连接 -> ② 断开 `logs -f` 跟随会话 -> ③ 停止 NVML 周期任务 ->
④ 停止调度生产者（不新调度、不终止训练进程，`RULE-10`）-> ⑤ 取消活动有限任务与
进程回收等待者 -> ⑥ 回收 blocking worker 与实时路径 -> ⑦ 等待需完成的有限任务
（终态与 lease 状态落盘）-> ⑧ 由 daemon 主线程（非 worker 线程）执行
`shutdown(true)`。

## 5. 里程碑索引

| 里程碑 | 名称 | 前置 | 能力增量 | 建议发布点 | 状态 |
| --- | --- | --- | --- | --- | --- |
| M0 | 工程骨架与基线 | 无 | CMake/CI/测试标签/规范工具/文档框架、Executor 锁定校验 | 无（内部基线） | Completed |
| M1 | 核心域契约与进程内调度闭环 | M0 | JobSpec、状态机、全局队列、FIFO 调度、GPU lease 记账；内存 StateStore 与伪 GpuProvider 下的进程内可测闭环 | 无 | Completed（2026-09-10 随 M7 收口，M1-06/07 见第 1 节） |
| M2 | 进程守护与启动适配 | M1 | ProcessSupervisor（spawn、进程组、取消、退出回收）、LaunchProfile、`exec` 前降权、日志捕获与落盘 | 无 | Completed |
| M3 | NVML 真实 GPU 集成 | M2 | `GpuProvider` NVML 适配：发现、UUID 身份、遥测、外部占用检测（`EXTERNAL_BUSY`）；`GpuManager` 周期采样（`EXEC-05`/`EXEC-09` GPU 快照） | 无 | Completed |
| M4 | 持久化与恢复 | M2 | SQLite StateStore、daemon 重启恢复、PID reuse 核验、`LOST` 语义 | 无 | Completed |
| M5 | IPC 与 CLI | M3、M4 | UDS 传输、`SO_PEERCRED` 鉴权、请求/响应协议与 owner/admin 授权、`submit`/`ps`/`queue`/`gpu`/`cancel`/`logs` 快照、IPC fuzz 起步 | 无 | Completed |
| M6 | 观察面 | M5 | `logs -f` 流式帧（offset 续传、`GAP`/`EOF`/`BACKPRESSURE`）、日志轮转、`yori tensorboard` | 无 | Completed |
| M7 | 打包与 MVP 端到端验收 | M6 | systemd unit、安装打包、设计 §19 判据逐项验收；前置：守护总装收口 | `v0.1.0`（MVP；tag/发布需负责人授权） | Completed |
| M8 | 提交时执行上下文 | M7 | 环境捕获（白名单/`--env`/`--inherit-env`）、executable 提交时解析、环境合并 v2 与保留键修订、`yori inspect`（owner/admin + 脱敏）、IPC 协议 v2、schema v2 迁移；Conda/venv 语义一致性验证 | `v0.2.0` | Completed（2026-09-12，PR [#18](https://github.com/Linductor-alkaid/yori/pull/18)，`v0.2.0` 发布） |
| M9 | GPU placement 亲和调度 | M7（与 M8 无代码依赖，建议随后执行以共享迁移框架） | `GpuPlacement` Core 类型与校验、daemon 侧 `--gpu` 解析、Scheduler 候选集过滤与 FIFO 有界跳过、`wait_reason`、schema v3 与恢复一致性；issue #10 12 场景矩阵 | `v0.3.0` | In Progress（2026-09-13 启动，DEC-012 Accepted） |

- M3 与 M4 在 M2 完成后可并行推进。
- 里程碑文件命名 `m<N>-<scope>.md`，在该里程碑启动时创建；当前实体文件：
  [M0 工程骨架与基线](m0-engineering-baseline.md)、
  [M1 核心域契约与进程内调度闭环](m1-core-contracts.md)、
  [M2 进程守护与启动适配](m2-process-supervision.md)、
  [M3 NVML 真实 GPU 集成](m3-nvml-gpu-integration.md)、
  [M4 持久化与恢复](m4-persistence-recovery.md)、
  [M5 IPC 与 CLI](m5-ipc-cli.md)、
  [M6 观察面](m6-observability.md)、
  [M7 打包与 MVP 端到端验收](m7-packaging-acceptance.md)、
  [M8 提交时执行上下文](m8-execution-context.md)。

## 6. 暂定默认值与未决问题

以下选择为推进而暂定，或为已识别的设计缺口；冻结前变更不受罚，冻结时按工程规范
第 6.2 节落为决策记录。已冻结的决策见 `docs/decisions/`（DEC-001 ~ DEC-010）。

| 项目 | 暂定默认值 / 未决问题 | 负责人 | 最迟冻结 | 冻结动作 |
| --- | --- | --- | --- | --- |
| 调度策略 | 已冻结：严格全局 FIFO，无优先级、配额或 backfill（[DEC-005](../decisions/DEC-005-global-fifo-scheduling.md)） | Linductor-alkaid | M1 | 已于 2026-09-04 冻结；变更需新决策记录替代 DEC-005 |
| 取消 grace period | 已冻结：默认 10 秒、下限 100ms、上限 10 分钟；宽限为软期限，升级前退出则空操作（[DEC-007](../decisions/DEC-007-cancel-grace-period.md)） | Linductor-alkaid | M2 | 已于 2026-09-08 冻结；变更需新决策记录替代 DEC-007 |
| 环境变量白名单初版 | 已冻结：身份块 + daemon 白名单 + GPU 映射块三层合并，保留键出现即拒绝（[DEC-006](../decisions/DEC-006-launch-environment-policy.md)） | Linductor-alkaid | M2 | 已于 2026-09-08 冻结；扩展白名单属配置级变更 |
| daemon 重启后的日志续捕 | 守护语义已冻结：子进程 exec 前 `SIGPIPE=SIG_IGN`，重启窗口输出丢失、文件原位续写（[DEC-008](../decisions/DEC-008-daemon-restart-log-continuity.md)）；观察面 offset/续捕细节最迟 M6 复核 | Linductor-alkaid | M2（已冻结）、M6（复核） | 设计 §10.2/§11.2 已同步 |
| 持久化实现 | 已冻结：SQLite 为唯一 `StateStore` 实现，内存实现仅测试用；dlopen 绑定与 schema 1 见 [DEC-009](../decisions/DEC-009-sqlite-state-store.md) | Linductor-alkaid | M4 | 已于 2026-09-09 冻结；格式变更需新决策记录 |
| IPC 端点 | 已冻结：`/run/yori/yori.sock`、`root:yori 0660`、`yori` 系统组连接准入、admin 组双路径判定（[DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md)） | Linductor-alkaid | M5 | 已于 2026-09-09 冻结；变更需新决策记录替代 DEC-010 |
| 日志与跟随上限默认值 | 已冻结：落盘单文件 256 MiB / 保留 1 个历史文件（M2 `LogSink`）；跟随会话每 Job 8 / 全局 64、回看窗口 8 MiB/流（64 KiB ~ 64 MiB）、订阅队列 64 chunk、会话写出缓冲 2 MiB、单帧写截止 2 s（M6 `LogStreamer`/`LogFollowService`，全部配置化并有负向测试） | Linductor-alkaid | M6 | 已于 2026-09-10 冻结（配置定稿 + 测试）；变更走配置级评审 |
| 仓库自身许可证 | 已冻结：MIT（2026-09-10 负责人选定，`v0.1.0` 发布）；与 Executor（MIT）兼容 | Linductor-alkaid | M7（发布前） | 已添加 `LICENSE` 并同步[供应链策略](../supply-chain/dependency-policy.md)与 `README.md` |
| 执行上下文捕获策略 | 已冻结：提交时白名单捕获 + `--env`/`--inherit-env`、executable 提交时解析、环境合并 v2（`LD_LIBRARY_PATH` 转白名单、`LD_PRELOAD`/`YORI_*` 维持拒绝）、无 `--shell`（用 `-- bash -c` 表达）（[DEC-011](../decisions/DEC-011-execution-context-capture.md)，issue #16） | Linductor-alkaid | M8 启动 | 已于 2026-09-12 冻结（负责人确认启动 M8，DEC-011 改 Accepted）；变更需新决策记录 |
| GPU placement 模型 | 已冻结：M9 交付 `ANY`+`REQUIRED`（`--gpu N` = 硬亲和，与 `--gpus N` 计数互斥）；设备身份复用 `GpuUuid`，daemon 侧解析 index→UUID；FIFO 修订为有界跳过（默认扫描 32）；`PREFERRED`/GPU Set/tag/pool/项目级 profile/管理员静态映射延后（[DEC-012](../decisions/DEC-012-gpu-placement-policy.md)，issue #10） | Linductor-alkaid | M9 启动 | 已于 2026-09-13 随 M9 启动冻结（负责人确认依计划推进，DEC-012 改 Accepted，部分修订 DEC-005 队首条款）；变更需新决策记录 |

## 7. 跨里程碑完成定义（DOD）

- `DOD-01` 职责分层正确：调度、状态机、权限在 Core；NVML/SQLite/UDS 在 Adapter；
  依赖方向指向抽象。
- `DOD-02` 全部并发工作受 Executor 管理；任务句柄、取消与关闭路径可见、可测试
  （第 4 节 EXEC 条目）。
- `DOD-03` 取消与 shutdown 路径闭合且有测试；失败对调用方与观察者可见；无吞掉的
  异常、无无主异步任务。
- `DOD-04` 新增并发路径至少覆盖六场景：正常完成、任务异常、提交拒绝、执行中取消、
  超时、shutdown。
- `DOD-05` 容量与背压上限显式（配置项 + 拒绝/溢出事件），并有负向测试。
- `DOD-06` 适用 sanitizer 通过：ASAN/UBSAN 常规；daemon 多线程状态与关闭路径
  TSAN；恢复场景故障注入；新增 parser 纳入 fuzz。
- `DOD-07` 公开契约（API、协议、配置、事件 schema、错误语义）变更同步设计文档与
  示例（工程规范第 8 节同步矩阵）。
- `DOD-08` 验证记录可复现（工程规范第 7 节）；未执行的验证保持未勾选并记录原因、
  负责人与补跑条件。
- `DOD-09` 安全相关变更（权限、身份、路径、协议）同步
  [威胁模型](../security/threat-model.md)并有负向测试。
- `DOD-10` 遇到 Executor 能力缺口时已按规则登记
  [反馈台账](../executor_feedback/ledger.md)并被实现引用；未引入第二套并发设施。

## 8. 拆分与合并顺序建议

- 先骨架后功能：M0 建立可构建、可测试、可校验依赖的空壳。
- 先契约后实现：M1 冻结 Core 接口（`GpuProvider`/`StateStore`/`LaunchAdapter`/
  `IpcTransport`）与状态机语义，配内存/伪实现。
- 先假实现后真实依赖：伪 GpuProvider -> M3 NVML；内存 StateStore -> M4 SQLite；
  环回/进程内调用 -> M5 UDS。
- 先请求/响应后流式：M5 一次性有界协议，M6 流式帧与背压。
- MR 拆分：Core 契约、各 Adapter、观察面、打包各自独立 MR；跨公开契约的变更先
  决策记录后实现。

## 9. 延后项（POST）

| 编号 | 内容 | 立项触发条件 | 设计依据 |
| --- | --- | --- | --- |
| `POST-01` | 多 GPU Job（`--gpus N>1`） | MVP 验收后出现明确的多卡训练需求 | §16.2 |
| `POST-02` | priority、weighted fair queue、per-user 并发/队列配额 | 运行记录显示单用户长期挤占队列 | §16.2 |
| `POST-03` | 显存/CPU/RAM 资源请求维度 | 出现因资源估计不足导致的训练失败记录 | §16.2 |
| `POST-04` | Job 自动重试（`RETRY_WAIT`） | 故障统计显示瞬态失败占比值得自动重试 | §6.2、§16.2 |
| `POST-05` | TUI | MVP 稳定且出现交互需求 | §16.2 |
| `POST-06` | Web UI、Container backend、Job dependency、Reservation、MIG | MVP 与第二阶段稳定后逐项评估 | §16.3 |
| `POST-07` | Heyaki transport、Central Scheduler、多节点调度 | 单节点容量饱和或出现跨服务器调度需求 | §15、§16.3 |
| `POST-08` | pidfd 进程生命周期增强 | 守护总装（M7）已确认 wait + 启动时间核验的不足：采纳进程（daemon 重启后恢复的 Job）非子进程，自然退出的状态不可得（FAILED + 显式原因），且退出发现有约 1 个探测周期的延迟；pidfd（`waitid(P_PIDFD)`）可消除两者 | §10.2、§6.2 |
| `POST-09` | 拆分 `yori-launch-helper`（最小特权 launcher） | MVP 稳定后的安全演进 | §5、DEC-004 |
| `POST-10` | daemon 托管常驻指标面板 | 用户提出常驻 TensorBoard 需求；届时必须新建设计与决策记录 | §11.6、DEC-003 |
| `POST-11` | GPU placement 第二批：`PREFERRED` 模式、GPU Set（`--gpu-any-of`）、affinity-aware 的 `ANY` 设备选择（避开队列中 `REQUIRED` 目标） | M9 交付后出现软偏好或目标卡被 `ANY` 占用的运行记录 | DEC-012、issue #10 |
| `POST-12` | GPU tag/pool 资源池、项目级默认 placement profile、管理员静态 user/project→GPU 映射 | 团队规模化后"专卡专用"需要集中治理 | DEC-012、issue #10 |
| `POST-13` | 独立 `--shell` 提交接口 | 用户普遍需要管道/`&&` 且 `-- bash -c` 表达被证明不足 | DEC-011 |
| `POST-14` | 完整环境 provenance（git commit/dirty、PyTorch/CUDA 版本探测）与训练复现报告 | `yori inspect` 基础 provenance（M8）使用后出现复现/排障需求 | DEC-011、issue #16 |

## 10. MVP 总体验收

MVP 验收判据以[设计文档](../design/yori-project-design.md)第 19 节为准，由 M7
逐项执行并在其里程碑文档记录证据；本文不复制清单，避免双源漂移。需要真实环境的
判据（NVML GPU、至少两个 Linux 用户、systemd）应在 M7 前明确补跑环境与负责人；
CI 无法覆盖的项按工程规范第 4 节保持未勾选并记录原因与补跑条件。

## 11. 文档地图

- 设计：[Yori 项目设计文档](../design/yori-project-design.md)
- 规范：[项目管理与工程规范](../project/project-standards.md)、[AGENTS.md](../../AGENTS.md)
- 计划：[M0 工程骨架与基线](m0-engineering-baseline.md)、
  [M1 核心域契约与进程内调度闭环](m1-core-contracts.md)、
  [M2 进程守护与启动适配](m2-process-supervision.md)、
  [M3 NVML 真实 GPU 集成](m3-nvml-gpu-integration.md)、
  [M4 持久化与恢复](m4-persistence-recovery.md)、
  [M5 IPC 与 CLI](m5-ipc-cli.md)、
  [M6 观察面](m6-observability.md)、
  [M7 打包与 MVP 端到端验收](m7-packaging-acceptance.md)、
  [M8 提交时执行上下文](m8-execution-context.md)
- 决策：[DEC-001 Executor 依赖引入与锁定](../decisions/DEC-001-executor-pinning.md)、
  [DEC-002 MVP 纳入训练观察面](../decisions/DEC-002-mvp-observability.md)、
  [DEC-003 TensorBoard 由 CLI 拉起](../decisions/DEC-003-tensorboard-cli-hosting.md)、
  [DEC-004 root daemon 与 exec 前降权](../decisions/DEC-004-privileged-daemon-demotion.md)、
  [DEC-005 MVP 全局 FIFO 调度策略](../decisions/DEC-005-global-fifo-scheduling.md)、
  [DEC-006 训练进程环境变量继承白名单](../decisions/DEC-006-launch-environment-policy.md)、
  [DEC-007 取消宽限期与升级语义](../decisions/DEC-007-cancel-grace-period.md)、
  [DEC-008 daemon 重启日志管道断裂语义](../decisions/DEC-008-daemon-restart-log-continuity.md)、
  [DEC-009 SQLite StateStore 采用与 dlopen 绑定](../decisions/DEC-009-sqlite-state-store.md)、
  [DEC-010 UDS IPC 端点、权限与管理员判定](../decisions/DEC-010-uds-ipc-endpoint.md)、
  [DEC-011 提交时执行上下文捕获与训练环境恢复](../decisions/DEC-011-execution-context-capture.md)（Accepted，M8）、
  [DEC-012 GPU placement 约束与 FIFO 有界跳过语义](../decisions/DEC-012-gpu-placement-policy.md)（Proposed，M9）
- 安全：[威胁模型（草案）](../security/threat-model.md)
- 供应链：[依赖管理与供应链策略](../supply-chain/dependency-policy.md)
- Executor 反馈：[能力缺口反馈台账](../executor_feedback/ledger.md)
- `docs/compatibility/` 与 `docs/benchmarks/` 按工程规范第 2 节在首份证据文档
  出现时创建。
