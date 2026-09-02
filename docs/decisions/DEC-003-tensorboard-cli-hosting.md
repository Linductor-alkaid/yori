# DEC-003：TensorBoard 由 CLI 用户会话拉起，daemon 只读提供 logdir 解析

> 状态：Accepted
> 日期：2026-09-02
> 负责人：Linductor-alkaid
> 冻结里程碑：M0（随设计 v0.2 生效）
> 替代/被替代：无

## 背景与问题

TensorBoard 是用户既有的指标可视化习惯，观察面纳入 MVP 后（
[DEC-002](DEC-002-mvp-observability.md)）必须确定承载位置。若由 daemon 托管，
需要为"非训练、可能长期存活、按用户隔离端口"的一类进程建立第二种进程模型、
端口资源表与恢复语义，MVP 收益低、攻击面高（[设计文档](../design/yori-project-design.md)
第 11.6 节）。

## 决策

- `yori tensorboard <job-id>` 由用户在自己的会话中运行；CLI 直接以当前用户身份
  spawn `tensorboard --logdir <dir> --port <p>`，前台运行，CLI 退出即终止。
- daemon 仅提供只读 IPC 查询，向 owner/admin 返回 logdir 解析结果；解析优先级：
  `--logdir` 参数 > JobSpec 的 `tensorboard_logdir` > Job `cwd`。
- 默认仅监听 `127.0.0.1`（用户经 SSH 端口转发访问）；绑定更大范围必须显式
  `--host`。
- 端口默认由 OS 分配（bind 端口 0），CLI 打印最终访问 URL；`--port` 指定时冲突
  则报错退出。
- TensorBoard 进程属于用户观察会话，不是受调度 workload：不占 GPU lease、不进
  全局队列。用户需要常驻面板时自行配合 `tmux`/`nohup`。
- 未来若出现常驻指标面板需求，演进为 daemon 管理的辅助进程类型，届时必须新建
  设计与决策记录（总计划 `POST-10`）。

## 备选方案

- daemon 托管 TensorBoard：第二进程模型 + 端口资源表 + 恢复语义，复杂度与攻击
  面不匹配 MVP 收益。
- 不提供入口，用户手动运行 TensorBoard：丢失 JobId 到 logdir 的关联与
  owner/admin 授权语义。

## 影响与风险

- 用户断开 SSH 即失去面板。缓解：属用户观察会话语义，文档明确说明配合
  `tmux`/`nohup`；不将其混入 daemon 托管范围。

## 验证方式

M6 测试：默认监听地址为 `127.0.0.1`；`--host` 显式放开生效；`--port` 冲突报错；
CLI 退出后 TensorBoard 进程终止；logdir 解析优先级正确；非 owner/admin 的查询
被拒绝。

## 关联文档和工作项

设计第 11.6、13.1 节；[DEC-002](DEC-002-mvp-observability.md)；
[总计划](../plans/yori-implementation-plan.md) `SCOPE-09`、M6、`POST-10`。
