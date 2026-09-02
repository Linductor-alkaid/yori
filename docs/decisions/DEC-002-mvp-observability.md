# DEC-002：MVP 纳入训练观察面（logs -f 与 tensorboard）

> 状态：Accepted
> 日期：2026-09-02
> 负责人：Linductor-alkaid
> 冻结里程碑：M0（随设计 v0.2 生效）
> 替代/被替代：无

## 背景与问题

训练从用户交互会话（tmux/SSH）移入后台 daemon 后，用户原有的两个观察入口——
终端日志与 TensorBoard——消失。若 MVP 只提供一次性日志快照，观察体验相对原有
tmux 工作流是倒退，用户没有理由放弃手动占卡（[设计文档](../design/yori-project-design.md)
第 11 节引言）。设计 v0.1 的 MVP 范围仅含快照日志，v0.2 将观察面纳入 MVP。

## 决策

MVP 范围纳入：

- `yori logs [-f]`：流式跟随。每帧携带逻辑 offset 支持断线续传；控制帧含
  `GAP`（轮转丢弃标记）、`EOF`（终态排空）、`BACKPRESSURE`（慢客户端断开）。
- `yori tensorboard <job-id>` 观察入口（承载方式见
  [DEC-003](DEC-003-tensorboard-cli-hosting.md)）。
- 配套能力：日志落盘与轮转、每订阅者有界发送缓冲、活跃跟随会话数上限、
  `ps`/`queue` 非自有 Job 信息脱敏（设计第 11.5 节）。

## 备选方案

- MVP 仅快照日志、观察面延后：接受度风险（见背景），用户可能继续手动占卡。
- daemon 托管 TensorBoard：需要为非训练、可长期存活的进程建立第二种进程模型、
  端口资源表与恢复语义，MVP 复杂度与攻击面高（设计第 11.6 节）。
- 解析日志/指标提供"更好"的观察：违反"Yori 不理解训练语义"的核心边界。

## 影响与风险

- IPC 协议必须包含流式帧与背压语义（设计第 13.2 节），日志轮转与 offset 续传
  增加实现复杂度（M6）。
- 落盘与跟随路径的容量上限成为强制约束（总计划 `RULE-08`）。

## 验证方式

[设计文档](../design/yori-project-design.md)第 19 节判据中 `logs -f` 与
`tensorboard` 两项；M6 退出条件含慢客户端 `BACKPRESSURE` 负向测试与轮转后
offset 续传测试。

## 关联文档和工作项

设计第 11、13、19 节；[DEC-003](DEC-003-tensorboard-cli-hosting.md)；
[总计划](../plans/yori-implementation-plan.md) `SCOPE-08`/`SCOPE-09` 与里程碑
M6。
