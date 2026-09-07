# M2：进程守护与启动适配

> 状态：In Progress
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M1（[核心域契约与进程内调度闭环](m1-core-contracts.md)）
> 建议发布点：无
> 更新日期：2026-09-08

## 目标

交付训练进程的启动与守护能力：`LaunchProfile`/`LaunchAdapter` 把调度结果映射为训练
命令环境（`CUDA_VISIBLE_DEVICES` 等），`ProcessSupervisor` 以独立进程组 spawn 训练
进程并在 exec 前完成身份降权，取消按 `SIGTERM -> grace -> SIGKILL` 升级并回收退出，
stdout/stderr 以有界 `LogSink` 捕获、落盘与轮转。全部并发路径由 pinned Executor 的
blocking worker 与 delayed task 承载（总计划 `EXEC-03`、`EXEC-07`）。

## 范围与非目标

范围：

- `LaunchProfile`（`cuda_visible_devices` / `physical_argument` 两种模式）与
  `LaunchAdapter` 公开契约与默认实现；环境变量三层合并与保留键拒绝
  （[DEC-006](../decisions/DEC-006-launch-environment-policy.md)）；提交用户身份
  （passwd 条目 + supplementary groups）经 `IdentityResolver` 在 fork 前解析。
- `ProcessSupervisor` Core 契约与 Linux 进程引擎：pipe 捕获、独立进程组
  （`setpgid`）、`close_range` 关闭继承描述符、子进程 `SIGPIPE` 忽略
  （[DEC-008](../decisions/DEC-008-daemon-restart-log-continuity.md)）、
  `setgroups -> setgid -> setuid` 降权（[DEC-004](../decisions/DEC-004-privileged-daemon-demotion.md)）、
  PATH 解析 exec、exec 结果确认管道、`/proc` 进程身份（PID/PGID/启动 ticks）。
- 取消升级状态机：`request_cancel()` 组信号 SIGTERM + 截止时刻，宽限期默认 10 秒
  （[DEC-007](../decisions/DEC-007-cancel-grace-period.md)），到期升级 SIGKILL；退出
  以 `waitpid` 回收，终态幂等，PID reuse 以启动 ticks 核验。
- `LogSink`：每 Job 独立 `stdout.log`/`stderr.log`，`0640` 权限与属主设置、单文件
  上限、轮转保留、写失败丢弃 + drop 标记；逻辑 offset 单调。
- Executor 承载：`ProcessExitMonitor`（单个 blocking worker：SIGCHLD 自管道 +
  逐 PID `waitpid(WNOHANG)`，注册/注销经 `MpscChannel`，退出事件经有界
  `MpscChannel` 投递）与 `LogPump`（单个 blocking worker：`poll` 排空多 Job 管道并
  同步写 `LogSink`）；宽限升级经 `submit_delayed` 一次性任务。
- 测试：六场景（正常完成、任务异常、提交拒绝、执行中取消、超时、shutdown）+
  进程组/孙进程终止、PID reuse、exec 失败、环境一致性、降权身份断言（root 环境，
  `multi-user` 标签）、日志上限/轮转/丢弃标记、慢生产者不阻塞训练。

非目标：

- NVML 真实接入（M3）、SQLite 持久化与 daemon 重启恢复（M4）、IPC/CLI（M5）、
  `logs -f` 流式跟随与订阅分发（M6）、systemd 打包（M7）。
- `yori-launch-helper` 拆分（`POST-09`）、pidfd（`POST-08`）。
- JobManager/daemon 总装：M2 交付可组合组件与集成测试，不修改 `yorid` 主程序行为。
- 全局磁盘预算与跟随会话上限默认值冻结（M6，总计划第 6 节）。

## 设计与决策依据

- 进程守护与取消：[设计文档](../design/yori-project-design.md)第 10 节；
  [DEC-007](../decisions/DEC-007-cancel-grace-period.md)。
- 启动适配与 GPU 映射：设计第 8 节；
  [DEC-006](../decisions/DEC-006-launch-environment-policy.md)。
- 日志捕获与落盘：设计第 11.2 节；
  [DEC-008](../decisions/DEC-008-daemon-restart-log-continuity.md)；
  [DEC-002](../decisions/DEC-002-mvp-observability.md)。
- 降权与安全边界：设计第 17 节；
  [DEC-004](../decisions/DEC-004-privileged-daemon-demotion.md)；
  [威胁模型](../security/threat-model.md)。
- Executor 承载：总计划 `EXEC-03`、`EXEC-07`；pinned integration skill 的
  Blocking I/O 与 Scheduling capability card（`wakeup` 解除阻塞、worker 生命周期、
  delayed task 句柄语义）；进程与权限语义属于 Yori 层，不构成 Executor 能力缺口。

## 工作项

- [ ] `M2-01` 冻结并实现 `LaunchProfile`/`LaunchAdapter` 公开契约与默认实现：
  GPU 映射（两种模式）、环境三层合并与保留键拒绝、`IdentityResolver` 身份解析
  （fork 前完成，含 supplementary groups），产物为有界、按键唯一排序的
  `LaunchPlan`。
- [ ] `M2-02` 实现 `ProcessSupervisor` Core 契约与 Linux 进程引擎：pipe 捕获、
  独立进程组、描述符收敛、子进程 `SIGPIPE` 忽略、`setgroups -> setgid -> setuid`
  降权、PATH 解析 exec、exec 确认管道、`/proc` 身份读取；spawn 失败全部为结构化
  结果且不留僵尸进程。
- [ ] `M2-03` 实现取消升级状态机（`DEC-007`）：SIGTERM 组信号 + 宽限截止 +
  SIGKILL 升级 + `waitpid` 回收；终态幂等，迟到升级为无害空操作，PID reuse 以
  启动 ticks 核验。
- [ ] `M2-04` 落地 Executor 承载：`ProcessExitMonitor` blocking worker（SIGCHLD
  自管道唤醒、逐 PID 回收、注册/注销/事件均经有界 `MpscChannel`，慢消费者背压
  显式可见）与 `LogPump` blocking worker（`poll` 多 Job 排空、同步落盘、
  `wakeup` 经自管道）；宽限升级用 `submit_delayed` 承载；停止顺序与句柄消费
  显式。
- [ ] `M2-05` 实现 `LogSink`：`0640` 与属主设置、单文件上限与轮转保留、写失败
  丢弃 + drop 标记、逻辑 offset 单调；权限收敛在非 root 测试环境下显式降级并可
  观察。
- [ ] `M2-06` 交付测试与验证：六场景 + M2 特有场景（进程组孙进程终止、PID reuse、
  exec 失败、环境一致性、慢生产者、日志边界）；`debug`/`release`/`asan`/`ubsan`/
  `tsan` 预设与 CI（GCC/Clang 矩阵、clang-format/clang-tidy）通过；root 环境降权
  身份断言用例以 `security;multi-user` 标签接入并在非 root 环境显式 skip。

## 风险与阻塞

- fork-exec 窗口的 async-signal-safe 纪律：子进程只调用 syscall 封装，全部 NSS/
  内存分配在 fork 前完成；评审重点检查。
- `waitpid` 逐 PID 扫描依赖 SIGCHLD 唤醒与注册后 kick 双保险；注册前退出的竞态
  必须 covered by测试。
- 本机无 clang-tidy-18 与 Clang 编译器，TSAN 需 `setarch -R`（既有环境限制）；
  以 PR CI 为最终门禁。
- 多进程测试在 sanitizer 下的稳定性（子进程不受父进程 ASAN 影响，但泄漏检测可能
  干扰）：必要时以 `ASAN_OPTIONS` 显式配置，验证记录中说明。

## 测试与退出条件

- [ ] `LaunchPlan` 契约测试通过：两种 GPU 映射模式、环境合并顺序与唯一性、
  白名单外变量不进入、保留键拒绝、无效 profile/身份错误码。
- [ ] 进程引擎测试通过：spawn -> 退出码回收；信号退出；进程组与孙进程终止；
  exec 失败结构化错误；管道内容捕获；子进程 `SIGPIPE` 为忽略；环境与 `LaunchPlan`
  一致；僵尸进程零残留。
- [ ] 取消升级测试通过：宽限期内退出不升级；忽略 SIGTERM 时升级 SIGKILL；空操作
  升级幂等；非法宽限期配置拒绝。
- [ ] `LogSink` 测试通过：`0640` 与属主（root 环境）、轮转与保留、丢弃标记、逻辑
  offset 单调、写失败不抛出且可恢复。
- [ ] Executor 承载测试通过：退出事件投递（含注册前退出竞态）、注销、stop 路径、
  日志泵 attach/detach/stop、多 Job 并发排空；六场景覆盖。
- [ ] `debug`/`release`/`asan`/`ubsan`/`tsan` 预设全部通过；PR CI（GCC 13/Clang 18
  Debug/Release、clang-format 18、clang-tidy 18、sanitizers、依赖门禁）全绿。
- [ ] 设计（第 8、10、11.2、17 节）、威胁模型、总计划（状态、暂定默认值表、
  文档地图）与本计划同步更新。

## 验证记录

