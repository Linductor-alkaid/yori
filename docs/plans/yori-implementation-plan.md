# Yori 实施总计划

> 状态：Active
> 版本：1.1
> 更新日期：2026-09-04
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
  `dependencies.lock.json` 锁定（`v0.4.0-82-g4fd8e60`，MIT），已在 M1-01 接入
  构建并形成进程私有 owner；本地五预设已通过，因 clang-tidy-18/Clang 与 PR CI
  尚未补跑，工作项保持未完成。
- 当前里程碑：M1（核心域契约与进程内调度闭环，`In Progress`，计划见
  [M1 里程碑文档](m1-core-contracts.md)）。
- MVP 端到端验收以设计文档第 19 节判据为准，由 M7 执行并记录证据（见第 10 节）。
- 里程碑文档在各自启动时创建（工程规范第 2 节）；当前实体文件：M0、M1。

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
| `EXEC-06` | Job 状态推进与调度触发 | 事件驱动有限任务 `submit_auto()`，保留并消费 future；排队取消用 `submit_cancellable` + `StopToken` | JobManager / Scheduler | 触发事件：新提交、Job 退出、取消、GPU 状态变化、恢复完成、管理员操作 | ④ 停止调度生产者 |
| `EXEC-07` | 进程退出监视与回收（waitpid） | blocking worker 或可取消有限任务 | ProcessSupervisor | wakeup；Job 取消为进程组 `SIGTERM` -> grace -> `SIGKILL`（Yori 外部进程语义，不与 Executor 任务取消混同） | ⑤ |
| `EXEC-08` | SQLite 写入与恢复读取 | 有限任务 `submit_auto()`，逐条 FIFO 串行化提交 | StateStore adapter | 写队列有界，拒绝显式化 | ⑦ 等待终态落盘完成 |
| `EXEC-09` | 状态快照、更新与启动协调 | GPU/调度状态快照 `DoubleBuffer`；Job 状态更新 `MpscChannel`/`LatestMailbox`（按语义）；启动阶段 `PhaseGate`（恢复 -> GPU 观察 -> 调度开启） | 对应组件 | 快照无取消语义，随组件回收 | ⑥ |

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
| M1 | 核心域契约与进程内调度闭环 | M0 | JobSpec、状态机、全局队列、FIFO 调度、GPU lease 记账；内存 StateStore 与伪 GpuProvider 下的进程内可测闭环 | 无 | In Progress |
| M2 | 进程守护与启动适配 | M1 | ProcessSupervisor（spawn、进程组、取消、退出回收）、LaunchProfile、`exec` 前降权、日志捕获与落盘 | 无 | Planned |
| M3 | NVML 真实 GPU 集成 | M2 | `GpuProvider` NVML 适配：发现、UUID 身份、遥测、外部占用检测（`EXTERNAL_BUSY`） | 无 | Planned |
| M4 | 持久化与恢复 | M2 | SQLite StateStore、daemon 重启恢复、PID reuse 核验、`LOST` 语义 | 无 | Planned |
| M5 | IPC 与 CLI | M3、M4 | UDS 传输、`SO_PEERCRED` 鉴权、请求/响应协议与 owner/admin 授权、`submit`/`ps`/`queue`/`gpu`/`cancel`/`logs` 快照、IPC fuzz 起步 | 无 | Planned |
| M6 | 观察面 | M5 | `logs -f` 流式帧（offset 续传、`GAP`/`EOF`/`BACKPRESSURE`）、日志轮转、`yori tensorboard` | 无 | Planned |
| M7 | 打包与 MVP 端到端验收 | M6 | systemd unit、安装打包、设计 §19 判据逐项验收 | `v0.1.0`（MVP） | Planned |

- M3 与 M4 在 M2 完成后可并行推进。
- 里程碑文件命名 `m<N>-<scope>.md`，在该里程碑启动时创建；当前实体文件：
  [M0 工程骨架与基线](m0-engineering-baseline.md)、
  [M1 核心域契约与进程内调度闭环](m1-core-contracts.md)。

## 6. 暂定默认值与未决问题

以下选择为推进而暂定，或为已识别的设计缺口；冻结前变更不受罚，冻结时按工程规范
第 6.2 节落为决策记录。已冻结的决策见 `docs/decisions/`（DEC-001 ~ DEC-005）。

| 项目 | 暂定默认值 / 未决问题 | 负责人 | 最迟冻结 | 冻结动作 |
| --- | --- | --- | --- | --- |
| 调度策略 | 已冻结：严格全局 FIFO，无优先级、配额或 backfill（[DEC-005](../decisions/DEC-005-global-fifo-scheduling.md)） | Linductor-alkaid | M1 | 已于 2026-09-04 冻结；变更需新决策记录替代 DEC-005 |
| 取消 grace period | `SIGTERM` 后等待时长未定（暂定 10s 量级） | Linductor-alkaid | M2 | 写入设计与配置默认值 |
| 环境变量白名单初版 | 继承集合未定 | Linductor-alkaid | M2 | 决策记录 + 设计 §17 条 5 细化 |
| daemon 重启后的日志续捕 | 未决：训练进程 stdout/stderr 管道随 daemon 退出断裂，重启后如何续捕（信号语义、追加写回、`LOST` 边界）需设计补充 | Linductor-alkaid | M2（守护语义）、M6（观察面） | 设计补充 + 决策记录 |
| 持久化实现 | SQLite 为唯一 `StateStore` 实现，内存实现仅测试用（设计 §12） | Linductor-alkaid | M4 | 决策记录 |
| IPC 端点 | `/run/yori/yori.sock`、`root:yori 0660`、`yori` 系统组（设计 §5） | Linductor-alkaid | M5 | 决策记录或设计确认 |
| 日志与跟随上限默认值 | 单文件 256 MiB、保留 1 个历史文件、每 Job 8 / 全局 64 跟随会话、全局磁盘预算（设计 §11.2/11.4） | Linductor-alkaid | M6 | 配置定稿 + 测试 |
| 仓库自身许可证 | 未决 | Linductor-alkaid | M7（发布前） | 选定并添加 LICENSE，同步[供应链策略](../supply-chain/dependency-policy.md) |

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
| `POST-06` | Web UI、Container backend、Job dependency、Reservation、GPU affinity、MIG | MVP 与第二阶段稳定后逐项评估 | §16.3 |
| `POST-07` | Heyaki transport、Central Scheduler、多节点调度 | 单节点容量饱和或出现跨服务器调度需求 | §15、§16.3 |
| `POST-08` | pidfd 进程生命周期增强 | 实现守护时确认 wait + 启动时间核验不足 | §10.2 |
| `POST-09` | 拆分 `yori-launch-helper`（最小特权 launcher） | MVP 稳定后的安全演进 | §5、DEC-004 |
| `POST-10` | daemon 托管常驻指标面板 | 用户提出常驻 TensorBoard 需求；届时必须新建设计与决策记录 | §11.6、DEC-003 |

## 10. MVP 总体验收

MVP 验收判据以[设计文档](../design/yori-project-design.md)第 19 节为准，由 M7
逐项执行并在其里程碑文档记录证据；本文不复制清单，避免双源漂移。需要真实环境的
判据（NVML GPU、至少两个 Linux 用户、systemd）应在 M7 前明确补跑环境与负责人；
CI 无法覆盖的项按工程规范第 4 节保持未勾选并记录原因与补跑条件。

## 11. 文档地图

- 设计：[Yori 项目设计文档](../design/yori-project-design.md)
- 规范：[项目管理与工程规范](../project/project-standards.md)、[AGENTS.md](../../AGENTS.md)
- 计划：[M0 工程骨架与基线](m0-engineering-baseline.md)、
  [M1 核心域契约与进程内调度闭环](m1-core-contracts.md)
  （M2 起随里程碑创建）
- 决策：[DEC-001 Executor 依赖引入与锁定](../decisions/DEC-001-executor-pinning.md)、
  [DEC-002 MVP 纳入训练观察面](../decisions/DEC-002-mvp-observability.md)、
  [DEC-003 TensorBoard 由 CLI 拉起](../decisions/DEC-003-tensorboard-cli-hosting.md)、
  [DEC-004 root daemon 与 exec 前降权](../decisions/DEC-004-privileged-daemon-demotion.md)、
  [DEC-005 MVP 全局 FIFO 调度策略](../decisions/DEC-005-global-fifo-scheduling.md)
- 安全：[威胁模型（草案）](../security/threat-model.md)
- 供应链：[依赖管理与供应链策略](../supply-chain/dependency-policy.md)
- Executor 反馈：[能力缺口反馈台账](../executor_feedback/ledger.md)
- `docs/compatibility/` 与 `docs/benchmarks/` 按工程规范第 2 节在首份证据文档
  出现时创建。
