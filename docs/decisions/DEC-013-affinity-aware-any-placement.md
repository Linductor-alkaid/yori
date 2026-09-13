# DEC-013：affinity-aware 的 ANY 设备选择（软保护）

> 状态：Accepted（2026-09-13 随 M10 启动冻结；负责人以 issue #22 提出并确认提前实现）
> 日期：2026-09-13
> 负责人：Linductor-alkaid
> 冻结里程碑：M10
> 替代/被替代：无（扩展 [DEC-012](DEC-012-gpu-placement-policy.md) 决策 3 的
> kAny 选择规则；POST-11 中"affinity-aware ANY 设备选择"子项由此提前承接）
> 需求来源：[#22](https://github.com/Linductor-alkaid/yori/issues/22)

## 背景与问题

M9 交付的 placement 模型中，GPU 选择只考虑"当前 Job 能用哪些 GPU"，不考虑
队列中其他 Job 的 placement 约束。`kAny` 的确定性规则是物理 index 升序取
第一张 FREE，于是在多卡空闲时可能发生资源错配：

``` text
GPU0 = FREE, GPU1 = FREE
J1 = ANY, J2 = REQUIRED GPU0
→ J1 获得 GPU0，GPU1 空闲，J2 无法运行
```

这不是资源总量不足，而是 placement 决策造成的碎片。强化学习训练场景中
REQUIRED 表达的硬亲和（稳定性、设备拓扑、可复现性、故障排查）真实存在，
可迁移任务应当优先使用没有被硬亲和任务依赖的 GPU。DEC-012 已将该场景记录
为已接受行为，并把 affinity-aware ANY 选择列为 POST-11 可选优化；issue #22
以真机反馈将其提前。

## 决策

### 1. 软保护（soft protection），不是硬预留（reservation）

`kAny` Job 在多个当前可用 GPU 之间选择时，避开同一 FIFO 扫描窗口内等待中
REQUIRED Job 指定的 GPU：

``` text
free_candidates     = observed FREE - active leases（候选集过滤不变）
affinity_targets    = 扫描窗口内 QUEUED 的 kRequired Job 的目标集合
preferred_candidates = free_candidates - affinity_targets

preferred 非空 → 从 preferred 按物理 index 升序选择
preferred 为空 → 回退完整 free_candidates（不人为空闲 GPU）
```

不因为队列中存在 REQUIRED 等待者就把 GPU 标记为 RESERVED、不阻止本来可以
执行的任务。所有保护信息由 authoritative 队列 + JobSpec 派生，不持久化、
不建立 lease、不新增 GPU 逻辑状态、无 reservation 生命周期。

### 2. 以 ranking 阶段表达，不写 REQUIRED 特判分支

`kAny` 候选集过滤之后增加轻量 ranking：先最小化与等待 REQUIRED 目标的
冲突，再以物理 index 升序决胜（`min(conflict, physical_index)`）。这为
POST-11 的 `PREFERRED`/GPU Set 与未来的 scarcity/topology 评分保留扩展位，
不需要重构 Scheduler 结构。

### 3. 保护范围与 bounded skip 共用扫描窗口

`affinity_targets` 只从当前 `run_once` 的有界扫描窗口（默认 32）收集，
窗口外 Job 的目标不参与本轮保护；窗口内预计算一次（O(window + gpu_count)），
不为每个 ANY Job 重复遍历队列。保护集合表达"当前仍等待这些 GPU 的 REQUIRED
Job"：REQUIRED 被调度（目标经 lease 离开候选集）、被取消或进入终态后，
下一次调度事件自然重算，无残留状态。

### 4. 选择结论可观察

`SchedulerEvent` 携带 `selection_reason`（仅诊断语义，不构成新 Job/GPU
状态、不进 IPC 协议）：

``` text
DEFAULT                   亲和保护未改变选择（含全部 kRequired 路径）
AVOID_REQUIRED_AFFINITY   存在非冲突候选，实际避开了等待中 REQUIRED 目标
AFFINITY_FALLBACK         全部 FREE 候选均为等待中 REQUIRED 目标，回退使用其一
```

`JobManagerStats` 对后两者计数（`scheduler_affinity_avoids`/
`scheduler_affinity_fallbacks`），供分析 placement 碎片改善与解释偏离
lowest-index 的选择。

### 5. 不变的契约

REQUIRED 仍是严格硬约束、绝不 fallback；GPU UUID 仍是持久化设备身份；
lease 仍是资源占用的 authoritative fact；NVML 观测与 lease 事实分离；
FIFO 仍决定 Job 服务顺序；bounded skip 仍负责不可调度 Job 的 HOL 消解；
本决策不保证未来 REQUIRED Job 一定立即获得目标 GPU。

## 备选方案

- **硬 reservation**：等待中的 REQUIRED 目标立即置 RESERVED、禁止前方 ANY
  使用。被否决：会阻止当前可执行的任务、人为空闲 GPU，实质改变 FIFO 服务
  语义，且需定义 reservation 生命周期、优先级、取消、饥饿与 backfill 规则；
  这些复杂度属于未来多资源组合调度（POST-01/POST-03 之后的统一设计）。
- **维持现状（POST-11 延后）**：被 issue #22 的真机反馈推翻——碎片场景
  已实际出现，且软保护实现成本有界。
- **完整 placement score 框架（多维度评分）**：当前只有单一冲突维度，
  先交付两值 ranking + 稳定 tie-breaker，框架位保留给 POST-11。

## 影响与风险

- **调度决策不再恒等于 lowest-index**：`kAny` Job 可能落在更高物理 index
  的 GPU 上。选择以 `selection_reason` 显式解释，deterministic 规则仍在
  （同输入同输出）。
- **窗口外目标不受保护**：REQUIRED 等待者位于扫描窗口之外时，ANY 仍可能
  占用其目标。与 bounded skip 的可观察范围一致，窗口大小可配
  （`--scheduler-scan-window`）。
- **无可替代资源时目标仍会被 ANY 占用**（AFFINITY_FALLBACK）：利用率优先
  的显式取舍，等待中的 REQUIRED 以既有 `wait_reason` 解释，行为可观察。
- 多个 REQUIRED 指向同一 GPU 时保护集合去重（集合语义），REQUIRED 之间的
  竞争仍按 FIFO 决胜，本决策不介入。

## 验证方式

issue #22 第"建议测试矩阵"10 条场景全部落地：等价替代时避开（1/3/7）、
无替代时回退且不人为空闲（2/4）、REQUIRED 在前正常服务（5）、目标忙时
保护无效果（6）、同目标多等待者去重与 FIFO 决胜（7）、窗口外不保护（8）、
取消后无残留（9）、全命中时回退事件可观察（10）。承载：
`unit_fifo_scheduler_test`（Core 矩阵）、`integration_gpu_manager_scheduler`
（观测驱动联动）、`unit_job_manager_test`（总装回退路径与计数器）。

## 关联文档和工作项

[Issue #22](https://github.com/Linductor-alkaid/yori/issues/22)；
[DEC-012](DEC-012-gpu-placement-policy.md)（决策 3 扩展；其"影响与风险"
中 POST-11 可选优化条目由本决策承接）；
[DEC-005](DEC-005-global-fifo-scheduling.md)（FIFO 服务顺序不变）；
[设计文档](../design/yori-project-design.md)第 9、16.2 节；
M10 工作项（[总计划](../plans/yori-implementation-plan.md)第 5 节）；
POST-11 余项（`PREFERRED`、GPU Set）、POST-12（tag/pool、项目级 profile）。
