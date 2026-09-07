# DEC-007：Job 取消宽限期默认 10 秒与升级语义

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Linductor-alkaid
> 冻结里程碑：M2
> 替代/被替代：无

## 背景与问题

`cancel` 必须终止完整训练进程组（设计第 10.2 节、总计划 `SCOPE-07`）：先向进程组发送
`SIGTERM`，等待宽限期后仍未退出则 `SIGKILL`。宽限期过长会让取消体验迟滞，过短会剥夺
训练程序做 checkpoint/清理的机会。总计划第 6 节将默认值冻结点定在 M2。

## 决策

- 默认宽限期 **10 秒**（`CancelPolicy::kDefaultGracePeriod`），可配置；有效下限
  `kMinGracePeriodMs = 100`，低于下限的配置被拒绝（防止配置错误把升级变成立即击杀），
  上限 `kMaxGracePeriodMs = 600000`（10 分钟）。
- 取消语义是**外部进程语义**，不与 Executor 任务取消混同（AGENTS.md 第 6 条）：
  `request_cancel()` 对整个进程组（`kill(-pgid, SIGTERM)`）发送 SIGTERM 并记录
  截止时刻；宽限期由 Executor delayed task 承载，到期后检查进程仍在运行才升级为
  `kill(-pgid, SIGKILL)`。进程在宽限期内自然退出时，升级动作必须是无害空操作。
- 宽限deadline 是**软期限**：delayed task 允许抖动，不构成抢占保证（工程规范 9.2 第
  6 条）；升级前进程可能已退出，终态以 waitpid 回收结果为准且幂等。
- `STOPPING` 期间进程组内子进程可能先于组长退出，回收与终态判定只针对组长 PID；
  进程组整体存活性由组信号语义保证（对组长的 `SIGKILL` 与对组的 `SIGKILL` 同发）。

## 备选方案

- 无宽限期直接 `SIGKILL`：训练无法保存状态，用户损失大，被否决。
- 宽限期可配置为零：与"立即击杀"等价且容易被误配置，设置下限 100ms 拒绝。
- 以轮询判定升级：违反调度器事件驱动约束（`RULE-07` 同源精神），且增加常驻唤醒；
  delayed task 一次性触发更符合 Executor 承载方式（总计划 `EXEC-07`）。

## 影响与风险

- 10 秒内 daemon 若崩溃，升级定时随之消失，进程组可能停留在已收到 SIGTERM 的状态；
  由 M4 恢复流程以进程身份核验收敛（进入 `LOST` 或继续终止）。
- 训练程序忽略 SIGTERM 时，最坏 10 秒后才被击杀，属预期行为。

## 验证方式

M2 测试覆盖：SIGTERM 后宽限期内退出（无升级）；忽略 SIGTERM 的进程组在宽限期后收到
`SIGKILL`（`WIFSIGNALED` 且 signal 为 SIGKILL）；进程组内孙进程一并终止；已退出进程的
升级动作为空操作；非法宽限期配置被拒绝。

## 关联文档和工作项

设计第 10.2 节；总计划第 6 节暂定默认值表、`SCOPE-07`、`EXEC-07`；M2 工作项 `M2-02`、
`M2-03`。
