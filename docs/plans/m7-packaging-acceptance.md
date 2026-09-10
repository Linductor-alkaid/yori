# M7：打包与 MVP 端到端验收（含守护总装收口）

> 状态：Completed
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M6（观察面，PR [#8](https://github.com/Linductor-alkaid/yori/pull/8)）
> 建议发布点：`v0.1.0`（MVP；tag 与发布动作需负责人明确授权后执行）
> 更新日期：2026-09-10

## 目标

1. **守护总装收口**（M5/M6 计划中显式顺延的前置项）：daemon 从"排队与观察
   子集"推进为完整闭环——调度触发联动（EXEC-06 后半）、训练进程启动/退出
   回收/STOPPING 重取消（EXEC-07 后半）、store 写路径经 `StoreTaskRunner`
   串行化（EXEC-08 收口）、启动 `PhaseGate` 编排（EXEC-09 收口）与 EXEC-10
   完整停止序。
2. **打包**：systemd unit（设计 §10.1）与安装打包，`/run/yori` 与
   `/var/lib/yori` 的生产部署路径收敛。
3. **MVP 端到端验收**：按[设计文档](../design/yori-project-design.md)第 19
   节判据逐项验收并记录证据；CI 无法覆盖的真实环境项（NVML GPU、双 Linux
   用户、root/systemd）保持未勾选并记录补跑条件。

## 范围与非目标

范围：

- `ProcessSupervisor` Core 契约扩展：`adopt()`（恢复采纳既有进程）与
  `abandon()`（daemon 关闭时不终止训练进程，RULE-10）。
- `yori_runtime` 新增 `JobManager`：单 blocking worker 的事件驱动守护编排
  （命令通道 + 唤醒管道，沿用 `LogFollowService`/`ProcessExitMonitor` 的既有
  模式），唯一持有队列变更、调度触发、进程守护、`StoreTaskRunner` 写路径。
- store 所有权串行化：`SerialStateStore`（所有权互斥，沿用 `LogStreamer`
  注册表的 mutex 语义）串行化 IPC 读路径与 JobManager 写路径对单 owner
  `StateStore` 的并发访问。
- IPC 服务契约调整：submit/cancel 经注入的 `JobControl` 接口委派给
  JobManager（授权与校验仍在 `IpcService`）；`queue` 视图改由 store 快照
  派生，`GlobalJobQueue` 成为 JobManager 私有派生索引。
- daemon 启动 `PhaseGate`（恢复 -> GPU 观察 -> 调度开启）与 EXEC-10 完整
  停止序（①IPC -> ②跟随会话 -> ③GPU 周期 -> ④调度生产者（abandon 运行
  中进程，RULE-10）-> ⑤退出监视/日志泵 -> ⑦终态落盘排空）。
- 恢复联动：采纳 `kAdoptedRunning`/`kAdoptedPromotedToRunning` Job 并重新
  注册退出监视；`kAdoptedStoppingNeedsRecancel` 重发 SIGTERM + 重建宽限。
- `yorid` 参数：`--log-root`（默认 `/var/lib/yori/jobs`）；日志目录创建与
  父目录链属主校验（威胁模型基线 8 的 M7 项）。
- systemd unit、CMake 安装规则、部署 README。
- 测试：JobManager 单测（六场景 + 恢复采纳/重取消/GPU 事件触发）、
  SerialStateStore 并发测试、IPC 新契约测试、守护闭环集成测试与 E2E 更新。
- 文档：设计 §6.2/§9/§10.2/§11.7/§12/§13、EXEC 表、威胁模型基线 6/8、
  总计划与本文档同步。

非目标：

- 仓库 LICENSE 选定与 `v0.1.0` tag/发布：属负责人决策与授权动作，本里程碑
  交付代码与打包；发布前按总计划第 6 节完成许可证冻结。
- 真实 NVML GPU、双 Linux 用户、root/systemd 环境验收：CI 无对应环境，
  按工程规范第 4 节保持未勾选并记录补跑条件（负责人 Linductor-alkaid，
  目标环境为部署服务器）。
- 多 GPU Job、优先级/配额、自动重试、TUI/Web UI、多节点（POST 项）。
- daemon 运行期动态重载配置、`yori-launch-helper` 拆分（POST-09）。

## 设计与决策依据

- 守护闭环：设计 §9（调度流程与触发事件）、§10.2（进程守护契约）、§6.2
  （恢复决策表，STOPPING 需重取消）、§11.2-11.4（日志捕获/落盘/跟随）、
  §14（JobManager 在 Executor 集成边界中的位置）。
- 关闭语义：`RULE-10`（daemon 关闭不终止训练）、EXEC-10 顺序、DEC-008
  （SIGPIPE 忽略与重启窗口输出丢失）、`abandon()` 语义。
- 取消语义：DEC-007（宽限默认 10s、SIGTERM -> grace -> SIGKILL）、
  `RULE-04`（终态幂等）。
- 写路径串行：DEC-009 第 5 条（adapter 单 owner，串行化由外部承载）、
  EXEC-08（`StoreTaskRunner`）、`SerialStateStore` 的所有权互斥沿用
  `LogStreamer` 头文件中"mutex 是生命周期所有权，不是通信通道的替代"的
  既定语义。
- 启动编排：EXEC-09（`PhaseGate`：恢复 -> GPU 观察 -> 调度开启）。
- IPC 契约：设计 §13.3/§13.5（协议 v1 不变；submit/cancel 的服务端承载
  从"IPC worker 同步写"演进为"JobControl 委派 + 有界等待"，协议 wire
  格式与错误码不变）。
- 安全：威胁模型基线 8（日志/cwd 父目录链属主校验留 M7）、基线 6（root
  端点收敛补跑）、RULE-09。

## 工作项

- [x] `M7-01` 扩展 `ProcessSupervisor` Core 契约：`adopt(ProcessIdentity)`
  采纳已核验身份的既有进程（无管道、不 spawn），`abandon()` 忘记进程且不
  发信号（RULE-10 关闭路径）；覆盖 adopt/abandon/误用矩阵单测并同步设计
  §10.2 与威胁模型。
- [x] `M7-02` 交付 `SerialStateStore` 与 IPC `JobControl` 契约：store 所有权
  互斥串行化（读并发 + 单写者）；`IpcService` 构造改为注入 `JobControl`，
  submit/cancel 委派（授权/校验/脱敏仍在服务内），`queue` 视图由 store 快照
  派生；更新协议/服务单测（wire 格式不变）。
- [x] `M7-03` 交付 `JobManager`（runtime）：blocking worker + 命令通道 +
  唤醒管道；唯一队列/调度/守护 owner；submit（建 Job + 准入 + 触发）、
  cancel（QUEUED 终态化 / 活动态 STOPPING + SIGTERM + 宽限）、调度结果
  落地（身份解析 -> LaunchPlan -> 日志目录/LogSink/LogStreamer -> spawn ->
  STARTING->RUNNING + identity/log_path -> 退出监视注册 -> 日志泵接入）、
  退出回收（终态 + lease 释放同 mutation -> EOF 发布 -> 再调度）、GPU 事件
  触发、恢复采纳与 STOPPING 重取消、`StoreTaskRunner` 写路径接入；六场景
  + Yori 特有场景单测。
- [x] `M7-04` Daemon 全量总装：启动序 = 恢复 -> GPU 观察 -> 观察面 ->
  JobManager（`PhaseGate` 调度开启）-> IPC；停止序为 EXEC-10 完整顺序
  （含运行中进程 abandon 与终态落盘排空）；`yorid` 增加 `--log-root`；
  守护闭环集成测试（FIFO 链式启动、取消升级、重启恢复采纳、E2E 更新）。
- [x] `M7-05` 打包：`packaging/systemd/yori.service`（设计 §10.1 + 加固
  项）、CMake 安装规则与部署 README（目录、权限、admin 组、NVML/SQLite
  库路径）；CI install 步骤验证 unit 随安装部署。
- [x] `M7-06` MVP 验收与文档：设计 §19 判据逐项记录（CI 项附证据，真机项
  记录补跑条件）；同步设计、EXEC 表、威胁模型（基线 6/8）、总计划与
  本文档。

## 风险与阻塞

- `RISK-2026-011` spawn 在 JobManager worker 上同步执行，exec 确认最长
  阻塞 10s（M2 契约），期间其他命令排队；MVP 单机负载下接受，多卡高并发
  再评估（关联 POST 项演进）。
- `RISK-2026-012` 运行中 mutation 失败（如 RUNNING 落盘失败）时进程已被
  spawn：采取"终止进程 + 尽力 FAILED 落盘 + 失败显式记录"路径；store 不
  一致窗口由恢复路径（LOST + lease 释放）收敛，测试覆盖。
- `RISK-2026-013` 既有 M5/M6 E2E 假设"daemon 不启动训练进程"，总装后行为
  改变：测试更新为真实 spawn（同 UID，CI 非 root 可跑），需逐个核对断言。
- 真机验收项（NVML/双用户/root）依赖部署服务器，未执行前保持未勾选。

## 测试与退出条件

- [x] `unit`：adopt/abandon 矩阵（`m7.unit.supervisor-adopt`）；SerialStateStore
  并发读写排空（`m7.unit.serial-state-store`）；JobControl 委派映射（假实现，
  `m5.unit.ipc-service` 更新）；JobManager 六场景（正常完成、任务异常、提交
  拒绝、执行中取消、超时升级、shutdown）+ 恢复采纳/重取消/GPU 触发/迟到
  退出幂等（`m7.unit.job-manager`）；采纳进程退出探测（`m7.unit.exit-monitor-adopted`）。
- [x] `integration`：守护闭环（submit -> FIFO 调度 -> spawn -> 日志落盘与
  跟随 -> 退出 -> lease 释放 -> 队首推进）；取消升级（SIGKILL）；daemon
  重启恢复采纳（RUNNING 不重启、QUEUED 不丢、采纳取消收敛；
  `m7.integration.supervision-closure`，SQLite 持久化跨 daemon 实例）。
  STOPPING 重取消见 `m7.unit.job-manager` 场景"Yori 特有 5"。
- [x] `ipc`：E2E（真实 CLI + UDS）在新总装下全绿（`m5.integration.ipc-e2e`
  更新为真实 spawn 闭环）；`m6.integration.logs-follow-e2e` 在新总装下全绿。
- [x] 五预设构建与 CI 全绿（clang-format/clang-tidy/gcc/clang/sanitizers）：
  本地五预设 42/42、clang-format `--Werror` 与 clang-tidy（18.1.3，
  WarningsAsErrors 类别）零告警；PR [#11](https://github.com/Linductor-alkaid/yori/pull/11)
  最终 CI [全绿](https://github.com/Linductor-alkaid/yori/actions/runs/34446034703)
  （8/8：format/tidy/gcc-13 与 clang-18 的 debug+release/sanitizers/依赖锁定）。
- [x] 设计 §19 判据逐项记录：见下方"MVP §19 验收矩阵"；CI 可判定项全部
  通过；真机项未勾选且有补跑条件、负责人与目标环境。
- [x] 文档同步矩阵（工程规范第 8 节）核对完毕（设计 v0.11、EXEC 表、威胁
  模型基线 8/待完成项、总计划 v1.9、M1 收口记录、本文档）。

## MVP §19 验收矩阵（设计第 19 节判据）

CI（进程内 Fake GPU + 单用户）可判定项：

| 判据 | 证据 |
| --- | --- |
| 仅一张可用 GPU 时第二个 Job 保持 `QUEUED` | `m7.unit.job-manager`（FIFO 链式）、`m5.integration.ipc-e2e` |
| 前一 Job 结束后后一 Job 自动启动 | `m7.unit.job-manager`、`m5.integration.ipc-e2e`（队首推进） |
| Job 以提交用户身份运行、文件 owner 正确 | `m7.unit.job-manager`（同 UID：spawn 无降权空操作 + `LogSink` 属主收敛）；跨 UID 见真机项 |
| `cancel` 终止完整进程组并释放 GPU | `m7.unit.job-manager`（场景 D/E：SIGTERM/SIGKILL + lease 释放）、`m5.integration.ipc-e2e` |
| 存在外部 GPU 进程时不错误分配 | `m3.unit.nvml-gpu-provider`（EXTERNAL_BUSY）+ `m7.unit.job-manager`（GPU 事件触发场景的 EXTERNAL_BUSY 阻塞段）；真实 NVML 见真机项 |
| `yorid` 异常重启后 `QUEUED` Job 不丢失 | `m7.integration.supervision-closure`（SQLite 跨实例重入队） |
| `RUNNING` Job 不因 daemon 重启被无条件重启 | `m7.integration.supervision-closure`（同一进程身份采纳）、`m4.integration.recovery-restart` |
| 能查询 Job、队列、GPU 和日志状态 | `m5.integration.ipc-e2e`（ps/queue/gpu/logs 全链路） |
| `logs -f` 实时跟随、断线续传、CLI 中断不影响训练 | `m6.integration.logs-follow-e2e`、`m5.integration.ipc-e2e`（since 回放 + EOF 对齐）；EOF 前置泵排空语义见设计 §11.7 |
| 日志大小上限与轮转；慢客户端 BACKPRESSURE 断开 | `m2.unit.log-sink`、`m6.unit.log-follow`/`m6.unit.log-streamer` |
| `yori tensorboard` 以提交用户身份、默认回环监听 | `m6.integration.logs-follow-e2e`（PATH 注入假二进制 CLI 契约）；真实 TensorBoard 见真机项 |
| GPU 释放后自动触发下一轮调度 | `m7.unit.job-manager`（场景 A/FIFO/GPU 事件）、`m5.integration.ipc-e2e` |

真机项（未勾选；补跑条件与负责人）：

- [ ] 两个不同 Linux 用户同时 `submit` 且进入同一全局队列（补跑条件：部署
  服务器两个真实用户 + `yori` 组；负责人：Linductor-alkaid）。
- [ ] CLI 退出 / SSH 断开不终止训练（补跑条件：真实终端会话 + daemon；
  负责人：Linductor-alkaid；CI 以"训练进程独立进程组 + CLI 子进程退出"近似
  覆盖于 `m7.integration.supervision-closure`）。
- [ ] `yori tensorboard` 启动真实 TensorBoard（补跑条件：安装 tensorboard 的
  观察机；负责人：Linductor-alkaid）。
- [ ] root/systemd 部署：`/run/yori/yori.sock` root:yori 0660 收敛、多用户
  连接准入、`systemctl stop yori` 后训练存活并恢复采纳（补跑条件：root +
  systemd + NVIDIA GPU 服务器；负责人：Linductor-alkaid；部署步骤见
  `packaging/systemd/README.md`）。
- [ ] 真实 NVML GPU 上外部占用不误分配（M3 已有 2026-09-10 RTX 4080 SUPER
  单卡证据；多卡 + 守护闭环组合补跑随上项）。

## 验证记录

2026-09-10：守护总装与打包落地（本地验证，Linux x86_64，GCC 13，debug
预设，工作树 `feat/m7-daemon-assembly-packaging`）。

- 实现：`ProcessSupervisor::adopt/abandon`、`ProcessExitMonitor` 采纳进程
  周期探测（默认 1s，仅存在采纳 PID 时激活）与事件监听回调、
  `GpuManager::set_event_listener`、`SerialStateStore`、IPC `JobControl`
  委派（submit/cancel/queue 视图）、`JobManager`（命令通道 + 唤醒管道，
  调度/守护/写路径唯一 owner）、`Daemon` 全量总装（`PhaseGate` 启动编排与
  EXEC-10 完整停止序）、`yorid --log-root`、systemd unit 与安装规则。
- 测试：`ctest --test-dir build/debug` -> **42/42 通过**（37 项既有 + 5 项
  新增：`m7.unit.supervisor-adopt`、`m7.unit.serial-state-store`、
  `m7.unit.job-manager`、`m7.unit.exit-monitor-adopted`、
  `m7.integration.supervision-closure`）；4 项环境依赖用例按既有约定显式
  skip（root 降权、真实 NVML、multi-user、performance）。
- 全预设：debug/release/asan/ubsan 各 **42/42** 通过；TSAN 以
  `setarch -R ctest --test-dir build/tsan`（关闭 ASLR，本机限制）**42/42**
  通过。TSAN 修复两处真实竞态：(1) 测试直传未包装的 InMemoryStateStore
  （IPC 读 vs StoreTaskRunner 写）-> E2E 统一经 SerialStateStore 包装；
  (2) `SchedulerTaskRunner/StoreTaskRunner::stop_accepting` 在 owner 线程调用
  而 trigger/submit 在 JobManager worker -> 违反单 owner 契约，移入 worker
  退出路径（EXEC-10 ④）。ASAN 修复一处测试 use-after-free（快照临时量的
  内部指针）。clang-format 18 `--Werror` 通过；clang-tidy 18
  WarningsAsErrors 类别（bugprone/clang-analyzer/performance/portability）
  零告警。
- 行为修正（相对 M5/M6 假设）：E2E 更新为真实 spawn 闭环；`logs -f` 的 EOF
  延迟到该 Job 日志泵排空后发布（退出事件与泵完成任意先后，两者到齐才发布，
  跟随者不丢尾部块）；daemon 关闭 abandon 运行中训练（RULE-10）。
- 限制：GitHub CI（GCC/Clang 双编译器矩阵与远端门禁）随 PR 收口；真机项见
  验收矩阵（root/systemd/双用户/NVML 组合）。
