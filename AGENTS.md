# Yori 项目协作约定

## 适用范围

本文件适用于 Yori 仓库中的全部自研代码、测试、文档与构建配置。`third_party/`
中的上游代码遵循其自身约定；除非任务明确要求升级或修复依赖，否则不要修改其中的
代码。本文件是仓库级最高强制约束，与[项目管理与工程规范](docs/project/project-standards.md)
（下称“工程规范”）配套使用：本文件定义底线，工程规范定义完整流程、模板与证据要求。

## 项目管理与文档规范

所有计划、里程碑、设计、决策、验证证据和文档变更必须遵循工程规范第 1-8 节。开始非平凡
变更前，必须确认所属计划工作项、相关设计和决策依据；完成时必须同步更新任务状态、测试
结果、验收证据以及受影响文档。环境限制导致的未执行验证不得标记为完成，必须记录原因、
负责人和补跑条件。

## 产品目标

Yori 是使用现代 C++ 构建的单节点多用户 GPU 训练任务排队、调度与进程守护系统。它解决
多用户共享一台多 GPU Linux 服务器时的 GPU 争抢与人工排队问题。核心闭环为：

`yori submit 提交 JobSpec -> yorid 全局队列排队 -> Scheduler 匹配空闲 GPU 并建立 lease -> 以提交用户身份启动训练进程并守护 -> 用户运行时观察（状态/日志/TensorBoard） -> 进程退出、释放 lease 并触发下一轮调度 -> 核验（恢复、权限、终态与资源回收正确）`

一台服务器只运行一个 `yorid`，它是唯一 authoritative scheduler；`yori` CLI 均为无状态
客户端。Yori 不理解 PPO、Isaac Lab、PyTorch 等训练框架语义，也不要求训练程序链接
Yori 或 Executor。实现必须保持 Core 与具体平台/传输/供应商解耦：NVIDIA NVML、SQLite、
Unix Domain Socket、systemd 等宿主能力只能通过 Adapter/Provider 接入，不得渗入 Core。
优先围绕以下接口形成稳定边界：

- `GpuProvider`：GPU 发现、遥测、外部占用检测与 GPU lease 的资源侧接口（NVML 适配实现）。
- `StateStore`：Job 与资源状态的持久化、恢复与审计接口（SQLite 适配实现）。
- `LaunchAdapter`：由 LaunchProfile 驱动、将调度结果映射为训练命令环境的启动适配接口
  （`CUDA_VISIBLE_DEVICES`、物理 GPU 参数模板等）。
- `IpcTransport`：CLI 与 daemon 之间的请求/响应与流式观察通道接口（Unix Domain Socket
  适配实现；多节点阶段由 Heyaki transport 接入）。

调度策略（MVP 全局 FIFO）、Job 状态机、多用户权限与进程守护属于 Yori Core，不得下沉到
Executor，也不得依赖具体 GPU 厂商类型。

## Executor 是强制并发基础设施

Yori 必须依赖仓库中的 pinned `third_party/executor` 管理所有并发任务和运行生命周期。
集成时以其公开头文件、`third_party/executor/docs/API.md` 和
`third_party/executor/docs/skill/executor-integration/SKILL.md` 为准；本地资源与 pinned
版本一致，优先于其他版本的文档。

以下规则是强制要求：

1. 所有异步任务、延时任务、周期任务、依赖任务、阻塞 I/O worker、实时控制任务及其启动、
   停止、等待和诊断，必须通过 Executor 的公开能力管理。创建、接纳、取消、排空、关闭、
   完成和失败必须对 Executor 保持可见。
2. 自研代码不得使用 `std::thread`、`std::jthread`、`std::async`、自建线程池、私有定时
   调度器、detached worker 或脱离 Executor 生命周期的 fire-and-forget 工作。平台 API 要求
   线程亲和时，将其封装在 Platform Adapter 中，并通过 Executor 支持的外部事件循环或扩展
   边界协调。
3. 普通有限任务优先使用 `submit_auto()`，并保留、消费其 `future`。成功入队只表示 admission，
   不等于执行成功；影响任务结果的异常不得被丢弃。
4. 跨执行上下文通信按语义选用 `executor::comm` 组件：逐条 FIFO 用 `MpscChannel`，最新状态
   用 `LatestMailbox`，一致快照用 `DoubleBuffer`，实时路径用 `RealtimeChannel`，多订阅广播
   用 `Topic`，启动阶段协调用 `PhaseGate`。不得自建 ad-hoc 队列，也不得以“共享可变状态 +
   mutex + 条件变量”替代 executor 通信。
5. 长期阻塞 I/O 使用 Executor 的 blocking worker 生命周期能力；允许抖动的后台周期任务使用
   timer 能力（`submit_delayed`/`submit_periodic` 及 `TimerHandle`）；固定周期或低延迟控制
   使用 realtime/low-latency 能力。不得用普通周期任务冒充实时控制。Yori 中的典型承载：
   IPC 连接等待与日志跟随 reader 走 blocking worker，NVML 遥测采样走周期任务，GPU lease
   与调度状态快照走 comm 组件。
6. 取消必须是协作式的：优先 `submit_cancellable` + `StopToken`/`request_task_cancel`，使
   排队期与运行期取消进入 Executor 生命周期视图。长任务和循环必须定期检查取消状态，并
   保证等待、网络和平台动作具有可解除阻塞的路径。超时不是强制终止，queued soft timeout
   不覆盖运行中任务。Job 取消最终落到进程组 `SIGTERM -> grace -> SIGKILL`，属于 Yori 的
   外部进程语义，不与 Executor 任务取消混同。
7. Executor 必须由明确的外部 owner 初始化和关闭，每个进程的 owner 唯一（`yorid` 的 owner
   是 daemon 主生命周期；`yori` CLI 若使用 Executor，owner 是 CLI 自身）。关闭顺序为：停止
   任务生产者，发出取消或停止请求，回收 worker/实时路径，等待需要完成的有限任务，最后由
   非 worker 线程执行 `shutdown(true)`。不得从 Executor worker 内完成自等待或最终 teardown。
8. Runtime、Service、Controller 和 Adapter 不得各自隐藏全局 Executor 生命周期。依赖通过构造
   参数或明确 context 传递；资源所有权、任务句柄和关闭顺序必须可见且可测试。
9. 使用 Executor 的监控、状态、失败事件和 `executor::comm` 统计作为任务健康的事实源；新
   任务路径必须使 admission 拒绝、执行失败、超时/取消、背压和关闭状态可经 Executor 设施
   观察。不得建立平行任务监控子系统。
10. 队列容量（含 `max_in_flight_tasks` 总量准入）、背压/drop 策略、超时、取消和关闭中的提交
    必须转化为明确结果与事件，不得静默重试、无限排队或吞掉失败。日志跟随与 IPC 流式推送
    的慢客户端策略同样必须有界且显式。
11. 第三方库回调（NVML 事件、systemd sd_notify、未来 Heyaki 回调）只做有界校验和投递，
    业务 handler 不得直接在第三方线程执行；经第三方事件循环的延续派发须遵守集成指南的
    豁免纪律并保持可追溯。

## Executor 文档与能力路由

设计和实现并发行为前，先使用 pinned 依赖自带的资源，按其路由说明只加载相关的 router 和
capability card，不读取无关卡片或实现源码：

- 应用集成：`third_party/executor/docs/skill/executor-integration/SKILL.md`。
- 仅在获得修改 Executor 本体的明确指示后，才使用
  `third_party/executor/docs/skill/executor-maintainer/SKILL.md`，并遵循其 source、
  invariant、test 和 documentation 检查。maintainer skill 不是修改依赖的隐式授权。
- 用户指南：`third_party/executor/website/`（zh/en）中 lifecycle、submission、communication、
  monitoring、failure、realtime 与 reliability 各节。公开头文件与 pinned 指南是权威，优先
  公共 facade 和文档化组件，而非本地抽象或实现细节。

## Executor 能力缺口与反馈台账

不得为了绕过 Executor 的能力边界而静默引入另一套并发或生命周期设施。当确认 Executor 无法
满足 Yori 的合理需求时，必须按工程规范第 9.4 节执行反馈流程：

1. 先核对当前版本的公开头文件、API 文档、集成指南及相关测试，排除 API 选型错误、配置
   错误、平台限制和应用层职责。
2. 在 `docs/executor_feedback/ledger.md` 中新增唯一编号（`EXE-YYYYMMDD-NNN`）的反馈记录，
   附可复现证据、影响范围、期望语义、建议的最小能力、延期影响和可验收结果。只写
   “Executor 不支持”不构成有效记录。
3. 在相关代码、测试或设计文档中引用该反馈编号。
4. 确需临时方案时，将其限制在单一 Adapter/compatibility boundary 内，说明行为差异、风险、
   移除条件和测试覆盖；临时方案不得创建线程、队列或调度器。
5. 未经明确授权，不直接修改 `third_party/executor` 来掩盖集成问题，也不把项目特有策略
   下沉到通用 Executor；先报告并等待明确指示。

以下情况不是 Executor 能力缺口：NVML/GPU 驱动适配、Linux 进程与权限语义（UID/GID 切换、
`SO_PEERCRED`、信号、进程组）、Unix Domain Socket 与 systemd 集成、调度与队列策略
（FIFO、优先级、配额）、TensorBoard 等外部观测工具的托管方式、错误使用已有 API、平台
本身不提供所需权限或设备。它们应在 Yori 对应层解决。

## Runtime 与状态模型

- Job 必须实现为可推进、可观测、可中断的显式状态机，而不是不可中断的单体 `run()`。
  标准状态集：`QUEUED / STARTING / RUNNING / STOPPING / FINISHED / FAILED / CANCELLED / LOST`
  （未来加入自动重试时扩展 `RETRY_WAIT`）。GPU 逻辑状态（`FREE / ALLOCATED / EXTERNAL_BUSY /
  UNAVAILABLE`）与 Job 状态机解耦，由 GpuProvider adapter 维护观测事实、由 Core 维护 lease
  事实。
- 每个推进步骤应是有界工作单元；NVML 扫描、日志跟随、IPC 收发、SQLite 写入和进程回收
  交给 Executor 管理，并通过结构化结果推动状态转换。调度器必须事件驱动，不得依赖高频
  轮询。
- Job 与 GPU lease 必须有稳定 ID（JobId、GPU UUID）和明确的取消上下文与生命周期所有者。
  终态必须幂等；迟到的退出结果、取消响应或恢复核验结果不得让已取消/已完成的 Job 重新
  进入活动状态。
- Job 恢复必须核验进程身份（PID/PGID/启动时间），防止 PID reuse 错误接管；无法确认时进入
  `LOST`。绝不能因 daemon 重启就无条件重新启动数据库中的 RUNNING Job。
- 管理员干预（禁用 GPU、强制取消、紧急停止）必须阻止新的调度与自主动作，取消或安全收敛
  正在执行的任务，并在恢复调度前重新观察 GPU 与队列状态。daemon 正常关闭不等于杀死运行中
  训练进程，两者的语义必须显式区分。

## 工程约束

- 使用 C++20 和 CMake（≥ 3.25）+ `CMakePresets.json`；公开 API 避免暴露平台与第三方类型
  （NVML、SQLite、UDS 细节），平台相关编译单元（NVML adapter、systemd unit）保持可选。
  依赖方向指向抽象，Adapter 依赖 Core 接口，不得反向依赖。
- 所有跨线程共享状态必须有明确所有权或使用 Executor 提供的通信原语；不得依赖隐式全局
  可变状态。所有队列、缓存、并发 operation、日志缓冲和 IPC 流都必须有容量或预算上限。
- 安全是核心功能：绝不以 root 执行用户提交的命令；用户身份必须来自 `SO_PEERCRED` 等内核
  机制；`exec` 前完成 supplementary groups、`setgid`、`setuid`；环境变量白名单继承；
  `/run/yori/yori.sock` 权限收敛；Job 查询、日志读取、取消和观察接口执行 owner/admin 授权。
- 为状态转换、取消竞态、关闭顺序、队列拒绝、IPC 协议解析和边界校验编写测试。新增并发路径时
  至少验证：正常完成、任务异常、提交拒绝、执行中取消、超时、shutdown。另须覆盖 Yori 特有
  场景：daemon 重启恢复、PID reuse、外部 GPU 占用、多用户权限隔离、慢日志客户端背压。
- ASAN/UBSAN 常规运行；daemon 多线程状态与关闭路径增加 TSAN；恢复与多进程场景增加故障
  注入；IPC parser 纳入 fuzz。
- 变更公开契约时同步更新设计文档和示例。不得宣称未通过目标平台或基准验证的实时性、性能
  或跨平台保证。

## Git 提交与仓库纪律

Commit、分支、MR、评审与合并必须遵循工程规范第 10 节。要点：

- Commit Message 使用 `<type>(<scope>): <subject>`；type 限于
  `feat`/`fix`/`refactor`/`perf`/`docs`/`test`/`build`/`ci`/`chore`/`revert`，scope 取自
  工程规范 10.2 定义的 Yori scope 词表。一个 Commit 对应一个独立逻辑修改；含义不明确的
  提交说明不可接受。
- `master` 是保护分支，只能经 MR 合入；不得 force-push 或改写已发布历史。
- MR 标题同 Commit 格式，描述必须包含修改内容、修改原因、实际执行的测试和影响范围；
  未执行测试不得填写“测试通过”。
- 提交前检查 `git status` / `git diff` / `git diff --cached`：不包含无关格式化、临时 Debug
  代码、运行日志、编译产物、IDE 文件、大文件和敏感信息；不把无关工作树改动带入提交。
- `user.name` 与 `user.email` 必须是提交者本人，严禁使用他人身份提交。
- Agent 可以在需要触发或验证 CI 时创建范围化 commit 并以普通非 force 方式 push 当前工作
  分支（含同一请求内修复 CI 失败的后续提交），但不得合并 PR、创建 release/tag、修改仓库
  设置或推送他人分支；这些操作仍需用户明确授权。
- 认证凭据不得打印、复制进仓库、写入远程 URL、暴露于进程参数或日志。`gh` CLI 可用于
  GitHub API 操作，使用前以 `gh auth status` 确认身份。

## 完成定义

一项 Yori 变更只有在以下条件满足时才算完成：职责位于正确层（调度/状态机/权限在 Core，
NVML/SQLite/UDS 在 Adapter）；全部任务受 Executor 管理；取消和 shutdown 路径闭合；失败对
调用方和 Observer 可见；关键事件可复现；相关测试通过；计划状态、设计、决策和验收证据已经
同步；Commit 与 MR 符合仓库纪律；若遇到 Executor 能力缺口，反馈台账已经按要求登记并被实现
引用。
