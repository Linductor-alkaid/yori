# DEC-014：GPU Set（--gpu-any-of）与 PREFERRED 模式

> 状态：Accepted（2026-09-13 随 M11 启动冻结；负责人以"依照设计与计划推进下一步
> 开发"确认按总计划推进 POST-11 余项）
> 日期：2026-09-13
> 负责人：Linductor-alkaid
> 冻结里程碑：M11
> 替代/被替代：无（承接 [DEC-012](DEC-012-gpu-placement-policy.md) 延后的
> POST-11 余项；与 [DEC-013](DEC-013-affinity-aware-any-placement.md) 叠加，
> 不改变其 kAny 选择规则）
> 需求来源：[#10](https://github.com/Linductor-alkaid/yori/issues/10)
> （Placement 模型建议 2/3；总计划 POST-11）

## 背景与问题

M9 交付了 `kAny` 与单设备 `kRequired`。issue #10 的原始建议包含第三种基础
形态，M9 时被裁剪为 POST-11 延后：

1. **GPU Set（eligible set）**：任务允许运行在用户声明的 GPU 集合内，调度器
   只在集合内排队与分配（硬约束作用于集合，而非单设备）。对应 CLI 记法
   `--gpu-any-of`。
2. **PREFERRED（优先指定、允许 fallback）**：用户给出软偏好的目标设备，
   目标不可用时允许 Yori 分配其他 GPU，避免"指定卡被占就永远排队"。

DEC-012 延后时的遗留缺口：`GpuPlacement.devices` 被"恰 1 个"单设备语义
锁死，协议、校验、调度与持久化都只支持单设备 REQUIRED。

## 决策

### 1. JobSpec 契约：多设备 REQUIRED 集合 + kPreferred 软偏好

- `GpuPlacementMode` 增加 `kPreferred = 2`。
- `GpuPlacement.devices` 语义按模式：
  - `kAny`：空（不变）。
  - `kRequired`：1..`kMaxPlacementDevices`（=8）个**去重、合法** UUID。
    单设备保持 `--gpu` 既有语义；多设备即 GPU Set——硬约束作用于集合，
    绝不 fallback 到集合外（DEC-012 的 REQUIRED 原则在集合上成立）。
  - `kPreferred`：恰 1 个合法 UUID（软偏好目标）。
- 校验沿用 `kInvalidGpuPlacement`（数量、重复、非法 UUID）与
  `kPlacementGpuRequestConflict`（placement 与 `gpu_request` 计数语义互斥）。
- `--gpu N`（单设备 REQUIRED）与 `--gpu-any-of`、`--gpu-preferred` 互斥，
  CLI 显式报错；`--gpus` 计数语义仍与一切 placement 互斥。

### 2. Scheduler：集合候选选择 + PREFERRED 回退

- `kRequired`（集合）：候选 = 集合内 `FREE` 且未被 lease 的设备，按物理
  index 升序取第一张；集合内无可用设备则保持 `QUEUED`。等待原因聚合
  （不新增 WaitReason）：集合内任一设备被 lease → `AFFINITY_GPU_ALLOCATED`；
  否则任一 `EXTERNAL_BUSY` → `AFFINITY_GPU_EXTERNAL`；否则
  `AFFINITY_GPU_STATE`（`UNAVAILABLE` 或观测缺失）。`ScheduleSkip.target`
  携带集合中按物理 index 最先出现的非可用设备（观测缺失时省略）。
- `kPreferred`：目标 `FREE` 且未被 lease → 使用目标（
  `selection_reason = kDefault`）；否则回退到 `kAny` 的亲和感知候选选择
  （DEC-013 ranking 不变），此时 `selection_reason = kPreferredFallback`
  （新增枚举值，诊断语义，不持久化）；全局无 FREE 候选 → 保持 `QUEUED`，
  等待原因 `kNoFreeGpu`（与 kAny 同语义：PREFERRED 允许 fallback，等待
  只发生在全局无空闲）。
- DEC-013 软保护集合同步扩展：`affinity_targets` 收集窗口内等待中
  `kRequired` Job 的**全部集合设备**；`kPreferred` 目标不参与保护（它允许
  迁移，不构成硬依赖）。

### 3. IPC 协议 v4（增量扩展，v1-v3 帧仍接受）

- `kProtocolVersion = 4`。
- SUBMIT：`gpu_spec` 在 v4 允许逗号分隔 1..8 个条目（index 或 UUID 混用，
  daemon 逐条以当前观测解析为去重 UUID 集合，mode = kRequired）；v3 帧及
  更早版本的合法输入（单条目）行为不变。
- INSPECT：`gpu_placement_mode` 值域扩展（0 = any，1 = required，
  2 = preferred）；v4 响应尾部追加 required 集合的设备 UUID 列表
  （u8 count + strings；preferred/any 为空）；单设备场景
  `gpu_placement_device` 字段保持不变，旧语义不破坏。

### 4. 持久化 schema v4（单事务增量迁移）

- 新增列 `gpu_placement_devices TEXT`：required 集合的逗号连接 UUID 列表
  （单设备 REQUIRED 同时写入单值列与集合列，读取以集合列为权威）。
- `gpu_placement_mode` 取值扩展 `'preferred'`；preferred 的目标继续存于
  `gpu_placement_device` 单值列。
- v1/v2/v3 库打开时按既有迁移链补齐（`migrate_schema_v3_to_v4`：ADD COLUMN
  并回填 required 行）。

### 5. 可观察性

- `wait_reason` 复用现有枚举（决策 2），ps/queue 视图无需新字段。
- INSPECT 显示 `placement: required A|B|C` / `placement: preferred X` /
  `placement: any`。

## 备选方案

- **为 GPU Set 引入独立模式 `kSet`**：与 kRequired 的硬约束语义重复，徒增
  状态空间与迁移负担；集合上的 REQUIRED 即 Set（否决）。
- **gpu_spec 改为结构化列表字段**：破坏 v3 尾部字段布局，v3 帧解析需分叉；
  逗号分隔在 daemon 侧解析即可承载集合，wire 布局不变（采纳逗号方案）。
- **PREFERRED 目标参与 DEC-013 软保护**：PREFERRED Job 本身允许迁移，
  其目标不是其他 Job 的硬依赖；参与保护会无谓收窄 kAny 候选（否决）。
- **新增 PREFERRED 专用 WaitReason**：PREFERRED 等待仅发生在全局无空闲
  GPU，与 kNoFreeGpu 语义重合（否决）。

## 影响与风险

- `JobSpec.validate`、`FifoScheduler`、IpcService 提交解析、SqliteStateStore
  迁移、CLI 提交/inspect、协议编解码同批变更；全部向后兼容（v1-v3 帧、
  v1-v3 库均可打开/接受）。
- 集合 REQUIRED 的 wait_reason 为聚合结论，不再指向唯一目标——已通过
  `ScheduleSkip.target` 的集合内代表设备保持可定位性。
- 多设备上限 8 与单 GPU Job 约束一致（POST-01 多 GPU Job 立项前不变）。

## 验证方式

- 单测：JobSpec 校验矩阵（集合数量/重复/非法 UUID/preferred 冲突）、
  调度场景（集合命中按 index、集合全忙跳过与 wait_reason 聚合、preferred
  命中/preferred 回退/preferred 全忙、DEC-013 集合保护）、协议 v4 编解码
  （gpu_spec 列表、inspect 集合、v3 兼容）、schema v3→v4 迁移与读写。
- 集成：submit→schedule→exit 闭环覆盖 preferred 回退与集合 REQUIRED。
- ASAN/UBSAN/TSAN + GCC/Clang 双编译 + 格式/静态检查，CI 全绿。

## 关联文档和工作项

- [M11 里程碑](../plans/m11-preferred-gpu-set.md)（POST-11 余项收口）。
- [DEC-012](DEC-012-gpu-placement-policy.md)（placement 模型母决策）、
  [DEC-013](DEC-013-affinity-aware-any-placement.md)（kAny 亲和感知选择）、
  设计文档 §9/§16.2/§16.3。
