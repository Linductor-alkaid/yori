# M9：GPU placement 亲和调度

> 状态：In Progress（实现完成，本地五预设验证通过；待 PR 合并后收口 Completed）
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M7（打包与 MVP 端到端验收，PR [#11](https://github.com/Linductor-alkaid/yori/pull/11)）；与 M8 无代码依赖
> 决策依据：[DEC-012](../decisions/DEC-012-gpu-placement-policy.md)（Accepted，2026-09-13 随 M9 启动冻结；部分修订
> [DEC-005](../decisions/DEC-005-global-fifo-scheduling.md) 队首不跳过条款）
> 需求来源：[Issue #10](https://github.com/Linductor-alkaid/yori/issues/10)
> 建议发布点：`v0.3.0`（tag 与发布动作需负责人明确授权后执行）
> 更新日期：2026-09-13（实现完成，本地五预设验证 + tidy/format 清洁）

## 目标

1. **placement 由用户声明**：`yori submit --gpu N|--gpu GPU-xxxx` 表达
   REQUIRED 硬亲和（该 Job 只允许运行在指定 GPU 上），缺省保持 ANY（全部
   GPU 候选）。Yori 不替用户假定任务可在 GPU 间迁移。
2. **硬亲和不造成全局队头阻塞**：FIFO 从"严格队首阻塞"修订为"FIFO 服务
   顺序 + 有界跳过"——暂不可满足的 Job 被跳过并保持 `QUEUED` 与原队列
   位置，后续 Job 可继续调度；跳过携带结构化原因，不静默。
3. **等待原因可见**：`ps`/`queue` 对 QUEUED Job 展示 daemon 派生的
   `wait_reason`（最近一次调度评估结论），owner/admin 附带目标 UUID，
   脱敏视图不暴露 placement 明细。
4. **持久化一致**：placement 随 JobSpec 持久化（schema v3），daemon 重启
   恢复后 UUID 枚举变化不错误迁移 REQUIRED Job（目标消失保持 `QUEUED`，
   由用户取消或管理员处置）。

## 范围与非目标

范围（DEC-012 决策 1-6）：

- `GpuPlacement` Core 类型（`kAny`/`kRequired`，设备身份复用 `gpu::GpuUuid`）
  与 `JobSpec` 校验：`kAny` devices 为空、`kRequired` devices 恰 1 个合法
  UUID、REQUIRED 与 `gpu_request` 计数语义互斥（`--gpu` 与 `--gpus` 显式
  同时出现即拒绝）。
- daemon 侧提交解析：CLI `--gpu N`（NVML index）或 `--gpu GPU-xxxx`（UUID）
  经 IPC 原样传入，daemon 在提交处理时以当前 GPU 观测快照解析为 UUID 写入
  JobSpec；索引不构成持久化身份；解析失败（不存在/未知/暂无观测）显式拒绝。
- Scheduler 候选集过滤与 FIFO 有界跳过：`run_once` 按
  `(submit_time, JobId)` 扫描队列（默认窗口 32，配置校验上限 4096），
  placement 限定候选设备 -> 排除 `UNAVAILABLE`/`EXTERNAL_BUSY`/已 lease ->
  物理索引升序选择；`kRequired` 目标不可用绝不 fallback；被跳过 Job 产生
  携带原因的结构化跳过事件；队首一致性核对扩展到扫描窗口。
- `wait_reason` 派生与展示：调度评估结论（`NO_FREE_GPU`/
  `AFFINITY_GPU_ALLOCATED`/`AFFINITY_GPU_EXTERNAL`/`AFFINITY_GPU_STATE`）
  由 JobManager 以 executor comm 最新值视图发布，IPC `ps`/`queue` 读取；
  非持久化字段。
- IPC 协议 v3：SUBMIT 尾部追加可选 placement 输入（v1/v2 帧缺省无亲和）；
  PS/QUEUE 条目追加 wait_reason（owner/admin 附目标 UUID）；INSPECT 追加
  placement；daemon 同时接受 v1/v2。
- 持久化 schema v3：`yori_jobs` 新增 `gpu_placement_mode`/`gpu_placement_device`
  列，v1/v2 库单事务增量迁移，v2 行按 `kAny` 补全读取。
- 测试与文档：issue #10 12 场景矩阵中 M9 范围内 10 项（PREFERRED 两项属
  POST-11）；设计文档（§6.1/§7/§9/§13）、总计划同步。

非目标：

- `PREFERRED` 软偏好模式、GPU Set（`--gpu-any-of`）、affinity-aware 的 ANY
  设备选择（避开队列中 REQUIRED 目标）：POST-11。
- GPU tag/pool 资源池、项目级默认 placement profile、管理员静态
  user/project→GPU 映射：POST-12。
- 多 GPU Job（`--gpus N>1`，POST-01）；`gpu_request` 计数语义不变（恒 1）。
- 公平性/配额（POST-02）：跳过改变的是"暂不可满足 Job 的阻塞范围"，不引入
  优先级。

## 设计与决策依据

- [DEC-012](../decisions/DEC-012-gpu-placement-policy.md)（决策 1-6 与验证方式）。
- [DEC-005](../decisions/DEC-005-global-fifo-scheduling.md)（部分修订：第 2 条
  "队首不跳过"由"有界跳过"替代；`kAny` 间先到先得与同目标 REQUIRED 竞争的
  FIFO 决胜不变）。
- [DEC-009](../decisions/DEC-009-sqlite-state-store.md)（schema 演进纪律：
  单事务增量迁移、篡改显式失败）；
  [DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md)（身份只信 `SO_PEERCRED`，
  placement 输入不携带身份语义）。
- 设计文档 §6.1（JobSpec placement）、§7（GPU 资源模型：lease 事实与观测
  分离，RULE-05）、§9（调度：候选集过滤与有界跳过）、§12（schema v3）、
  §13（协议 v3 与 `--gpu` CLI）。
- ISSUE 真机反馈：[#10](https://github.com/Linductor-alkaid/yori/issues/10)。

## 工作项

- [x] `M9-01` Core 类型与调度语义：`gpu::GpuUuid` 提取为独立公共头（解除
  job↔gpu 头件环）；`job::GpuPlacement`（mode + devices）与
  `JobSpec` 校验（结构规则 + REQUIRED/gpu_request 互斥）；`FifoScheduler`
  重构为候选集过滤 + FIFO 有界跳过（`SchedulerConfig.scan_window`，默认 32、
  上限 4096），`ScheduleResult` 携带 `ScheduleEvaluation`（被跳过 Job、原因、
  目标与窗口截断标记），`kHeadBlocked` 语义移除并以 `kNoCandidate` 替代；
  单测覆盖 ANY/REQUIRED 选择矩阵、跳过原因、窗口截断与一致性核对。
- [x] `M9-02` IPC 协议 v3：`kProtocolVersion=3`；SUBMIT v3 尾部追加可选
  `gpu_spec`（原始 index/UUID 字符串，存在即 REQUIRED；v1/v2 帧缺省）；
  PS/QUEUE 条目尾部追加 `wait_reason` + 可选 detail（owner/admin）；
  INSPECT 追加 placement；版本矩阵（v1/v2/v3 互解、畸形、golden vector）
  与 fuzz 语料扩展。
- [x] `M9-03` daemon 提交解析与 wait_reason 派生：`IpcService::handle_submit`
  以 GPU 观测解析 `gpu_spec`（数字=index、`GPU-` 前缀=UUID；无观测
  `kNotAvailable`、未命中 `kInvalidSpec`）；`ScheduleStatusSource` 数据源
  接入 `ps`/`queue`（脱敏视图仅 reason 不含 detail）；`handle_inspect`
  填充 placement。
- [x] `M9-04` JobManager 与 Executor 承载：`JobManagerConfig.scheduler_scan_window`
  配置与校验；ManagerWorker 消费调度评估并以 comm 最新值视图
  （`LatestMailbox<ScheduleEvaluation>`）发布；JobManager 实现
  `ScheduleStatusSource`；触发链路（submit/cancel/exit/GPU 事件/恢复）不变。
- [x] `M9-05` 持久化 schema v3：`gpu_placement_mode`（TEXT NULL：
  `required`；NULL/`any` = `kAny`）与 `gpu_placement_device`（TEXT NULL，
  仅 `required` 时非空）列；v2→v3 与 v1→v2→v3 链式单事务迁移；round-trip、
  迁移与篡改（device 无 mode、非法 mode 值）负向单测；恢复一致性
  （REQUIRED Job 目标从观测消失保持 QUEUED）集成测试。
- [x] `M9-06` CLI：`submit --gpu N|UUID`（与 `--gpus` 显式互斥、本地格式
  预检）；`ps`/`queue` 展示 WAIT 列（owner/admin 附目标）；`inspect`
  placement 行；用法文本更新；退出码契约不变（本地拒绝 = 2）。
- [x] `M9-07` 集成测试与文档：issue #10 场景 1-5、8-12（ANY 正常选择；
  REQUIRED 空闲只分目标卡；目标被 lease/`EXTERNAL_BUSY`/`UNAVAILABLE`
  保持 QUEUED 不 fallback；有界跳过不全局 HOL；重启恢复 placement；
  index↔UUID 变化不错误迁移；owner/admin/脱敏与 `ps`/`queue` wait_reason
  展示）；E2E `--gpu` 提交到启动 `CUDA_VISIBLE_DEVICES` 正确；同步设计文档、
  总计划与本文档验证记录。

## 风险与阻塞

- `RISK-2026-017` FIFO 语义放宽（严格队首 -> 有界跳过）：后来者可越过暂
  不可满足的亲和 Job 启动，是 DEC-012 的核心取舍；以跳过事件 + `wait_reason`
  保证可解释，窗口大小可配置。DEC-005 修订注记与调度器测试锁定语义。
- `RISK-2026-018` `kAny` Job 可占后队 `kRequired` 目标卡（无前瞻预留）：
  已接受行为（DEC-012 影响与风险）；affinity-aware 选择延后 POST-11，
  测试记录该行为。
- `RISK-2026-019` `wait_reason` 是最近一次评估的派生视图，事件间隙可能短暂
  过时（展示语义 = "最近一次调度评估结论"）：协议与文档明示，不承诺实时。
- 协议 v3 与 v1/v2 混部：daemon 接受 v1/v2（缺省字段按无亲和/无 wait_reason
  处理），CLI 与 daemon 同版本发布；老 CLI 行为不回退。
- `GpuUuid` 公共头移动：纯文件重组（`gpu_provider.hpp` 转发包含），公共 API
  不变；public header boundary 测试复验。

## 测试与退出条件

- [x] `unit`：`m1.unit.job-spec` 扩展（placement 校验矩阵与错误码）；
  `m1.unit.fifo-scheduler` 扩展/重写（候选集过滤、有界跳过、四类 wait
  reason、窗口截断、一致性核对扩展、`kNoCandidate`）；协议 v1/v2/v3 矩阵
  （`m5.unit.ipc-protocol`）；submit 解析与 ps/queue/inspect 填充
  （`m5.unit.ipc-service`）；schema v1/v2→v3 迁移、round-trip 与篡改负向
  （`m4.unit.sqlite-state-store`）；JobManager wait_reason 发布与调度链路
  （`m7.unit.job-manager` 扩展）。
- [x] `integration`：issue #10 场景 1-5、8-12；REQUIRED 目标消失的恢复
  一致性（daemon 重启 + 观测变化）；`--gpu` 提交到 `CUDA_VISIBLE_DEVICES`
  的 E2E 断言。
- [x] `security`：脱敏视图（非 owner 非 admin）不含 placement detail（UUID），
  wait_reason 本身可见；REQUIRED/`--gpus` 互斥拒绝。
- [x] 全预设本地验证（debug/release/asan/ubsan/tsan 需 `setarch`）+ 格式 +
  tidy；Linux GCC/Clang CI 以 PR 运行为准（证据在验证记录追加）。
- [x] 文档同步：设计 §6.1/§9/§13（协议 v3 小节）、DEC-005 修订注记、
  总计划状态、CHANGELOG（随发布）。

## 验证记录

2026-09-13：M9 启动（负责人以"依照设计与计划推进下一步开发"确认依计划
推进）。DEC-012 由 Proposed 冻结为 Accepted（部分修订 DEC-005 第 2 条队首
不跳过条款）；里程碑文档创建；分支 `feat/m9-gpu-placement`。实现与验证
证据按工作项追加。

2026-09-13（实现完成）：M9-01～M9-07 全部落地于分支 `feat/m9-gpu-placement`
（基线 master `163bfc9`）。环境：Linux 6.x x86_64、Ubuntu 24.04、GCC
13.3.0、CMake 五预设；无真实 NVML GPU（CI 与真机补跑条件沿用 M7 验收矩阵，
调度语义由 Fake/Atomic GpuProvider 覆盖）。

- 验证命令与结果（全部本地执行）：
  - `cmake --preset <debug|release|asan|ubsan|tsan>` + `cmake --build` +
    `ctest --preset <...>`（tsan 经 `setarch -R`）：五个预设均
    44/44 通过（含新增 M9 场景：调度器单测重写、协议 v3 矩阵、服务层
    解析/脱敏、schema v2→v3/v1→v3 迁移、JobManager 亲和+跳过+wait_reason
    发布、恢复一致性与 `--gpu` E2E）。
  - `clang-format --dry-run --Werror`（对全部变更文件）与
    `clang-tidy -p build/debug`（同范围，18.1.3，`WarningsAsErrors` 生效）：
    无 error（修复两处：RawSql 空函数指针路径守卫——新增调用点使静态分析器
    探出既有构造路径；调度器单测指针单次绑定——规避 GCC -O3
    -Wnull-dereference 对"同参重复调用 + 短路解引用"的误报）。
- 议题映射（issue #10 十二场景中 M9 范围内 10 项）：场景 1-5（ANY 选择、
  REQUIRED 空闲只分目标卡、lease/EXTERNAL_BUSY/UNAVAILABLE 保持 QUEUED 不
  fallback）由 `m1.unit.fifo-scheduler` 覆盖；场景 8（无全局 HOL）由调度器
  单测 + `m7.unit.job-manager` M9 段（J2 越过 J1 启动）+ E2E 覆盖；场景 9
  （重启恢复 placement）由 `recovery.integration` 场景 D 覆盖；场景 10
  （index↔UUID 变化不迁移）由场景 D 的"目标从观测消失保持 QUEUED +
  AFFINITY_GPU_STATE"覆盖；场景 11（owner/admin/脱敏视图 placement）由
  `m5.unit.ipc-service` M9 段覆盖；场景 12（queue/ps 解释等待原因）由
  服务层单测 + E2E 的 WAIT 列断言覆盖。场景 6/7（PREFERRED）属 POST-11。
- 实现要点：`GpuUuid` 提取为 `include/yori/gpu/gpu_uuid.hpp`（解除
  job↔gpu 头件环，公共 API 不变）；调度一致性核对扩展为全量成员 + 窗口
  条目字段级核对；`ScheduleResultCode::kHeadBlocked` 语义移除并以
  `kNoCandidate` 替代；`mutation_core::same_spec` 补入 `gpu_placement`
  （spec 完整性检查缺口，防御性修复）；评估视图经
  `LatestMailbox<ScheduleEvaluation>`（executor comm 最新值语义）从
  JobManager worker 发布、IPC 读路径消费。
- 文档同步：设计文档 §13.2/13.3（协议 v3）、§6.1/§9/§12（M9 段落随立项
  已预置，本次确认与实现一致）；威胁模型基线 23 扩展 wait_reason 脱敏
  边界；DEC-012 Accepted、DEC-005 修订注记、总计划状态、CHANGELOG
  Unreleased 段。
- 限制与补跑：真实 NVML 多卡环境下的 `--gpu` 端到端（NVML index 与
  CUDA_VISIBLE_DEVICES 的物理对应）未在本机执行（无 GPU），补跑条件沿用
  M7 真机矩阵；Linux Clang 编译矩阵由 CI 覆盖（本机仅 GCC 13.3）。
