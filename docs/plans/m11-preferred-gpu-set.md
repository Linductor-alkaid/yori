# M11：GPU Set（--gpu-any-of）与 PREFERRED 模式

> 状态：InProgress（2026-09-13 启动；负责人以"依照设计与计划推进下一步开发"
> 确认按总计划推进 POST-11 余项）
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M9（PR [#20](https://github.com/Linductor-alkaid/yori/pull/20)）、
> M10（PR [#23](https://github.com/Linductor-alkaid/yori/pull/23)）
> 决策依据：[DEC-014](../decisions/DEC-014-preferred-gpu-set-placement.md)
> （Accepted，2026-09-13 随 M11 启动冻结；承接
> [DEC-012](../decisions/DEC-012-gpu-placement-policy.md) 延后的 POST-11 余项）
> 需求来源：[Issue #10](https://github.com/Linductor-alkaid/yori/issues/10)
> （总计划 POST-11："M9/M10 交付后出现软偏好需求"——issue #10 的软偏好
> 反馈在 M9/M10 交付后仍未有对应能力）
> 建议发布点：`v0.5.0`
> 更新日期：2026-09-13（启动）

## 目标

1. **GPU Set（eligible set）**：`--gpu-any-of` 声明允许运行的 GPU 集合
   （1..8 个 index/UUID），调度器只在集合内排队与分配，绝不 fallback 到
   集合外；单设备 `--gpu` 语义不变。
2. **PREFERRED 软偏好**：`--gpu-preferred` 声明优先目标，目标不可用时
   回退到亲和感知的 kAny 候选选择（DEC-013 ranking），不因指定卡被占而
   无限期排队；回退结论以 `selection_reason = PREFERRED_FALLBACK` 可观察。
3. **等待可解释**：集合 REQUIRED 的等待原因按 lease > 外部占用 > 状态
   聚合（复用现有 `wait_reason` 枚举），携带集合内代表设备。
4. **向后兼容**：协议 v4 增量扩展（v1-v3 帧接受）、schema v4 单事务迁移
   （v1-v3 库可打开）；既有单设备 REQUIRED、kAny 行为与 FIFO 服务顺序
   不变。

## 范围与非目标

范围（DEC-014 决策 1-5）：

- Core：`GpuPlacementMode::kPreferred`、`GpuPlacement.devices` 多设备集合
  语义与 `kMaxPlacementDevices = 8` 上限、`JobSpec::validate` 扩展、
  `GpuSelectionReason::kPreferredFallback`。
- Scheduler：集合 REQUIRED 候选选择与聚合 wait_reason、PREFERRED 命中/
  回退/跳过、`affinity_targets` 扩展到集合。
- IPC v4：SUBMIT `gpu_spec` 逗号分隔列表、INSPECT mode 值域 + 设备列表。
- 持久化 schema v4：`gpu_placement_devices` 列 + v3→v4 迁移。
- CLI：`--gpu-any-of` / `--gpu-preferred`（互斥校验、本地预检、usage、
  inspect 展示）。
- 测试矩阵（见下）与文档同步。

非目标：

- 多 GPU Job（`--gpus > 1` 仍拒绝）：POST-01。
- PREFERRED 目标参与 DEC-013 软保护（决策 2 明确排除）。
- 项目级 placement profile、tag/pool、管理员静态映射：POST-12。
- IPC/持久化之外的观测面变化：无新 ps/queue 字段。

## 设计与决策依据

- [DEC-014](../decisions/DEC-014-preferred-gpu-set-placement.md)（决策 1-5
  与验证方式）。
- [DEC-012](../decisions/DEC-012-gpu-placement-policy.md)（placement 模型、
  daemon 侧 index→UUID 解析、有界跳过）。
- [DEC-013](../decisions/DEC-013-affinity-aware-any-placement.md)（kAny
  亲和感知选择，本里程碑叠加不修改）。
- 设计文档 §9（调度流程）、§16.2/§16.3。

## 工作项

- [x] `M11-01` Core 契约扩展：`kPreferred` 模式、devices 集合语义、
  validate 矩阵、`kPreferredFallback`。
- [x] `M11-02` Scheduler：集合 REQUIRED 选择与聚合等待原因、PREFERRED
  命中/回退、affinity_targets 集合化。
- [x] `M11-03` IPC v4：SUBMIT gpu_spec 列表、INSPECT 设备列表、编解码与
  v3 兼容；daemon 侧解析（去重、逐条解析失败拒绝）。
- [x] `M11-04` 持久化 schema v4：新列、迁移链、读写往返。
- [x] `M11-05` CLI：`--gpu-any-of` / `--gpu-preferred`、usage、inspect
  展示。
- [x] `M11-06` 测试矩阵落地与全量验证（GCC/Clang、ASAN/UBSAN/TSAN、
  格式/静态检查）。
- [x] `M11-07` 文档同步：DEC-012/DEC-014、设计文档 §9/§16.2/§16.3、
  总计划（状态、POST-11 收口）、CHANGELOG。

## 风险与阻塞

- 无环境限制项：全部验证本地可执行；Ubuntu 22.04 兼容构建按惯例依赖
  CI runner。

## 测试与退出条件

- [x] JobSpec 校验：required 集合 1..8、重复/非法 UUID 拒绝、preferred
  恰 1、preferred×gpu_request 冲突、any 携带设备拒绝。
- [x] 调度：集合命中按 index 最小、集合全忙跳过（wait_reason 聚合三种
  各自可达 + target 代表设备）、preferred 目标命中（kDefault）、preferred
  回退（PREFERRED_FALLBACK 且遵循 DEC-013 ranking）、preferred 全忙
  （kNoFreeGpu）、集合 REQUIRED 参与 affinity_targets 保护。
- [x] IPC：gpu_spec 列表编解码往返、非法列表拒绝、v3 单条目帧兼容、
  inspect 集合列表往返、mode 值域校验。
- [x] SQLite：v3 库打开迁移到 v4、required 集合/preferred 读写往返、
  篡改显式失败不回归。
- [x] 全量测试 + ASAN/UBSAN/TSAN + 格式/静态检查本地通过（见验证记录）；
  Clang 构建由 CI runner 覆盖（本地无 clang）。CI 全绿后按仓库纪律提交 MR
  （合并需负责人授权）。

## 验证记录

### 2026-09-13：M11-01～M11-07 实现与本地验证

- 环境：Ubuntu Linux（本机），GCC C++20；clang-format 18.1.3。
- 实现提交：Core 契约（`kPreferred`、集合语义、`kMaxPlacementDevices=8`）、
  调度器（集合 REQUIRED 选择与聚合等待原因、PREFERRED 命中/回退、
  affinity_targets 集合化、`kPreferredFallback`）、IPC 协议 v4
  （`gpu_spec` 列表、`gpu_preferred_spec`、INSPECT 设备列表）、
  SqliteStateStore schema v4（`gpu_placement_devices` 列 + v3→v4 迁移）、
  CLI（`--gpu-any-of`/`--gpu-preferred`、usage、inspect 展示）。
- 测试矩阵落地：unit_job_spec（集合上限/重复/preferred 校验）、
  unit_fifo_scheduler（M11 七场景）、unit_ipc_protocol（v4 编解码 + v3
  兼容 + mode 值域手工帧）、unit_ipc_service（列表解析去重/上限/互斥/
  preferred/解析失败）、unit_sqlite_state_store（v4 roundtrip、v3 库迁移
  回填、集合列篡改显式失败）。
- 结果（本地，全部通过）：
  - `build/debug`：ctest 44/44 passed。
  - `build/asan`：ctest 44/44 passed。
  - `build/ubsan`：ctest 44/44 passed。
  - `build/tsan`（`setarch $(uname -m) -R` 规避 ptrace 限制）：44/44 passed。
  - `clang-format --dry-run --Werror`（全部变更文件）：无输出（通过）。
- 未执行项与补跑条件：Clang 双编译与 Ubuntu 22.04 兼容构建依赖 CI runner
  （本地无 clang）；push 后以 CI run 为准，MR 合并与 `v0.5.0` 发布需负责人
  授权。
