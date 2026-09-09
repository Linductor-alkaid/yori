# Yori 项目设计文档

> **定位**：单节点多用户 GPU 训练任务排队、调度与进程守护系统\
> **状态**：设计草案 v0.9（冻结 M5 IPC 协议 v1、UDS 传输与授权脱敏语义）\
> **日期**：2026-09-09

## 1. 项目摘要

Yori 是面向共享 GPU
训练服务器的轻量级多用户工作负载调度器。其核心目标是解决多个算法研发人员在同一台多
GPU Linux 服务器上进行强化学习、深度学习或其他 CUDA 训练时的 GPU
争抢与人工排队问题。

用户不再自行观察空闲 GPU 后直接启动训练，而是将训练命令提交给 Yori；Yori
维护服务器级唯一任务队列，在 GPU
可用时为任务分配设备并主动启动训练进程，同时持续守护任务生命周期。

Yori 不理解 PPO、Isaac Lab、PyTorch 等训练框架的业务语义，也不要求
Executor 理解 GPU。职责划分为：

-   **Yori**：Job、GPU、队列、调度策略、进程生命周期与多用户权限。
-   **Executor**：Yori 内部的并发执行、阻塞
    I/O、事件传递和任务协调基础设施。
-   **Heyaki**：未来多节点版本可选的远程控制面与节点通信基础设施。

------------------------------------------------------------------------

## 2. 设计目标与非目标

### 2.1 核心目标

1.  **服务器级统一队列**\
    同一服务器上的所有授权用户共享唯一 authoritative
    scheduler，避免各用户独立排队造成 GPU 资源竞争。

2.  **自动 GPU 分配**\
    检测可用 NVIDIA GPU，在满足资源请求时自动启动排队任务，直到无空闲
    GPU 或无待执行任务。

3.  **训练代码低侵入**\
    默认将训练程序视为黑盒外部进程，通过启动环境和可配置启动适配器完成
    GPU 映射，而不是解析或修改训练源码。

4.  **多用户隔离**\
    任务以提交用户的 UID/GID 身份运行，使输出文件、缓存、checkpoint
    和日志保持正确的用户权限。

5.  **进程守护**\
    CLI 退出或 SSH 断开不影响训练；daemon
    负责启动、监控、取消、回收和状态恢复。

6.  **持久状态**\
    队列和 Job 元数据写入 SQLite。daemon
    重启后能够恢复队列，并核验原有运行进程。

7.  **运行时可观察**\
    训练在后台运行期间，用户通过 `yori ps` 看 Job 与 GPU 状态、`yori logs
    [-f]` 实时跟随训练日志、`yori tensorboard` 一键拉起指标可视化，替代原本
    “tmux 里挂着训练 + 手动开 TensorBoard”的工作流。

8.  **可演进架构**\
    MVP 聚焦单节点，同时为优先级、用户配额、多 GPU
    Job、TUI/Web、多节点调度和 Heyaki transport 留出空间。

### 2.2 非目标（MVP）

-   不实现 Kubernetes、Slurm 等通用集群编排系统的完整能力。
-   不自动识别任意现存 Python 进程并将其接管为 Yori Job。
-   不解析或修改用户训练源码来替换 GPU 编号。
-   不在 MVP 中实现跨服务器全局调度。
-   不以 `GPU utilization == 0` 作为唯一 GPU 空闲判据。
-   不要求训练程序链接 Yori、Executor 或 Heyaki。

------------------------------------------------------------------------

## 3. 总体架构

``` text
用户 A ─┐
用户 B ─┼── yori CLI ── Unix Domain Socket ──► yorid
用户 C ─┘                                      │
                                               ├─ IPC Server / Auth
                                               ├─ JobManager
                                               ├─ Global JobQueue
                                               ├─ Scheduler
                                               ├─ GpuManager (NVML)
                                               ├─ ProcessSupervisor
                                               ├─ LogStreamer（logs -f 跟随）
                                               ├─ StateStore (SQLite)
                                               └─ Executor runtime
                                                      │
                                                      └─ 用户身份训练进程
```

每台服务器只运行一个 `yorid`。

所有 `yori` CLI 实例均为无状态客户端，通过 Unix Domain Socket
将请求发送给 `yorid`。队列、GPU lease 和 Job 状态仅由 `yorid`
修改，因此服务器内部不存在多个调度器对同一 GPU 的竞态。

核心原则：

> **一个服务器 = 一个 authoritative scheduler。**

而不是：

> 一个用户 = 一个 scheduler。

------------------------------------------------------------------------

## 4. 组件职责

  -----------------------------------------------------------------------------------------------------------------
  组件                    职责                                               关键边界
  ----------------------- -------------------------------------------------- --------------------------------------
  `yori` CLI              `submit`、`ps`、`queue`、`logs`、`cancel`、`gpu`   不维护全局状态，不直接启动受调度训练
                          等用户入口                                         

  `yorid`                 服务器级唯一调度 daemon，持有队列、资源和 Job      由 systemd 管理；不能把用户命令直接以
                          生命周期                                           root 执行

  `JobManager`            JobId、状态机、所有权、查询、取消和恢复            Job owner 来自内核 peer credential

  `GpuManager`            NVML 资源发现、外部占用检测、GPU lease、遥测       NVML 遥测不能替代 Yori 自身 ownership

  `Scheduler`             根据队列与 GPU 状态进行资源匹配                    调度策略与执行机制分离

  `ProcessSupervisor`     spawn、日志捕获、进程组、退出码、取消和恢复          子进程必须降权为提交用户

  `LogStreamer`           日志落盘、轮转、`logs -f` 订阅分发                  不解析日志内容；慢客户端策略有界

  `StateStore`            SQLite 持久化 Job 和资源状态                       daemon 重启后作为恢复依据

  `Executor`              内部并发、阻塞 I/O、事件传递和任务协调             不知道 GPU、训练框架和调度策略

  `Heyaki`                未来多节点 RPC/Event/Stream 通信                   MVP 本机 IPC 不依赖 Heyaki
  -----------------------------------------------------------------------------------------------------------------

------------------------------------------------------------------------

## 5. 多用户与权限模型

推荐 MVP 使用 systemd 启动一个系统级 `yorid`。

第一版可以让 `yorid` 以 root
启动，以便可靠地为任意提交用户创建子进程，但 root 权限必须严格限制在
daemon 自身以及进程身份切换阶段。

训练进程必须在 `exec` 前切换为原提交用户。

``` text
/run/yori/yori.sock
    owner: root
    group: yori
    mode : 0660

连接建立
   ↓
SO_PEERCRED
   ↓
获得真实 pid / uid / gid
   ↓
Job.owner_uid = peer.uid
   ↓
Scheduler 分配 GPU
   ↓
fork / spawn
   ↓
initgroups + setgid + setuid
   ↓
exec 用户训练命令
```

建议建立 `yori` 系统组，仅组内用户可以连接 socket（连接准入由文件权限
承担，主组与补充组均可；管理员判定与端点治理细节冻结于
[DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md)）。

客户端不得提供可信的"目标 UID"。daemon 必须使用 Linux `SO_PEERCRED`
等内核机制确定连接用户身份。

长期版本可以进一步拆分：

``` text
yorid
(unprivileged)
     │
     ▼
yori-launch-helper
(privileged, minimal)
     │
     ▼
用户训练进程
```

这样可以显著缩小 root 攻击面。

------------------------------------------------------------------------

## 6. Job 模型与状态机

### 6.1 JobSpec

``` text
JobSpec
  owner_uid / owner_gid       # daemon 从 SO_PEERCRED 确定
  argv                        # 原始命令参数
  cwd                         # 提交时工作目录
  env                         # 白名单继承 + 显式覆盖
  gpu_request                 # MVP 默认 1 GPU
  launch_profile              # 可选项目/框架适配器
  tensorboard_logdir          # 可选；相对 cwd 的指标目录，供 yori tensorboard 使用
  priority                    # 后续扩展
  submit_time
```

M1 冻结的 C++20 契约位于 `include/yori/job/job.hpp`：`JobId` 是非零
`uint64_t` 强类型；`owner_uid == 0` 被拒绝，保证 root 提交不会进入可执行 Job。
M5 从 IPC 建立 `JobSpec` 时，owner 字段只能取自 `SO_PEERCRED`，客户端同名字段
不构成可信输入。`gpu_request` 在 MVP 必须为 1，`submit_time` 必须晚于 Unix epoch，
以便 `DEC-005` 的 `(submit_time, JobId)` 排序稳定。

所有可变长字段在进入 Job 前校验：

| 字段 | M1 上限 / 规则 |
| --- | --- |
| `argv` | 1..256 项；单项 16 KiB；总计 64 KiB；`argv[0]` 非空；不得含 NUL |
| `cwd` | 绝对路径；1..4096 bytes；不得含 NUL |
| `env` | 最多 256 项；name 最多 255 bytes 且非空、不含 `=`/NUL；value 最多 32 KiB；name+value 总计 256 KiB |
| `launch_profile` | 可选；存在时 1..128 bytes；不得含 NUL |
| `tensorboard_logdir` | 可选；存在时为 cwd 下相对路径，1..4096 bytes，不得含 NUL、绝对路径或 `..` 路径分量 |

校验拒绝通过 `JobSpecValidationResult` 返回稳定错误码和可选条目下标；Job 创建
失败通过 `JobCreationError` 区分无效 JobId 与无效 JobSpec，不以截断或静默修复
接受输入。

### 6.2 状态机

``` text
QUEUED
   ├────────────► STARTING ───► RUNNING ───► FINISHED
   │                  │             │
   │                  ├─────────────┼──────► FAILED
   │                  ├─────────────┼──────► LOST
   │                  └─ cancel ────┴──────► STOPPING ───► CANCELLED
   │                                                ├────► FAILED
   │                                                └────► LOST
   └─ queued cancel ─────────────────────────────────────► CANCELLED
```

`FINISHED`、`FAILED`、`CANCELLED`、`LOST` 是终态。每个合法转换都是 O(1) 的
单步推进并使 `revision` 加一。转换返回 `TransitionResult`：合法推进为 `APPLIED`；
活动状态的非法边为 `REJECTED`；对同一终态的重复结果为
`IDEMPOTENT_TERMINAL`；终态后的不同迟到结果为 `IGNORED_AFTER_TERMINAL`。
后三者均不改变状态或 revision，使上层能够记录拒绝与丢弃事件，同时保证终态
Job 不复活。

未来如果加入自动重试，可以增加：

``` text
RETRY_WAIT
```

daemon 重启时，对数据库中 `RUNNING` / `STARTING` 的 Job 进入恢复流程：

1.  核验 PID 是否存在。
2.  核验 PID 是否仍然属于原训练进程。
3.  核验 GPU 是否仍被该进程占用。
4.  原训练仍存活则重新建立监控关系。
5.  无法确认时标记为 `LOST`。

**绝不能因为 daemon 重启就直接重新启动数据库中的 RUNNING Job。**

M4 冻结的恢复 Core 位于 `include/yori/recovery/job_recovery.hpp`
（`JobRecovery`，同步、单 owner、无内部线程；Executor 承载与启动编排随
daemon 总装落地）。决策输入为 `StateStore` 一致快照，核验依据是 M2 冻结的
进程身份三元组（`/proc/<pid>/stat` 的 PGID 与启动 ticks，`RULE-06`；GPU 侧
一致性由启动序列的重新观测与 lease 事实分列保证，见第 7 节）：

| 快照状态 | 身份核验结果 | 恢复决策 |
| --- | --- | --- |
| `QUEUED` | 不适用 | 经 `GlobalJobQueue::restore()` 重新入队 |
| `STARTING`（已记录身份） | 通过 | 提升 `RUNNING`（exec 已在崩溃前确认），lease 保留 |
| `STARTING`（无身份，崩溃窗口） | 不适用 | `LOST` 并释放 lease；残留进程由外部占用检测标记 `EXTERNAL_BUSY` 兜底 |
| `RUNNING` | 通过 | 采纳：保持 `RUNNING`，lease 保留（DEC-008 第 2 条） |
| `STOPPING` | 通过 | 保持 `STOPPING` 并显式标记"需要重发取消"（宽限截止已丢失，重发 SIGTERM + 重建宽限由守护层执行） |
| `STARTING/RUNNING/STOPPING` | 进程消失 / PGID 或启动 ticks 不符（PID reuse） | `LOST` 并释放 lease，`failure_reason` 记录稳定原因 |
| 终态 | 不适用 | 不动（历史事实） |

`LOST` 转换与其 lease 释放位于同一 mutation（lease 矩阵约束）；活动 Job 数量
超过单 mutation 条目上限时按 `kMaxEntries` 分块、以递增 revision 链接，部分
分块失败后已落盘的终态保持幂等，可安全重入恢复。恢复完成后以
`kRecoveryCompleted` 触发调度（第 9 节）。

------------------------------------------------------------------------

## 7. GPU 资源模型

建议 GPU 使用以下逻辑状态：

``` text
FREE
ALLOCATED
EXTERNAL_BUSY
UNAVAILABLE
```

含义：

-   `FREE`：可以被调度。
-   `ALLOCATED`：已经被 Yori lease 给某个 Job。
-   `EXTERNAL_BUSY`：存在非 Yori
    管理的外部计算进程，或按策略判定当前不可调度。
-   `UNAVAILABLE`：GPU 故障、管理员禁用或其他不可调度状态。

内部资源身份建议优先保存稳定的 **GPU UUID**，而不是仅保存可能变化的 GPU
index。

GPU 利用率只能作为遥测数据，不能作为 ownership。

例如强化学习训练可能出现：

``` text
GPU training
    ↓
rollout / simulation
    ↓
GPU utilization 暂时降低
    ↓
继续 training
```

此时 Yori 不能因为瞬时 utilization 为 0 就释放 GPU。

因此：

> **Yori lease 是调度事实，NVML 是资源观测。**

对于 daemon 启动前已经存在的外部训练进程，可以通过 NVML 检测并将对应 GPU
标记为 `EXTERNAL_BUSY`，但 Yori 不主动接管或终止这些进程。

M1 冻结的 `GpuProvider` SPI 位于 `include/yori/gpu/gpu_provider.hpp`。Provider
一次同步 `observe()` 只返回 `GpuObservationSnapshot`：

- `GpuObservedState` 只有 `FREE / EXTERNAL_BUSY / UNAVAILABLE`，不包含
  `ALLOCATED`；最多 128 台设备，GPU UUID 为 1..96 bytes，UUID 与 index 在同一
  snapshot 中唯一。
- snapshot revision 非零、观测时间晚于 Unix epoch；utilization 若存在则为
  0..100，显存 used 必须同时带 total 且不超过 total。
- `GpuLease {GPU UUID, JobId}` 是 Core 事实。逻辑状态先看 lease：有 lease 为
  `ALLOCATED`；无 lease 时才映射 Provider 观测。原始观测始终保留，以便独立诊断
  lease 与 `EXTERNAL_BUSY/UNAVAILABLE` 的冲突。
- `observe()` 的 backend unavailable、permission denied 和 observation failed
  均为显式错误。周期采样与任务生命周期由外部 Executor owner 承载，Provider
  不得隐藏 timer、worker 或队列。

M1 的 `FakeGpuProvider` 是单 owner、进程内测试实现：状态替换先完整校验，失败
不覆盖上一份有效快照，并可显式注入 Provider 错误。它不安装为产品 API；M3 的
NVML adapter 实现上述同一 SPI。

M3 落地的 `NvmlGpuProvider`（`include/yori/gpu/nvml_gpu_provider.hpp`）是 Linux
Adapter 层实现，附加语义：

- 运行期 `dlopen` 绑定（默认 `libnvidia-ml.so.1`，库路径只接受管理员配置或
  测试注入），显式 `load()`（`dlsym` + `nvmlInit_v2`）成功后 `observe()` 才可用；
  构建不依赖 CUDA/NVML SDK，NVML 类型不出现在公共 API（`RULE-02`）。
- 外部占用以计算进程计数判定：`nvmlDeviceGetComputeRunningProcesses_v3`
  （旧驱动回退 `_v2`）以 count-only 查询（null 缓冲），计数 > 0 即
  `EXTERNAL_BUSY`；不读取任何进程条目字段，不采集外部进程身份。
- 占用查询 `NOT_SUPPORTED`/`GPU_IS_LOST`/超时类错误映射设备 `UNAVAILABLE`
  （按策略不可调度，宁可保守）；`NO_PERMISSION` 为 Provider 级
  `kPermissionDenied`；驱动未装载/库缺失/未初始化为 `kBackendUnavailable`。
- 遥测失败（utilization/显存查询错误或数值越界不可信）只省略对应 optional
  字段；设备 UUID 不可读或设备数超出快照上限时整次观测 `kObservationFailed`，
  不产出部分设备快照（防止设备群身份静默缩编导致 lease 悬空）。
- revision 为进程内原子单调递增；NVML 本身线程安全，`observe()` 可并发调用，
  析构由 owner 保证无并发观测（见 GpuManager 停止顺序）。

`GpuManager`（`yori_runtime` 内部组件，`EXEC-05`/`EXEC-09`）承载周期采样：
start 先同步完成首次观测建立基线（首次观测不产生迁移事件），再以
`submit_periodic` + `TimerHandle` 周期采样；快照经 `DoubleBuffer` 发布，
观测状态迁移（设备出现/消失/状态翻转）向有界事件通道投递调度触发事件，
遥测-only 波动不触发（utilization 不是 ownership）；provider 错误记入 streak
统计并在开始/结束时显式通知；事件通道满时显式计数并在下一条事件补投标记；
stop 取消 `TimerHandle` 并有界 join 在途采样（超时显式失败，不静默）。

------------------------------------------------------------------------

## 8. 训练启动与 GPU 适配

Yori 的基本原则：

> **Scheduler 拥有物理 GPU 选择权，训练程序只面对进程内逻辑 GPU。**

对于标准 CUDA / PyTorch 项目，优先通过 `CUDA_VISIBLE_DEVICES` 完成 GPU
映射。

用户：

``` bash
yori submit --gpus 1 -- \
    python train.py --task humanoid --device cuda:0
```

Scheduler 分配物理 GPU 3：

``` bash
CUDA_VISIBLE_DEVICES=3 \
python train.py --task humanoid --device cuda:0
```

此时训练程序内部：

``` text
cuda:0 -> physical GPU 3
```

### 8.1 不同项目指定 GPU 的差异

部分旧项目可能使用：

``` bash
python train.py --gpu 3
```

或者：

``` bash
./train --accelerator 3
```

Yori 不应该解析训练源码。

应提供可配置的 `LaunchAdapter` / `LaunchProfile`：

``` yaml
profiles:
  isaaclab:
    gpu_mode: cuda_visible_devices
    logical_device: "cuda:0"

  legacy_rl:
    gpu_mode: physical_argument
    argument: "--gpu"
```

也可以支持显式模板：

``` text
{logical_gpu}
{physical_gpu}
```

但应鼓励新项目统一采用：

``` text
CUDA_VISIBLE_DEVICES + cuda:0..N-1
```

这样训练程序完全不需要知道服务器物理 GPU 编号。

M2 冻结的公开契约位于 `include/yori/launch/launch_adapter.hpp`：

- `LaunchProfile` 两种模式：`cuda_visible_devices`（设置
  `CUDA_VISIBLE_DEVICES=<NVML 物理索引>` 与 `CUDA_DEVICE_ORDER=PCI_BUS_ID`，不改
  argv）和 `physical_argument`（把 `<argument> <物理索引>` 追加到 argv，不设置 CUDA
  变量，遗留程序自行解释编号）。参数名必须以 `-` 开头、无空白、有界。
- `DefaultLaunchAdapter::prepare()` 按 DEC-006 三层合并生成 `LaunchPlan`：最终
  argv、按键字典序唯一的环境条目、cwd、目标身份（uid/gid/username）与 fork 前已
  解析的 supplementary groups。`LaunchPlan` 经独立复验（空 argv、绝对 cwd、环境
  数量/尺寸上限、uid/gid 非 0）后才允许进入 spawn。
- 提交用户身份由 `IdentityResolver`（默认 `PosixIdentityResolver`，`getpwuid_r` +
  `getgrouplist`）在 fork 前解析；子进程内只执行 `setgroups -> setgid -> setuid`
  三个 syscall 封装，保持 fork-exec 窗口的 async-signal-safe 纪律。

------------------------------------------------------------------------

## 9. 调度流程

典型生命周期：

``` text
yori submit
    ↓
创建 Job
    ↓
QUEUED
    ↓
Scheduler 被唤醒
    ↓
查找满足要求的 FREE GPU
    ↓
建立 GPU lease
    ↓
STARTING
    ↓
构造 env / CUDA_VISIBLE_DEVICES
    ↓
切换 UID/GID
    ↓
启动训练进程
    ↓
RUNNING
    ↓
ProcessSupervisor 监控
    ↓
进程退出
    ↓
FINISHED / FAILED
    ↓
释放 GPU lease
    ↓
Scheduler 再次运行
    ↓
启动下一个 Job
```

调度器应采用事件驱动方式，而不是依赖高频轮询。

典型调度触发事件包括：

-   新 Job 提交。
-   Job 结束。
-   Job 被取消。
-   GPU 外部占用状态变化。
-   daemon 恢复完成。
-   管理员修改资源状态。

M1-04 冻结的服务器级队列契约位于
`include/yori/queue/job_queue.hpp`。`GlobalJobQueue` 是 `yorid` 单 owner 持有的
同步 Core 派生索引，不是 Executor 任务队列，也不按用户拆分：

- 队列仅保存 `(submit_time, JobId)`，不复制 JobSpec；严格按该二元组升序，
  同一提交时间由 JobId 稳定决胜。默认容量 1024，配置硬上限 4096，零容量或
  超上限配置无法创建队列。
- `admit()` 只接受通过完整校验的 `QUEUED/revision=0` Job；无效 ID/Spec/状态、
  重复 Job 和容量耗尽均返回稳定 `QueueErrorCode`，不会修改队列。
- 每次准入、移除或恢复尝试都返回 `QueueOperationResult` 和结构化 `QueueEvent`，
  事件包含操作类别、拒绝原因、JobId（若适用）、操作前后大小与容量。事件由上层
  在整个状态操作成功后投递；队列内部不隐藏回调、事件缓冲、线程或重试。
- `restore(StateSnapshot)` 以 StateStore 同一 revision 快照原子重建队列，只纳入
  `QUEUED` Job；`STARTING/RUNNING/STOPPING` 与终态 Job 不会重新入队。恢复会重新
  排序并校验 Job、重复 ID、revision 与容量，任一失败都保留原队列。

StateStore 仍是持久化权威。M1-05 的 JobManager/Scheduler 在同一个 Executor
串行有限任务中协调队列修改与 StateStore mutation，并仅在持久化操作成功后发布
候选队列事件；daemon 重启时始终通过 `load()` + `restore()` 重建派生索引。

M1-05 的 `FifoScheduler::run_once()` 将每个调度事件实现为一个有界推进单元：

1. 校验最新 GPU observation snapshot，读取唯一全局队首和 StateStore 一致快照；
   每次核对 Store 中 `QUEUED` 数量和全局最早排序键，防止派生队列漏项后绕过
   更早 Job；队列为空、观测无效、存储失败与任意分歧均返回结构化结果。
2. 只考察队首 Job。没有未被 lease 的 `FREE` GPU 时返回 `HEAD_BLOCKED`，不查看或
   启动后续 Job；有多个候选时按当前 physical index 升序选择，lease 仍保存稳定
   GPU UUID。
3. 单次调用最多推进一个 Job。先从派生队列暂时移除队首，再以一个 StateStore
   mutation 原子提交 `QUEUED -> STARTING` 和 GPU lease；写入失败时按稳定排序键
   原子回滚队列，不静默重试。成功结果携带 JobId、GPU UUID 与新 store revision。

触发类别固定为新提交、Job 退出、Job 取消、GPU 状态变化、恢复完成和管理员状态
变化；调用本身不包含循环或定时器。进程私有 `SchedulerTaskRunner` 使用 Executor
`submit_cancellable()` 承载该有限单元，始终保留 task handle 与 future：前一结果
未消费时新触发明确返回 `BUSY`，停止生产后返回 `NOT_ACCEPTING`，排队取消通过
`request_task_cancel()` 进入 Executor 生命周期，future 的完成、取消、总量准入拒绝
与异常均映射为显式结果。运行中取消只在进入有界 Core 单元前协作检查，不抢占已经
开始的 StateStore 原子操作。M1-06 再用 Executor comm 组件承接并合并并发触发，
本适配器不自建事件队列、锁或线程。

------------------------------------------------------------------------

## 10. 进程守护

### 10.1 yorid 守护

`yorid` 由 systemd 管理：

``` ini
[Unit]
Description=Yori GPU Scheduler

[Service]
Type=simple
ExecStart=/usr/local/bin/yorid
Restart=on-failure
RuntimeDirectory=yori

[Install]
WantedBy=multi-user.target
```

现代 Linux 下不需要自行实现传统 double-fork daemonization。

### 10.2 训练进程守护

训练进程由 `ProcessSupervisor` 管理，与提交 CLI 和 SSH 会话解耦。

每个 Job 建议拥有独立的：

``` text
process group / session
```

取消任务：

``` text
SIGTERM
   ↓
grace period（默认 10 秒，DEC-007）
   ↓
仍未退出
   ↓
SIGKILL
```

需要记录：

``` text
PID
PGID
start time / process identity
```

避免 daemon 恢复时因为 PID reuse 错误接管其他进程。

Linux 上后续可以考虑使用 `pidfd` 增强进程生命周期管理。

M2 冻结的公开契约位于 `include/yori/process/process_supervisor.hpp`：

- `ProcessSupervisor` 是单 owner、单进程的同步状态机
  （`IDLE -> RUNNING -> TERMINATING -> EXITED`），无内部线程；异步承载由
  Executor 适配层（`yori_runtime`）驱动。取消是 Yori 的外部进程语义，不与
  Executor 任务取消混同。
- `spawn(LaunchPlan)`：双端 `setpgid` 建立独立进程组；stdout/stderr 经管道捕获；
  子进程在 exec 前将 `SIGPIPE` 置为忽略（DEC-008，daemon 退出不击杀训练）、
  完成 `setgroups -> setgid -> setuid` 降权（目标即当前有效身份时为幂等空操
  作）、`close_range` 收敛继承描述符；PATH 解析与 execve 失败经专用报告管道
  回传，父进程在确认 exec 成功前不返回。`/proc/<pid>/stat` 的启动 ticks 与
  PGID 构成 `ProcessIdentity`，是恢复与 PID reuse 核验依据。
- `request_cancel()` 对整个进程组发送 SIGTERM 并记录宽限截止（steady_clock）；
  `grace_expired()` 与 `escalate()` 完成升级；宽限未到时升级被显式拒绝，进程
  已退出时升级为空操作（终态幂等）。
- `poll_exit()` 以 `waitpid(WNOHANG)` 有界推进一步，`consume_exit()` 终态幂等；
  退出状态区分正常退出（携带退出码）与信号终止（携带信号号）。析构对仍在运行
  的进程组执行 SIGKILL 并有界回收，保证不留孤儿与僵尸。
- Executor 承载（总计划 `EXEC-07`）：`ProcessExitMonitor` 以单个 blocking
  worker 安装 SIGCHLD 自管道唤醒，按注册 PID 逐个核验身份后 `waitpid(WNOHANG)`
  回收并投递 `ExitEvent`；注册/注销命令与事件均走有界 `MpscChannel`，事件容量
  满时 worker 有界重试并经 comm 统计可见。宽限升级经 `submit_delayed` 一次性
  任务承载（`GraceEscalation`，arm/disarm 显式消费 future）。

------------------------------------------------------------------------

## 11. 训练状态观察（日志与 TensorBoard）

引入 Yori 后，训练从用户的交互会话（tmux/SSH）转移到后台 daemon。用户原本依赖的两个
观察入口——训练日志和 TensorBoard——必须由系统提供等价或更好的替代，否则用户没有
理由放弃手动占卡。

观察面由三部分组成：

``` text
yori ps / yori queue / yori gpu    # 状态观察：Job、队列、GPU
yori logs [-f] <job-id>            # 日志观察：stdout/stderr 快照与实时跟随
yori tensorboard <job-id>          # 指标观察：以提交用户身份拉起 TensorBoard
```

### 11.1 观察面设计原则

1.  **Yori 不理解训练语义**。不解析日志内容、不读取或解释 TensorBoard event
    文件、不提取 loss/reward 等指标。Yori 只负责：捕获字节流、落盘、传输流、
    托管观察入口。指标可视化完全交给 TensorBoard 本身。
2.  **观察不得影响被观察者**。日志跟随、TensorBoard、`ps` 查询的失败或退出
    不得影响训练进程；慢观察客户端不得拖垮 daemon。
3.  **一切有界**。日志文件有大小上限与轮转；跟随会话有数量上限；每个订阅者
    的发送缓冲有容量上限，溢出策略显式（见 11.4）。
4.  **权限与多用户隔离**。观察接口与日志同样执行 owner/admin 授权（见 11.5）。

### 11.2 日志捕获与落盘

`ProcessSupervisor` 以管道捕获子进程的 `stdout` 与 `stderr`，交给 `LogStreamer`
分两路处理：

``` text
训练进程 stdout ─┐
                 ├─► 管道 ─► LogStreamer ─┬─► 落盘 <job_dir>/stdout.log
训练进程 stderr ─┘                        └─► 订阅分发（logs -f）
```

每个 Job 拥有独立日志目录：

``` text
/var/lib/yori/jobs/<job-id>/
    stdout.log
    stderr.log
```

落盘规则：

-   文件 owner 为提交用户、group 为 `yori`、mode `0640`；owner 可绕过 IPC 直接
    `tail -f` 自己的日志文件。M2 落地为 `LogSink`（`include/yori/observe/log_sink.hpp`）：
    打开即对两个流应用 `0640` 与属主（root 环境经 `fchmod`/`fchown`；非 root 测试
    环境显式降级为仅权限位），单文件大小上限（默认 256 MiB，配置范围 4 KiB ~
    1 GiB）与轮转保留数（默认 1 个历史文件，上限 4）。
-   每个流有单文件大小上限（默认 256 MiB）与轮转保留数（默认 1 个历史文件，
    即 `stdout.log` + `stdout.log.1`）；磁盘占用存在全局预算。轮转对跟随会话
    透明：分发基于“逻辑 offset（已写入字节序号）”，轮转后按新文件内 offset
    继续投递，不重发、不丢失标记。M2 实现中逻辑 offset 只计接受字节、跨轮转
    单调递增，单个超过上限的块自动跨轮转分块写入；既有文件原位追加
    （`O_APPEND|O_NOFOLLOW`），daemon 重启后 offset 从文件大小继续（DEC-008）。
-   训练进程大量写出导致落盘跟不上时，管道读端必须持续排空：优先保证训练
    不因管道写阻塞而停顿，超出保留策略的日志内容允许丢弃，并在流中插入
    drop 标记帧（含丢弃字节数）。M2 的承载为 `LogPump`（单个 blocking
    worker，`poll` 排空多 Job 管道并同步写 `LogSink`）；写失败丢弃数据并
    在下次成功写入前插入 `[yori] dropped N bytes` 标记，标记不计入逻辑
    offset。停止日志泵只关闭读端，训练进程的后续写获得 `EPIPE`（SIGPIPE 已
    忽略，DEC-008），不终止进程。

### 11.3 `yori logs`：快照与跟随

``` bash
yori logs <job-id>                 # 输出当前日志尾部（stdout+stderr 合并视图）
yori logs -f <job-id>              # 跟随输出，直到 Job 终态且流排空后退出
yori logs --stdout <job-id>        # 仅 stdout
yori logs --since-offset N <job-id>  # 从逻辑 offset N 开始（断线重连）
```

MVP 即提供 `-f`。实现路径：

-   daemon 侧 `LogStreamer` 将管道读到的每个数据块同时落盘并发布到该 Job 的
    日志广播通道；IPC 层为每个 `logs -f` 会话建立订阅，把块封装为流式帧经
    Unix Domain Socket 推送给 CLI。
-   CLI 断开或 Ctrl-C 只结束观察会话，对训练进程零影响。
-   断线重连：每帧携带逻辑 offset，客户端记录已收字节，重连时以
    `--since-offset` 续传；轮转导致的历史不可回放部分以明确的 gap 帧告知。
-   Job 进入终态后，`-f` 在排空剩余日志后正常退出，退出码与 Job 终态对齐。

### 11.4 慢客户端与背压策略

每个 `logs -f` 订阅者有独立的固定容量发送队列（有界）。溢出时策略固定为：

``` text
订阅队列满
   ↓
断开该订阅，返回明确的 BACKPRESSURE 错误帧（含当前 offset）
   ↓
客户端提示并支持一键以 --since-offset 重连
```

不做静默丢弃、不做无界缓存、不阻塞 LogStreamer 主路径。活跃跟随会话数有
上限（默认每 Job 8、全局 64），超出的订阅请求被拒绝并返回明确错误。

### 11.5 权限与信息隔离

-   Job owner 可以查看、跟随自己的日志与状态。
-   管理员（`yori` 管理组）可以查看全部 Job 的日志与状态。
-   普通用户默认不能查看其他用户 Job 的日志、`tensorboard_logdir` 等敏感字段；
    `ps`/`queue`/`gpu` 展示的非自有 Job 信息默认脱敏（仅 JobId、状态、资源占用，
    不含 argv、cwd、日志内容）。
-   授权判定在 daemon 侧基于 `SO_PEERCRED` 身份执行，客户端声明不可信。

### 11.6 TensorBoard 观察接口

用户的指标可视化习惯是 TensorBoard。Yori 的定位是**托管入口**，不是指标系统：

> Yori 负责“在哪里、以谁的身份、监听哪里”三件事；TensorBoard 读什么、怎么画，
> 是训练程序与 TensorBoard 之间的事。

``` bash
yori tensorboard <job-id>                     # 自动解析 logdir + 自动选端口
yori tensorboard <job-id> --logdir runs/exp1  # 显式指定（相对 Job cwd）
yori tensorboard <job-id> --port 6006         # 显式指定端口
yori tensorboard <job-id> --host 0.0.0.0      # 显式对局域网开放（默认 127.0.0.1）
```

行为定义：

-   **logdir 解析**：优先 `--logdir` 参数，其次 JobSpec 的 `tensorboard_logdir`
    （提交时 `yori submit --tensorboard-logdir runs/ ...` 记录），再次回退 Job
    `cwd`。daemon 仅通过 IPC 查询接口向 owner/admin 返回解析结果。
-   **进程身份**：`yori tensorboard` 由用户在自己的会话中运行，CLI 直接以当前
    用户身份 spawn `tensorboard --logdir <dir> --port <p> --host 127.0.0.1`，
    前台运行，Ctrl-C 即停止。无需 daemon 特权参与。
-   **网络边界**：默认只监听 `127.0.0.1`，用户经 SSH 端口转发访问；绑定更大
    范围必须显式 `--host`。避免用户训练曲线默认暴露给整台服务器的所有网络
    邻居。
-   **端口分配**：默认让 OS 分配空闲端口（bind 端口 0），CLI 打印最终访问
    URL；`--port` 指定时冲突则报错退出。
-   **生命周期**：TensorBoard 进程属于用户的观察会话，不是受调度的 workload——
    不占 GPU lease、不进入全局队列、CLI 退出即终止。用户断开 SSH 想保留常驻
    TensorBoard 时，自行配合 `tmux`/`nohup` 使用。

**为什么不放进 yorid**：daemon 托管 TensorBoard 意味着要为“非训练、可能长期
存活、按用户隔离端口”的一类进程建立第二种进程模型、端口资源表与恢复语义，
MVP 收益低、攻击面高。CLI 侧拉起把复杂度留在用户会话内，daemon 只暴露一个
只读查询。若未来出现“常驻指标面板”需求，再演进为 daemon 管理的辅助进程类型
（届时作为独立设计决策记录）。

### 11.7 Executor 承载

观察路径全部运行在 Executor 生命周期内：

``` text
IPC 连接等待 / logs -f 会话        blocking worker（IBlockingIoWorker + wakeup）
管道读取（LogStreamer log pump）    blocking worker，wakeup 关闭管道读端
日志块订阅分发                     executor::comm::Topic<LogChunk>（每 Job 一个）
Job/GPU 状态快照（ps/gpu 查询）    DoubleBuffer 一致快照
订阅准入计数、BACKPRESSURE 事件    Executor 监控/失败事件可见
```

`Topic` 的每订阅者队列容量即 11.4 的有界发送缓冲；溢出事件经 comm 统计与
回调进入服务监控，不静默吞掉。daemon 关闭时按 AGENTS.md 关闭顺序先停止 IPC
生产者与跟随会话，再回收 blocking worker。

------------------------------------------------------------------------

## 12. 持久化

MVP 推荐 SQLite。

至少持久化：

``` text
JobId
owner_uid
owner_gid
argv
cwd
必要 env
launch_profile

requested_gpus
assigned_gpu_uuid
assigned_gpu_index

tensorboard_logdir

status
submit_time
start_time
end_time

pid
pgid
exit_code
failure_reason

log_path
```

SQLite 的主要目的不是让多个 scheduler 同时操作队列。

Yori 始终只有一个 authoritative daemon。

SQLite 用于：

-   daemon 重启恢复。
-   Job 历史查询。
-   状态持久化。
-   审计。
-   故障诊断。

M1 冻结的 `StateStore` SPI 位于 `include/yori/store/state_store.hpp`，使用两个
操作：`load()` 返回同一 store revision 下的 Job 与 GPU lease 一致快照；
`apply()` 原子应用 `StateMutation`。mutation 最多 64 条，包含 Job create/update、
lease acquire/release，并携带 `expected_revision`：

- store revision 不匹配返回 `REVISION_CONFLICT`；成功 mutation 只增加一次
  store revision。
- 初始 Job 必须为 `QUEUED/revision=0`；更新必须保持 JobSpec 不变、Job revision
  恰加一且遵守第 6.2 节转换矩阵。
- `STARTING/RUNNING/STOPPING` Job 必须恰有一个 lease；`QUEUED` 与终态 Job
  不得有 lease；GPU 与 Job 两侧均为一对一。
- 任一条目无效、容量耗尽或 backend 失败时返回稳定错误码，整个 mutation 不得
  留下部分写入。异步写入、FIFO 串行化与有界 admission 由 `EXEC-08` 的外部
  Executor 路径（`StoreTaskRunner`）负责，不在 adapter 内另建线程或写队列。

M4 扩展了 `StoredJob` 的执行记录（`JobExecutionRecord`）：进程身份三元组
（`ProcessIdentity`：pid/pgid/start_ticks）、`start_time`/`end_time`、退出状态
（`ExitStatus`）、`failure_reason`（1..4096 bytes，仅终态）与 `log_path`
（绝对路径，1..4096 bytes，`QUEUED` 禁止）。结构校验由
`validate_execution()` 固化：身份要么全零（未记录）要么三元组有效；
`QUEUED` 禁止携带身份，`RUNNING/STOPPING` 必须携带（到达路径必经 exec 确认），
`STARTING` 与终态可选（spawn 前窗口/审计事实）；`exit`/`end_time` 仅终态可
携带；`start_time` 仅身份存在时不早于 `submit_time`。违规 mutation 以
`kInvalidExecutionRecord` 拒绝。两个后端的接受/拒绝决策共享
`src/store/mutation_core.cpp` 验证核心，保证同语义。

`InMemoryStateStore` 是容量显式、单 owner 的 M1 测试实现，通过副本校验后一次
提交保证原子性；`SqliteStateStore`（M4，`include/yori/store/sqlite_state_store.hpp`，
选型见 [DEC-009](../decisions/DEC-009-sqlite-state-store.md)）是唯一生产实现：
运行期 `dlopen` 绑定系统 `libsqlite3.so.0`（构建不依赖 libsqlite3-dev，库路径
只接受管理员配置或测试注入），显式 `open()` 完成符号解析、幂等 schema 初始化
（`schema_version=1`：`yori_meta`/`yori_jobs`/`yori_leases`，argv/env 以
LE32 length-prefixed blob 编码、时间以 Unix epoch 纳秒编码），数据库文件为
符号链接时拒绝打开，打开后尽力收敛权限 `0600`。`apply()` 以
`BEGIN IMMEDIATE` 单事务完成"读 revision -> 内存验证 -> 写入 -> revision
递增"，任何失败显式回滚不留部分写入；busy/locked/损坏文件/SQL 失败映射
`kBackendUnavailable`，行数据篡改（非法状态、revision 与状态矛盾、编码越界、
lease 矩阵破坏）使 `load()` 以稳定错误码显式失败而非静默跳过。两个 SPI 的
公开头只使用 C++20 标准库与 Yori Core 类型，不暴露 NVML、SQLite 或 Executor
类型。

------------------------------------------------------------------------

## 13. IPC 与 CLI

MVP 使用 Unix Domain Socket：

``` text
/run/yori/yori.sock
```

相比本机 TCP，它能够天然利用 Unix 文件权限，并通过 `SO_PEERCRED`
获得连接用户身份。

### 13.1 初始 CLI

``` bash
yori submit --gpus 1 [--tensorboard-logdir runs/] -- python train.py ...
yori ps
yori queue
yori logs <job-id>
yori logs -f <job-id>
yori tensorboard <job-id>
yori cancel <job-id>
yori gpu
```

`submit` 成功后返回 JobId：

``` text
Submitted job 42
```

随后 CLI 即可退出。

训练任务属于 `yorid`，而不是该 CLI 进程。唯一的例外是 `yori tensorboard`：
它启动的 TensorBoard 进程属于当前观察会话（见第 11.6 节），随 CLI 退出而终止。

未来可以增加：

``` bash
yori ps --mine
yori ps --all

yori logs --since-offset N <job-id>   # 断线续传增强（协议 MVP 已支持基础形态）

yori submit --priority high ...
yori submit --gpus 2 ...
```

### 13.2 IPC 协议要点

MVP 的 IPC 消息分为两类：

-   **请求/响应**：submit、ps、queue、gpu、cancel、job 查询（含 tensorboard
    logdir 解析）。一次性、有界。
-   **流式帧**：`logs -f` 的日志块推送。每帧携带 JobId、流标识（stdout/stderr）、
    逻辑 offset 与数据；控制帧包括 BACKPRESSURE、GAP（轮转丢弃标记）、EOF
    （终态排空）。帧格式采用长度前缀，全部输入经边界校验与 fuzz 覆盖（见第
    17 节）。

流式会话数量与每会话缓冲容量在 daemon 配置中有明确上限，拒绝与溢出返回
显式错误帧，不静默重试。

### 13.3 协议 v1（M5 冻结）

M5 落地请求/响应族协议 v1，公开契约位于 `include/yori/ipc/`：

-   **帧格式**（两个方向一致）：`[u32 LE payload_bytes][payload]`，
    payload = `[u8 version=1][u8 kind][kind body]`。负载上限 1 MiB、单字符串
    64 KiB、请求内计数 256、列表 1024、日志尾部每流 256 KiB（协议级常量
    `IpcProtocolLimits`）。
-   **请求 kind**：`SUBMIT`/`PS`/`QUEUE`/`GPU`/`CANCEL`/`LOGS`（快照）。
    请求结构不携带任何身份字段——Job owner 只来自 `SO_PEERCRED`（第 5 节、
    DEC-010），客户端无从声明目标 UID。
-   **响应**：统一错误码（NONE/PROTOCOL/UNSUPPORTED/DENIED/INVALID_SPEC/
    QUEUE_REJECTED/STORE_FAILED/NOT_FOUND/INVALID_STATE/NOT_AVAILABLE/LIMIT/
    INTERNAL）+ detail + 按 kind 的结果体；错误响应携带可用上下文（如
    CANCEL 拒绝时的当前状态）。
-   **解析纪律**：解码器只产生稳定错误码（截断/超限/坏版本/坏 kind/坏
    字符串/计数超限/尾部字节/值域非法）或完整解析；字符串禁止 NUL；全部
    输入必须精确消费；语义校验（JobSpec 精确限制）由 daemon 侧
    `job::validate` 承担，协议层只管结构安全。fuzz 集从 M5 起纳入回归
    （第 17 节第 9 条）。

### 13.4 传输承载与端点治理（M5）

-   `IpcTransport` 抽象（`ipc_transport.hpp`）：daemon 侧
    `IpcRequestHandler`（在 UDS blocking worker 内被串行调用）+
    `IpcServerTransport`/`IpcClientTransport`。
-   UDS 服务端（EXEC-02）：单个 Executor blocking worker 以 poll 等待
    listen fd 与唤醒管道，串行 accept -> `SO_PEERCRED` -> 读一帧 ->
    handler -> 写回一帧 -> 关闭。每连接请求总预算（默认 5s）到期即断开
    （慢客户端有界化）；帧界违规回 PROTOCOL 错误帧后断开；handler 异常
    映射 INTERNAL 响应（不吞、不崩）。
-   UDS 客户端：同步有界（connect/发送/等待共享一个截止时间预算），
    不创建 Executor；`yori` CLI 直接使用。
-   端点治理（DEC-010）：默认 `/run/yori/yori.sock`、`root:yori 0660`；
    bind 以收紧的 umask 创建 socket，随后显式 chmod/chown（Linux 的
    fchmod 对 socket fd 不生效）；陈旧端点仅在确为 socket 文件时替换。
    停止 = 唤醒 + join + unlink（EXEC-10 阶段 ①），幂等且不可重启。

### 13.5 授权、脱敏与 CLI 契约（M5）

-   授权在 daemon 侧基于 `SO_PEERCRED` 判定：Job owner（uid 相等）或
    admin（主 GID 匹配配置组，或 UID 属于启动时解析的 admin 组成员集，
    DEC-010）。`ps`/`queue`/`gpu` 对所有连接者开放（非自有 Job 脱敏），
    `cancel`/`logs` 仅 owner/admin。
-   脱敏（第 11.5 节）：非 owner 非 admin 的 Job 仅暴露
    JobId/状态/revision/owner_uid/退出状态；argv/cwd/tensorboard_logdir
    不出现在响应中（masked 标记置位）。
-   `yori` CLI 退出码：0 成功；1 请求失败（daemon 显式错误）；
    2 用法错误；3 传输失败（连接/超时/协议）。`logs -f` 与
    `yori tensorboard` 为 M6 范围，当前显式拒绝。

------------------------------------------------------------------------

## 14. Executor 集成边界

Yori 以 pinned git submodule 引入 Executor（`third_party/executor`，初始固定于
origin/master `4fd8e60`（2026-09-02），2026-09-09 升级至 `e2dc8ca`，MIT；
来源与校验信息见仓库根 `dependencies.lock.json`），并遵循仓库
[AGENTS.md](../../AGENTS.md) 的 Executor 强制条款与能力路由。Yori 是 Executor
很好的真实生产级使用场景，但 GPU scheduler 逻辑不能进入 Executor 核心。

职责边界：

``` text
Executor
────────────────────
Thread / Task
Channel / Mailbox
Blocking I/O
Snapshot
Sequencing
Diagnostics

Yori
────────────────────
Job
JobQueue
GPU
NVML
GPU Lease
Scheduling Policy
Process Lifecycle
User Permission
SQLite
```

训练程序本身：

``` text
Training Process
```

只是 Yori 管理的外部 workload，不需要链接 Executor。

`yorid` 内部可以使用 Executor 构造类似：

``` text
UnixSocketServer
       │
       ▼
SubmitRequest Channel
       │
       ▼
JobManager
       │
       ▼
Scheduler
       │
       ├──── GPU State Snapshot
       │
       └──── Job State Channel
                    │
                    ▼
            ProcessSupervisor
                    │
                    ▼
            LogStreamer ──► Topic<LogChunk> ──► logs -f 订阅者
```

------------------------------------------------------------------------

## 15. Heyaki 的未来位置

单节点 MVP 不需要 Heyaki。

本机：

``` text
yori CLI
    │
Unix Domain Socket
    ▼
yorid
```

即可。

进入多节点阶段后，可以演进为：

``` text
                 Central Scheduler
                        │
                     Heyaki
               ┌────────┼────────┐
               ▼        ▼        ▼
            yorid A   yorid B   yorid C
             8 GPU     4 GPU     8 GPU
```

此时每个 `yorid` 同时承担 Node Agent。

Heyaki 可以提供：

``` text
RPC
Event
ByteStream
Authentication
File Transfer
```

例如 RPC：

``` text
node.list_resources()
node.launch_job(JobSpec)
node.cancel_job(JobId)
node.query_job(JobId)
```

事件：

``` text
gpu.available
gpu.allocated

job.started
job.finished
job.failed

node.online
node.offline
```

训练日志可以通过 Heyaki ByteStream 远程传输。

checkpoint、模型等产物未来也可以按需要复用 Heyaki 文件传输能力。

因此整体层次为：

``` text
┌─────────────────────────────────┐
│              Yori               │
│                                 │
│ Job / Queue / GPU / Policy      │
├─────────────────────────────────┤
│ Heyaki（多节点时）              │
│                                 │
│ RPC / Event / Stream / Network  │
├─────────────────────────────────┤
│ Executor                        │
│                                 │
│ Execution / Async / Concurrency │
└─────────────────────────────────┘
```

------------------------------------------------------------------------

## 16. 调度策略演进

### 16.1 MVP

MVP 调度策略已由
[DEC-005](../decisions/DEC-005-global-fifo-scheduling.md)冻结为严格全局 FIFO：
可调度 Job 按 `(submit_time, JobId)` 升序排列；当前单 GPU Job 在队首等待
`FREE` GPU 时不做 backfill。调度只由提交、退出、取消、GPU 状态变化、恢复完成
和管理员操作等事件触发。

MVP 采用：

-   单服务器。
-   多 NVIDIA GPU。
-   多 Linux 用户。
-   单 GPU Job 为主。
-   全局 FIFO。
-   Yori GPU lease。
-   NVML 外部占用检测。
-   systemd daemon。
-   Unix Domain Socket。
-   SQLite。
-   UID/GID 降权执行。
-   Job 日志（捕获、落盘、轮转）。
-   `yori logs` 与 `yori logs -f` 流式跟随。
-   `yori tensorboard` 观察入口。
-   Job cancel。
-   daemon restart recovery。

### 16.2 第二阶段

增加：

-   多 GPU Job。
-   每用户最大并发数。
-   每用户队列配额。
-   priority。
-   weighted fair queue。
-   GPU 显存需求。
-   CPU/RAM 资源请求。
-   Job retry。
-   TUI。

对于多人共享服务器，长期来看 weighted fair queue / per-user quota
通常比单纯全局 priority
更重要，避免某个用户一次提交大量任务长期占据队列。

### 16.3 后续阶段

增加：

-   Web UI。
-   Container backend。
-   Job dependency。
-   Reservation。
-   GPU affinity。
-   MIG。
-   Heyaki transport。
-   Central Scheduler。
-   Multi-node scheduling。

------------------------------------------------------------------------

## 17. 安全要求

Yori 是系统级多用户服务，因此安全设计属于核心功能，而不是后续补丁。

必须遵守：

1.  **绝不以 root 身份直接执行用户提交的任意训练命令。**
2.  用户身份必须通过 `SO_PEERCRED` 等内核机制获得。
3.  Job owner 不得由客户端任意指定。
4.  `exec` 前正确执行 supplementary groups、`setgid`、`setuid`
    等身份切换。M2 细化：组列表在 fork 前经 `IdentityResolver` 解析，子进程内
    仅执行 `setgroups -> setgid -> setuid`（目标即当前有效身份时幂等跳过）；
    argv/envp 同样在 fork 前构造，子进程不分配内存。
5.  严格定义环境变量继承策略。M2 冻结为 DEC-006 三层合并：身份块（HOME/USER/
    LOGNAME/SHELL，值来自提交用户 passwd 记录）、daemon 白名单（PATH、LANG、
    TERM、TZ 与 `LC_*` 前缀）、GPU 映射块（`CUDA_VISIBLE_DEVICES`、
    `CUDA_DEVICE_ORDER=PCI_BUS_ID`）；用户变量最后应用，但保留键（身份块、
    CUDA 两键、`LD_PRELOAD`、`LD_LIBRARY_PATH`）出现即拒绝整个启动计划。
6.  限制 `/run/yori/yori.sock` 权限。
7.  Job 查询、日志读取、取消等操作执行 owner/admin 授权。
8.  防止日志路径、cwd、runtime 目录和持久化目录上的符号链接攻击。M2 已对
    日志文件使用 `O_NOFOLLOW|O_APPEND` 打开；父目录链的属主校验在 M4/M7
    落地。
9.  特权 daemon 的 IPC parser 和 launch path 应尽可能简单。M2 的 launch
    path 已收敛为：有界 `LaunchPlan` 复验 -> fork -> 仅 syscall 封装的子进程
    路径 -> exec 报告管道确认；exec 未确认（10 秒超时）即击杀并回收。
10. 外部 GPU 进程只影响资源状态，不主动终止或接管。
11. 长期考虑将 privileged process launcher 从主 daemon 中拆分。

------------------------------------------------------------------------

## 18. 推荐目录结构

``` text
yori/
├── apps/
│   ├── yori/                  # CLI
│   └── yorid/                 # daemon
│
├── include/yori/
│   ├── job/
│   ├── scheduler/
│   ├── gpu/
│   ├── process/
│   ├── recovery/
│   ├── ipc/
│   ├── store/
│   ├── launch/
│   └── observe/               # 日志流、TensorBoard 入口
│
├── src/
│
├── configs/
│   └── profiles/
│
├── packaging/
│   └── systemd/
│       └── yori.service
│
├── tests/
└── docs/
```

------------------------------------------------------------------------

## 19. MVP 完成判据

Yori MVP 至少满足：

-   [ ] 两个不同 Linux 用户可以同时 `submit`，且 Job 进入同一全局队列。
-   [ ] 当仅有一张可用 GPU 时，第二个 Job 保持 `QUEUED`。
-   [ ] 前一个 Job 结束后，后一个 Job 自动启动。
-   [ ] Job 以提交用户身份运行，产生的文件 owner 正确。
-   [ ] CLI 退出不终止训练。
-   [ ] SSH 断开不终止训练。
-   [ ] `cancel` 能终止完整训练进程组并释放 GPU。
-   [ ] 存在外部 GPU 训练进程时，不会错误分配对应 GPU。
-   [ ] `yorid` 异常重启后 `QUEUED` Job 不丢失。
-   [ ] `RUNNING` Job 不会因 daemon 重启被无条件重复启动。
-   [ ] 能查询 Job、队列、GPU 和日志状态。
-   [ ] RUNNING Job 的日志可通过 `yori logs -f` 实时跟随；CLI 中断或断线不影响
    训练进程，重连可基于 offset 续传。
-   [ ] 用户可通过 `yori tensorboard <job-id>` 以提交用户身份启动 TensorBoard
    查看训练指标；默认仅监听 `127.0.0.1`，对局域网开放需显式 `--host`。
-   [ ] 日志文件具备大小上限与轮转；慢跟随客户端被断开并收到明确
    BACKPRESSURE 错误，daemon 与训练进程不受影响。
-   [ ] GPU 释放后自动触发下一轮调度。

------------------------------------------------------------------------

## 20. 核心设计原则

Yori 的核心不是一个"检测 GPU 然后执行 shell"的脚本，而是：

> **服务器级单一权威的 GPU workload supervisor。**

用户只声明：

``` text
我要运行什么
+
我需要多少资源
```

Yori 决定：

``` text
什么时候运行
+
在哪些 GPU 上运行
+
如何维护该进程生命周期
```

最终职责关系保持为：

``` text
Yori
  └─ Resource Scheduling / Job Lifecycle

Executor
  └─ Local Execution / Concurrency

Heyaki
  └─ Cross-node Communication
```

保持这三层边界，可以让 Yori 从解决当前共享服务器 GPU
排队问题的轻量工具，逐步演进为多用户、多 GPU、最终可扩展到多节点的轻量
GPU workload scheduler。
