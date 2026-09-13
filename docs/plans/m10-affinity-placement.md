# M10：亲和感知 ANY 设备选择

> 状态：Completed（2026-09-13 PR
> [#23](https://github.com/Linductor-alkaid/yori/pull/23) 合并于 master
> `dbdd0d0`（负责人授权）；随 `v0.4.0` 发布）
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M9（GPU placement 亲和调度，PR [#20](https://github.com/Linductor-alkaid/yori/pull/20)）
> 决策依据：[DEC-013](../decisions/DEC-013-affinity-aware-any-placement.md)（Accepted，2026-09-13
> 随 M10 启动冻结；扩展 [DEC-012](../decisions/DEC-012-gpu-placement-policy.md) 决策 3）
> 需求来源：[Issue #22](https://github.com/Linductor-alkaid/yori/issues/22)（POST-11 中
> "affinity-aware ANY 设备选择"子项的提前实现）
> 建议发布点：`v0.4.0`（2026-09-13 发布，负责人授权）
> 更新日期：2026-09-13（PR #23 合并，v0.4.0 发布收口）

## 目标

1. **消除 placement 碎片**：`kAny` Job 在多个当前可用 GPU 之间选择时，
   避开同一 FIFO 扫描窗口内等待中 REQUIRED Job 指定的 GPU（软保护），
   把稀缺的特定 GPU 留给只能使用它的任务。
2. **不降低利用率**：不存在非冲突候选时回退完整候选集，绝不因为等待中的
   REQUIRED 而人为空闲 GPU（不演变成隐式 reservation）。
3. **选择结论可观察**：`SchedulerEvent.selection_reason`
   （`DEFAULT`/`AVOID_REQUIRED_AFFINITY`/`AFFINITY_FALLBACK`）+
   `JobManagerStats` 计数，解释 ANY Job 为何偏离 lowest-index。
4. **契约零变更**：FIFO 服务顺序、bounded skip、REQUIRED 绝不 fallback、
   lease 权威性、观测与 lease 分离、IPC 协议与 schema 均不变。

## 范围与非目标

范围（DEC-013 决策 1-5）：

- `FifoScheduler`：扫描窗口内预计算 `affinity_targets`（QUEUED 的 REQUIRED
  目标集合）；`kAny` 候选 ranking——先最小化与等待 REQUIRED 目标的冲突、
  再物理 index 升序决胜；非冲突候选为空时回退。
- `SchedulerEvent` 新增 `selection_reason`（仅诊断语义，不进 IPC 协议、
  不持久化、不新增 Job/GPU 状态）；`JobManagerStats` 新增
  `scheduler_affinity_avoids`/`scheduler_affinity_fallbacks` 计数。
- 测试：issue #22 建议测试矩阵 10 条场景（Core 矩阵 + 观测驱动集成 +
  总装回退路径）。
- 文档：DEC-013、DEC-012 同步、设计文档 §9/§16.2、总计划、CHANGELOG。

非目标：

- 硬 reservation / backfill：属于未来多资源组合调度（POST-01/POST-03 之后
  统一设计）。
- `PREFERRED` 模式、GPU Set（`--gpu-any-of`）：仍属 POST-11 余项。
- IPC/CLI 面变化：`ps`/`queue` 的 `wait_reason` 不变，无新 wire 字段。
- 持久化/schema 变化：保护集合全部由队列派生，无新存储。

## 设计与决策依据

- [DEC-013](../decisions/DEC-013-affinity-aware-any-placement.md)（决策 1-5
  与验证方式；备选方案含硬 reservation 的否决理由）。
- [DEC-012](../decisions/DEC-012-gpu-placement-policy.md)（placement 模型、
  候选集过滤与有界跳过——本里程碑只扩展其决策 3 的 kAny 选择规则）。
- [DEC-005](DEC-005-global-fifo-scheduling.md)（FIFO 服务顺序不变）。
- 设计文档 §9（调度流程：M10 亲和感知 ranking 段）、§16.2。

## 工作项

- [x] `M10-01` `FifoScheduler` 亲和感知 `kAny` 选择：窗口内 `affinity_targets`
  预计算 + 候选 ranking（冲突优先级、物理 index 决胜）+ 回退语义，公开
  `GpuSelectionReason` 与 `SchedulerEvent.selection_reason`。
- [x] `M10-02` `JobManagerStats` 亲和选择计数（avoids/fallbacks），调度结果
  消费路径归类递增。
- [x] `M10-03` issue #22 测试矩阵 10 场景落地（Core 单测、GpuManager 观测驱动
  集成、JobManager 总装回退路径与计数断言）。
- [x] `M10-04` 文档同步：DEC-013 新建、DEC-012 风险条目与关联更新、设计文档
  §9/§16.2、总计划（状态/POST-11/里程碑索引）、CHANGELOG。

## 风险与阻塞

- 无环境限制项：全部验证（GCC/Clang、ASAN/TSAN、格式与静态检查）本地与
  CI 均可执行；Ubuntu 22.04 兼容构建按惯例依赖 CI runner。

## 测试与退出条件

- [x] issue #22 矩阵 10 场景全部通过（`unit_fifo_scheduler_test`：
  替代时避开 1/3/7、无替代回退 2/4、REQUIRED 在前 5、目标忙无保护效果 6、
  同目标去重与 FIFO 决胜 7、窗口外不保护 8、取消无残留 9、全命中回退
  可观察 10）。
- [x] `integration_gpu_manager_scheduler_test`：观测驱动下 ANY 避开 REQUIRED
  目标且后者随后获得目标卡（lease 事实不破坏）。
- [x] `unit_job_manager_test`：总装 fallback 路径（利用率优先）+ 计数器
  断言 + J2 保持 QUEUED。
- [x] 全量 debug/ASAN/TSAN 套件通过；clang-format 18.1.3 与 clang-tidy
  干净；CI（含 ubuntu-22.04 构建）全绿。
- [x] 文档同步完成（DEC-013/DEC-012/设计/总计划/CHANGELOG）。

## 验证记录

2026-09-13：M10 实现与验证（PR
[#23](https://github.com/Linductor-alkaid/yori/pull/23)，分支
`feat/m10-affinity-placement`，提交 `52e0e38` scheduler + `77d020b` job
计数 + `31eec80` 文档）。

- **环境**：开发机 Ubuntu 24.04（glibc 2.39）、GCC 13.3、CMake 3.28、
  pip clang-format 18.1.3 / clang-tidy 18.1.8（本地门禁复现口径）。
- **本地验证**：`ctest --preset debug|asan|tsan`（tsan 经 `setarch -R`）
  均 44/44 通过、0 失败（4 项环境相关跳过与 master 基线一致：process
  demotion、NVML 真机、两 example 项）；`clang-format --dry-run --Werror`
  touched 文件干净；touched 文件 clang-tidy 无 error 类别
  （bugprone/clang-analyzer/performance/portability）新增（apps/ 与部分
  src 的 misc-include-cleaner 等非 error 类别告警为存量，与本变更无关）。
- **CI（[run 34748713087](https://github.com/Linductor-alkaid/yori/actions/runs/34748713087)）**：
  9/9 全绿——clang-format、clang-tidy、gcc-13/clang-18 × debug/release、
  sanitizers（asan+ubsan+tsan）、deb 打包冒烟（ubuntu-22.04 / gcc-12 符号
  版本红线）、依赖锁定校验。
- **限制与剩余项**：无。合并与发版已收口：PR #23 于 2026-09-13 经负责人
  授权合并（master `dbdd0d0`），issue #22 随交付关闭；随发版 PR 定稿
  `v0.4.0`（CHANGELOG 定稿、project VERSION 0.4.0、README M10 版本标注）。
