# DEC-012：GPU placement 约束与 FIFO 有界跳过语义

> 状态：Accepted（2026-09-13 随 M9 启动冻结；负责人确认依计划推进）
> 日期：2026-09-12（Proposed），2026-09-13（Accepted）
> 负责人：Linductor-alkaid
> 冻结里程碑：M9
> 替代/被替代：部分修订 [DEC-005](DEC-005-global-fifo-scheduling.md)（队首不跳过条款）；需求来源 [#10](https://github.com/Linductor-alkaid/yori/issues/10)

## 背景与问题

MVP 调度模型隐含"满足基本资源条件的 GPU 对训练任务是同质、可互换的"假设。
强化学习团队的真机反馈显示该假设不总是成立：任务与特定 GPU 存在耦合（长期
验证稳定、迁移后异常或不可复现）、Isaac 类仿真涉及多设备选择路径、部分项目
对物理 index/拓扑有显式或隐式依赖，且排查与复现阶段需要保持设备固定。用户
需要的是"由用户决定任务允许在哪些 GPU 上运行，Yori 在约束范围内排队调度"，
而不是 Yori 替用户假定任务可任意迁移。

当前实现与该需求的冲突点：`JobSpec` 无 placement 字段，调度把 GPU 视为同质池
并按物理 index 升序取第一张 FREE；DEC-005 明确"队首 Job 在没有 FREE GPU 时
不得跳过队首"，这会使一个暂时不可满足的硬亲和 Job 阻塞整个全局队列
（Head-of-Line Blocking）。

## 决策

### 1. Placement 模型（M9 交付 ANY + REQUIRED）

``` cpp
enum class GpuPlacementMode { kAny, kRequired };

struct GpuPlacement {
  GpuPlacementMode mode{GpuPlacementMode::kAny};
  std::vector<gpu::GpuUuid> devices;   // kRequired 时恰 1 个（单 GPU Job 约束）
};
```

- `kAny`：现行为，全部 GPU 参与候选；
- `kRequired`：只允许运行在指定 GPU 上，目标被 lease/`EXTERNAL_BUSY`/
  `UNAVAILABLE` 时保持 `QUEUED`，绝不 fallback 到其他 GPU；
- `kPreferred`（优先指定、允许 fallback）**不进入 M9**，与 GPU Set
  （`--gpu-any-of`）一同延后（POST-11）。

Issue #10 开放问题的裁决：

- **`--gpu N` 语义 = REQUIRED（硬亲和）**。`--gpus N` 维持资源计数语义
  （POST-01 前恒为 1），两者互斥并显式报错。用户心智模型"这卡是我的"对应
  硬约束；软偏好等 PREFERRED 落地时再给独立拼写。
- **设备身份复用现有 `GpuUuid`**，不新建 `GpuId` 抽象。UUID 已是 lease 与
  StateStore 的稳定身份（设计第 7 节），引入第二套身份只会造成换算层。
- **项目级默认 placement profile 与管理员静态 user/project→GPU 映射延后**
  （POST-12）：先交付用户显式声明的最小模型。

### 2. 提交解析：daemon 侧 index → UUID

CLI `--gpu N`（NVML index）或 `--gpu GPU-xxxx`（UUID）经 IPC 传入，**由
daemon 在提交处理时**以当前 GPU 观测快照解析为 UUID 并写入 JobSpec。索引只是
易用输入，不构成持久化身份；解析失败（索引不存在/UUID 未知）拒绝提交。设备
枚举顺序变化时亲和指向由 UUID 保证不漂移。

### 3. Scheduler 候选集过滤

`run_once` 的 GPU 选择由"取第一张 FREE"扩展为：

``` text
Job placement → 限定候选设备集合（kRequired 为目标单卡，kAny 为全部）
→ 排除 UNAVAILABLE / EXTERNAL_BUSY
→ 排除已被 lease 的 GPU
→ 候选非空则按现有确定性规则（物理 index 升序）选择并建立 lease
→ 候选为空则该 Job 本轮不可调度
```

lease 事实与 NVML 观测分离的原则（RULE-05）不变。

### 4. FIFO 有界跳过（修订 DEC-005 第 2 条队首不跳过条款）

调度按 `(submit_time, JobId)` FIFO 顺序扫描队列；遇到当前不可调度的 Job
（无空闲 GPU 或亲和目标不可用）时**跳过并继续考察后续 Job**，单次扫描条数
有界（默认 32，配置校验上限），被跳过 Job 保持 `QUEUED` 与原队列位置，并
产生携带原因的结构化跳过事件（可观察、不静默）。`kAny` Job 之间的先到先得
语义不变；`kRequired` Job 之间对同一目标 GPU 的竞争同样按 FIFO 顺序决胜。

即 FIFO 从"严格队首阻塞"修订为"FIFO 服务顺序 + 有界跳过"：

``` text
J1(REQUIRED GPU0, GPU0 忙) + J2(ANY, GPU1 空闲) → J2 先启动，J1 保持 QUEUED
```

队首一致性核对（防派生队列漏项）扩展到扫描窗口内的被跳过 Job。

### 5. 等待原因可见

`ps`/`queue` 对 QUEUED Job 暴露 daemon 派生的 `wait_reason`（非持久化字段，
来自最近一次调度评估）：

``` text
NO_FREE_GPU            全局无空闲 GPU
AFFINITY_GPU_ALLOCATED 亲和目标被 Yori lease
AFFINITY_GPU_EXTERNAL  亲和目标被外部进程占用
AFFINITY_GPU_STATE     亲和目标 UNAVAILABLE/观测缺失
```

owner/admin 视图附带目标 UUID；脱敏视图（非 owner 非 admin）不暴露 placement
明细，维持既有脱敏边界。

### 6. 持久化与恢复（schema v3）

`GpuPlacement` 随 JobSpec 持久化（SQLite schema 2 → 3 增量迁移，v2 库按
`kAny` 补全）。daemon 重启恢复时 placement 随 Job 一致恢复；**UUID 枚举变化
不得迁移 REQUIRED Job**——目标 GPU 从观测中消失时 Job 保持 `QUEUED` 并以
`AFFINITY_GPU_STATE` 解释，由用户取消或管理员处置，绝不自动改绑其他设备。

## 备选方案

- 管理员静态映射（user/project → GPU 表）先行：覆盖"专卡专用"团队习惯更快，
  但引入管理员策略面与冲突语义，用户显式声明的最小模型已可满足当前反馈，
  映射作为 POST-12 演进。
- 完整 backfill 框架：跳过 + 原因事件已满足单 GPU 模型需求；异构资源
  （多卡/显存）下的预留与回填决策留给 POST-01/POST-03，届时按 DEC-005
  既定要求重新决策。
- 维持严格队首：一个被指定 GPU 阻塞的 Job 会阻塞全局队列，直接违背
  issue #10 的核心诉求，不可接受。

## 影响与风险

- **严格 FIFO 可预测性被显式放宽为"服务顺序 + 有界跳过"**：后来者可能越过
  暂不可满足的亲和 Job 启动。这是决策的核心取舍，跳过事件与 `wait_reason`
  保证行为可解释；公平性语义（配额/权重）仍留在 POST-02。
- `kAny` Job 可能占用后队 `kRequired` Job 的目标卡（调度不做前瞻预留）：
  记录为已接受行为；affinity-aware 的 ANY 设备选择（避开队列中 REQUIRED 目标）
  作为 POST-11 的可选优化。
- 有界扫描窗口（默认 32）之后的 Job 本轮不被考察：窗口大小可配置，跳过
  事件携带"窗口截断"标记，不静默。
- `wait_reason` 是最近一次评估的派生视图，事件间隙可能短暂过时——展示语义
  定义为"最近一次调度评估结论"。

## 验证方式

M9 测试矩阵采用 issue #10 第 12 节的 12 条场景：ANY 正常选择；REQUIRED 空闲
时只分配目标卡；目标被 lease/`EXTERNAL_BUSY`/`UNAVAILABLE` 时保持 QUEUED 且
不 fallback；有界跳过不造成全局 HOL 阻塞；daemon 重启后 placement 正确恢复；
index↔UUID 映射变化不错误迁移 REQUIRED Job；owner/admin/脱敏视图与
`ps`/`queue` 的 `wait_reason` 展示。

## 关联文档和工作项

[Issue #10](https://github.com/Linductor-alkaid/yori/issues/10)；
[DEC-005](DEC-005-global-fifo-scheduling.md)（部分修订）；
[DEC-009](DEC-009-sqlite-state-store.md)；
[设计文档](../design/yori-project-design.md)第 6.1、7、9、13 节；
M9 工作项（[总计划](../plans/yori-implementation-plan.md)第 5 节）；
POST-11（PREFERRED/GPU Set/affinity-aware 选择）、POST-12（tag/pool、项目级
profile、管理员静态映射）。
