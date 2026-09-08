# M3：NVML 真实 GPU 集成

> 状态：In Progress
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M2（[进程守护与启动适配](m2-process-supervision.md)）
> 建议发布点：无
> 更新日期：2026-09-08

## 目标

交付真实 GPU 资源侧集成：`NvmlGpuProvider` 以 `dlopen` 绑定 NVML C API，实现 M1
冻结的 `GpuProvider` SPI（设备发现、UUID 稳定身份、utilization/显存遥测、外部计算
进程占用检测并映射 `EXTERNAL_BUSY`），并交付 `GpuManager` 的 Executor 承载
（总计划 `EXEC-05`：`submit_periodic` + `TimerHandle` 周期采样；`EXEC-09` 的 GPU
快照部分：`DoubleBuffer` 发布与观测状态变化事件）。测试通过自研可控 stub NVML
共享库在无 GPU 环境覆盖适配器行为，真实 GPU 用例以 `platform;gpu` 标签接入。

## 范围与非目标

范围：

- `NvmlGpuProvider` 公开契约与实现：显式 `load()`（`dlopen` + `dlsym` +
  `nvmlInit_v2`），单次 `observe()` 完成 `nvmlDeviceGetCount_v2`/`GetHandleByIndex_v2`/
  `GetUUID`/`GetUtilizationRates`/`GetMemoryInfo`/
  `GetComputeRunningProcesses_v3`（回退 `_v2`）并组装为已冻结的
  `GpuObservationSnapshot`；错误映射为 `kBackendUnavailable`/
  `kPermissionDenied`/`kObservationFailed`。
- 适配器语义（写入设计第 7 节）：外部占用以“存在计算进程（含容器内不可见句柄时
  报告的进程计数 > 0）”判定为 `EXTERNAL_BUSY`，不读取进程身份；占用查询
  `NOT_SUPPORTED`/`GPU_IS_LOST` 映射设备 `UNAVAILABLE`（按策略不可调度）；
  设备 UUID 不可读时整次观测失败（保持设备群身份完整，防止静默缩编导致 lease
  悬空）；遥测失败仅省略对应 optional 字段。
- `GpuManager`（`yori_runtime` 内部组件）：start 先同步完成首次观测并建立基线，
  再提交周期任务；每次 tick 校验、经 `DoubleBuffer` 发布快照、对比观测状态迁移
  （设备出现/消失/状态翻转）发送有界事件；错误记入 streak 统计并在 streak
  开始/结束时显式通知；事件通道满时计数并在下一条事件补投 `gap` 标记；stop 为
  取消 `TimerHandle` + 有界等待 `active_callback_count` 归零（以 Executor 自身
  状态为事实源）。
- 测试：自研 stub NVML 共享库（控制接口注入设备表、遥测与逐调用错误）驱动的
  适配器单测；`GpuManager` 六场景（见测试与退出条件）与 M3 特有场景（状态迁移
  事件、遥测-only 变化不触发事件、背压补投、join 超时、先 shutdown 后 stop）；
  GpuManager 事件驱动 `FifoScheduler` 的进程内集成闭环；真实 GPU 探测用例升级为
  走 `NvmlGpuProvider`。
- 文档：设计第 7 节（适配器语义）、威胁模型（NVML 观测面条目）、总计划状态与
  文档地图同步。

非目标：

- SQLite 持久化与 daemon 重启恢复（M4）；IPC/CLI 与 `yori gpu` 命令（M5）。
- GPU lease 记账与调度决策（M1 已交付，归 Core；M3 只供给观测与触发事件）。
- MIG、`nvmlDeviceGetGraphicsRunningProcesses`（图形进程）、UUID_v2/MIG 实例
  身份、`nvmlDeviceGetName` 诊断字段（`POST-06` 后随需求评估；设计第 7 节语义
  以计算进程为外部占用判据）。
- `GpuManager` 与 JobManager/daemon 总装（M5 起）；`EXEC-09` 的 Job 状态更新与
  启动 `PhaseGate` 部分（M1-06 遗留，随 daemon 总装落地）。
- NVML 事件订阅（XID/黑名单通知）与实时路径（遥测采样允许抖动，按 timer 能力
  承载即可）。

## 设计与决策依据

- GPU 资源模型、SPI 与 lease/观测分离：[设计文档](../design/yori-project-design.md)
  第 7 节（`GpuObservedState` 不含 `ALLOCATED`；lease 是调度事实，NVML 是资源
  观测，`RULE-05`）。
- 外部占用不接管不终止外部进程：设计第 7 节、
  [威胁模型](../security/threat-model.md) 基线条目 10（`EXTERNAL_BUSY` 测试）。
- 调度触发事件含“GPU 状态变化”且调度器事件驱动：设计第 9 节、`RULE-07`；
  `GpuManager` 仅在观测状态迁移时发送事件，遥测波动不唤醒调度。
- Executor 承载：总计划 `EXEC-05`（周期任务 + `TimerHandle`，holder 为
  GpuManager，关闭阶段 ③）、`EXEC-09` GPU 快照部分（`DoubleBuffer`）；pinned
  integration skill 的 Scheduling 与 Communication capability card（周期任务重叠
  可能性、cancel 不撤回在途回调、`DoubleBuffer` 是状态快照非队列）。
- 平台解耦：`RULE-02`（NVML 只经 `GpuProvider` 接入、公开 API 不暴露 NVML 类型）；
  `dlopen` 绑定 + 内部最小 NVML 声明使构建不依赖驱动/SDK，lib 路径仅由管理员
  配置，不受用户输入影响。
- 真实依赖不可用于 CI：工程规范第 11 节测试标签（`gpu` 标签显式 skip + 补跑
  条件；核心逻辑以 stub/伪实现覆盖）。

## 工作项

- [ ] `M3-01` 实现 `NvmlGpuProvider`：`dlopen` 绑定与显式 `load()`、设备发现与
  UUID 身份、utilization/显存遥测（失败省略）、外部计算进程占用检测映射
  `EXTERNAL_BUSY`、`NOT_SUPPORTED`/`GPU_IS_LOST` 映射 `UNAVAILABLE`、NVML 错误
  到 `GpuProviderErrorCode` 的显式映射；NVML 类型不出现在公开头。
- [ ] `M3-02` 实现 `GpuManager`（`EXEC-05`/`EXEC-09`）：start 首次同步观测建基线 +
  `submit_periodic_with_handle` 周期采样；快照经 `DoubleBuffer` 发布；观测状态
  迁移（出现/消失/翻转）发送有界事件，遥测-only 变化不发送；provider 错误
  streak 统计与开始/结束事件；事件背压显式计数与补投；stop 取消 `TimerHandle`
  并有界 join 在途 tick。
- [ ] `M3-03` 交付 stub NVML 测试库与适配器测试：可控共享库注入设备表/遥测/逐
  调用错误，覆盖 load 失败、init 错误映射、发现与 UUID、遥测省略、外部占用、
  v3→v2 回退、UUID 不可读整次失败、revision 单调；真实 GPU 探测用例改为经
  `NvmlGpuProvider`（`platform;gpu`，无驱动显式 skip）。
- [ ] `M3-04` 交付 `GpuManager` 与调度集成测试：六场景（正常完成、任务异常=
  provider 错误/快照非法、提交拒绝=Executor 已关闭、执行中取消=运行中 stop、
  超时=stop join 截止、shutdown=先 shutdown 后析构）+ M3 特有场景（状态迁移
  事件、遥测-only 不触发、设备消失、背压补投、streak 起止）+ GpuManager 事件
  驱动 `FifoScheduler` 建立 lease 的进程内闭环；`debug`/`release`/`asan`/
  `ubsan`/`tsan` 预设与 CI 通过。
- [ ] `M3-05` 同步文档与验证记录：设计第 7 节适配器语义、威胁模型 NVML 观测面、
  总计划（第 1/5/11 节）、本计划验证记录；若发现 Executor 能力缺口按工程规范
  9.4 登记台账。

## 风险与阻塞

- NVML ABI 兼容性：以内部最小声明 + 只读返回码/计数字段的方式绑定，避免依赖
  结构体布局细节；stub 与适配器共享同一声明头，配对 ABI 一致。真实驱动行为
  （如 `INSUFFICIENT_SIZE` 语义）以 `gpu` 标签用例在带 GPU 主机补验。
- 周期任务在途回调与组件生命周期：cancel 不撤回已派发回调；stop 以
  `active_callback_count` 有界 join，join 超时作为显式结果返回（不静默）；
  tick 回调经 `shared_ptr<Impl>` 延长状态生命周期，provider 引用由 owner 保证
  存活（与 M2 GraceEscalation/ExitMonitor 相同的 owner 纪律）。
- 本机无 clang-tidy-18 与 Clang 编译器、无 NVIDIA GPU；TSAN 需 `setarch -R`
  （既有环境限制），以 PR CI 为最终门禁，真实 GPU 用例保持补跑项。
- 无 GPU 环境下 CI 对适配器的覆盖完全依赖 stub 的保真度；stub 行为与真实驱动
  的差异（错误码组合、时序）需在 M7 验收前以真实主机复核。

## 测试与退出条件

- [ ] 适配器测试通过（stub 驱动）：load 错误映射（库缺失/驱动未装载→
  `kBackendUnavailable`、`NO_PERMISSION`→`kPermissionDenied`）；发现与 UUID 组装；
  遥测成功/失败省略；计算进程计数>0→`EXTERNAL_BUSY`、=0→`FREE`；
  `NOT_SUPPORTED`/`GPU_IS_LOST`→设备 `UNAVAILABLE`；UUID 读取失败→整次
  `kObservationFailed`；v3 缺失回退 v2；revision 单调；`observe()` 未 load 的
  显式错误；析构调用 `nvmlShutdown` 且幂等。
- [ ] `GpuManager` 测试通过：start 首次发布与基线；周期 tick 发布快照；观测
  状态迁移（出现/消失/翻转）产生事件且事件携带变更 UUID；遥测-only 变化不产生
  事件；provider 错误 streak 起止事件与统计；非法快照不发布且计数；事件通道
  满时丢弃计数与后续补投标记；stop 后不再产生新 tick 且 join 有界；stop 幂等；
  Executor 已关闭时 start 显式拒绝；先 shutdown 后 stop/析构不挂起不崩溃。
- [ ] 集成闭环测试通过：Job 排队 + GPU `EXTERNAL_BUSY` 时队首阻塞；外部占用
  消失→`GpuManager` 状态变化事件→以 `DoubleBuffer` 快照驱动 `FifoScheduler`
  →Job 进入 `STARTING` 且 lease 建立（store 断言）；lease 归 Core、观测与 lease
  分列不被破坏。
- [ ] 真实 GPU 探测用例（`platform;gpu`）在无驱动环境显式 skip 并给出补跑条件。
- [ ] `debug`/`release`/`asan`/`ubsan`/`tsan` 预设全部通过；PR CI（GCC 13/
  Clang 18 的 Debug/Release、clang-format 18、clang-tidy 18、sanitizers、依赖
  门禁）全绿。
- [ ] 设计（第 7 节）、威胁模型、总计划（第 1/5/11 节）与本计划同步更新。

## 验证记录

（随实施追加；格式：日期、范围/commit、环境、命令与结果、限制与补跑条件、
同步说明。）
