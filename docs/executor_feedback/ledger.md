# Executor 能力缺口反馈台账

> 状态：Active
> 版本：1.0（初始，尚无记录）
> 创建日期：2026-09-02
> 规则依据：[AGENTS.md「Executor 能力缺口与反馈台账」](../../AGENTS.md)、
> [项目管理与工程规范](../project/project-standards.md)第 9.4 节

## 1. 登记规则（摘要）

- 登记前先核对当前 pinned 版本（`v0.4.0-82-g4fd8e60`）的公开头文件、
  `third_party/executor/docs/API.md`、集成 SKILL 及相关测试，排除 API 选型错误、
  配置错误、平台限制与应用层职责。
- 仅记录真实能力缺口，必须包含：缺失的行为、造成缺口的 API/语义限制、为什么
  现有 lifecycle 与 comm 设施不足、建议的最小 Executor 能力或获批例外、延期
  影响、可复现证据与可验收结果。只写"Executor 不支持"不构成有效记录。
- 临时方案必须限制在单一 Adapter/compatibility boundary 内，说明行为差异、风险、
  移除条件与测试覆盖；临时方案不得创建线程、队列或调度器。
- 未经明确授权不修改 `third_party/executor` 来掩盖集成问题；不把项目特有策略
  下沉到通用 Executor。
- 以下不是能力缺口，在 Yori 对应层解决：NVML/GPU 驱动适配、Linux 进程与权限
  语义（UID/GID 切换、`SO_PEERCRED`、信号、进程组）、UDS 与 systemd 集成、
  调度与队列策略、外部观测工具托管方式、错误使用已有 API、平台权限或设备缺失。

## 2. 记录索引

| 编号 | 日期 | 等级 | 状态 | 标题 | 引用位置 |
| --- | --- | --- | --- | --- | --- |

当前无记录。

- 等级：`P1` 系统性将就（影响整个代码面的派发可见性）/ `P2` 结构性将就（某子
  系统整体绕开设施）/ `P3` 有而未用（应用侧待办，非缺口）/ `违规`（应用违反
  AGENTS.md，需自行整改）。
- 状态：`Open` / `Proposed` / `Accepted` / `Resolved` / `Rejected`。
- 上游收敛后回写迁移结论与证据，状态更新为 `Resolved`。

## 3. 条目模板

```md
## EXE-YYYYMMDD-NNN：标题

- 等级：P1 | P2 | P3 | 违规
- 状态：Open | Proposed | Accepted | Resolved | Rejected
- 日期：YYYY-MM-DD；负责人：
- 缺失行为：
- 造成缺口的 API/语义限制：
- 为什么现有 lifecycle 与 comm 设施不足：
- 建议的最小能力或获批例外：
- 临时方案（如有）、行为差异与移除条件：
- 影响范围与延期影响：
- 可复现证据：
- 可验收结果：
- 引用位置（代码/测试/设计）：
- 跟进记录：按日期追加；上游收敛后记录迁移结论与证据。
```
