# M2：进程守护与启动适配

> 状态：Completed
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

- [x] `M2-01` 冻结并实现 `LaunchProfile`/`LaunchAdapter` 公开契约与默认实现：
  GPU 映射（两种模式）、环境三层合并与保留键拒绝、`IdentityResolver` 身份解析
  （fork 前完成，含 supplementary groups），产物为有界、按键唯一排序的
  `LaunchPlan`。
- [x] `M2-02` 实现 `ProcessSupervisor` Core 契约与 Linux 进程引擎：pipe 捕获、
  独立进程组、描述符收敛、子进程 `SIGPIPE` 忽略、`setgroups -> setgid -> setuid`
  降权、PATH 解析 exec、exec 确认管道、`/proc` 身份读取；spawn 失败全部为结构化
  结果且不留僵尸进程。
- [x] `M2-03` 实现取消升级状态机（`DEC-007`）：SIGTERM 组信号 + 宽限截止 +
  SIGKILL 升级 + `waitpid` 回收；终态幂等，迟到升级为无害空操作，PID reuse 以
  启动 ticks 核验。
- [x] `M2-04` 落地 Executor 承载：`ProcessExitMonitor` blocking worker（SIGCHLD
  自管道唤醒、逐 PID 回收、注册/注销/事件均经有界 `MpscChannel`，慢消费者背压
  显式可见）与 `LogPump` blocking worker（`poll` 多 Job 排空、同步落盘、
  `wakeup` 经自管道）；宽限升级用 `submit_delayed` 承载；停止顺序与句柄消费
  显式。
- [x] `M2-05` 实现 `LogSink`：`0640` 与属主设置、单文件上限与轮转保留、写失败
  丢弃 + drop 标记、逻辑 offset 单调；权限收敛在非 root 测试环境下显式降级并可
  观察。
- [x] `M2-06` 交付测试与验证：六场景 + M2 特有场景（进程组孙进程终止、PID reuse、
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

- [x] `LaunchPlan` 契约测试通过：两种 GPU 映射模式、环境合并顺序与唯一性、
  白名单外变量不进入、保留键拒绝、无效 profile/身份错误码。
- [x] 进程引擎测试通过：spawn -> 退出码回收；信号退出；进程组与孙进程终止；
  exec 失败结构化错误；管道内容捕获；子进程 `SIGPIPE` 为忽略；环境与 `LaunchPlan`
  一致；僵尸进程零残留。
- [x] 取消升级测试通过：宽限期内退出不升级；忽略 SIGTERM 时升级 SIGKILL；空操作
  升级幂等；非法宽限期配置拒绝。
- [x] `LogSink` 测试通过：`0640` 与属主（root 环境）、轮转与保留、丢弃标记、逻辑
  offset 单调、写失败不抛出且可恢复。
- [x] Executor 承载测试通过：退出事件投递（含注册前退出竞态）、注销、stop 路径、
  日志泵 attach/detach/stop、多 Job 并发排空；六场景覆盖。
- [x] `debug`/`release`/`asan`/`ubsan`/`tsan` 预设全部通过；PR CI（GCC 13/Clang 18
  Debug/Release、clang-format 18、clang-tidy 18、sanitizers、依赖门禁）全绿。
- [x] 设计（第 8、10、11.2、17 节）、威胁模型、总计划（状态、暂定默认值表、
  文档地图）与本计划同步更新。

## 验证记录

### 2026-09-08：M2-01～M2-05 实现与本地验证（PR 前）

- 范围：工作树 `feat/m2-process-supervision`。交付 `LaunchProfile`/
  `DefaultLaunchAdapter`/`PosixIdentityResolver`（DEC-006 三层环境合并与保留键）、
  `ProcessSupervisor` Linux 引擎（双端 setpgid、SIGPIPE 忽略、`close_range` 收敛、
  fork 前构造 argv/envp/组列表、exec 报告管道、`/proc` 身份）、取消升级状态机
  （DEC-007）、`LogSink`（0640/属主、轮转、drop 标记、offset 单调）与 Executor
  承载（`ProcessExitMonitor`、`LogPump`、`GraceEscalation`）。
- 测试：新增 7 个测试目标（launch-adapter、process-supervisor、log-sink、
  exit-monitor、log-pump、integration、security 降权）覆盖六场景（正常完成、
  任务异常=exec/chdir 失败结构化错误、提交拒绝=无效 plan/重复 spawn/无效宽限、
  执行中取消=SIGTERM 组信号、超时=宽限到期升级 SIGKILL、shutdown=monitor/pump
  stop 不终止训练）与 M2 特有场景（进程组孙进程终止、PID 身份 ticks、注册前退出
  竞态、环境一致性、管道断裂后子进程存活、轮转分块与丢弃标记、多 Job 并发排
  空、monitor 回收权与 supervisor 单回收纪律）。
- 验证：Linux x86_64、内核 `7.0.0-31-generic`、GCC 13.3.0、Executor pin
  `4fd8e6097879`。`debug`/`release`/`asan`/`ubsan`/`tsan` 五预设全部执行
  configure/build/ctest（TSAN 按 CI 规定以 `setarch -R ctest --preset tsan` 执行），
  每套 25 个用例为 18 passed、7 个环境/后续里程碑占位用例 skipped（GPU、
  multi-user 两个、IPC、recovery、fuzz、performance），无失败、无 race 报告。
  clang-format-18 全量格式化已执行。`cmake --install build/debug --prefix
  build/m2-install` 后 `tests/consumer` 仅使用安装产物配置/编译/运行通过，新增
  launch/process/observe 公共 API 进入 consumer 冒烟；`public_header_boundary_test`
  已覆盖 `log_sink.hpp`。
- 限制：本机无 clang-tidy-18 与 Clang 编译器，PR CI 尚未触发，`M2-01`～`M2-06`
  保持未勾选；`m2.security.process-demotion` 在非 root 环境显式 skip（补跑条件：
  root 下设置 `YORI_DEMOTION_TEST_UID`/`YORI_DEMOTION_TEST_GID` 后 `ctest -L
  multi-user`）。负责人：Linductor-alkaid；补跑条件：PR CI 的 GCC 13/Clang 18
  Debug/Release 矩阵、clang-format/clang-tidy 18 与 sanitizers 全绿后勾选。
- 同步：设计第 8、10.2、11.2、17 节（v0.6）、威胁模型基线 16-19 与待完成项、
  总计划第 1/5/6/11 节、DEC-006/007/008、安装 consumer 与本计划。未修改
  `third_party/`，未发现 Executor 能力缺口（blocking worker 名称单次注册语义
  按其 DuplicateName 语义以实例唯一名适配，属应用侧职责）。

### 2026-09-08：M2-01～M2-06 PR CI 收尾

- 范围：PR [#3](https://github.com/Linductor-alkaid/yori/pull/3)，提交 `f0b26fc`～
  `b80966c`（M2 计划与决策、launch/process/observe/runtime 实现、集成测试、
  设计/威胁模型同步、格式修复与 clang-tidy 门禁修复）。
- 首轮 CI run [34150959478](https://github.com/Linductor-alkaid/yori/actions/runs/34150959478)
  暴露 2 类问题：新增 `src/launch|observe|process` 目录未纳入本地格式化批次
  （修复格式化流程并提交 `0d99cfc`）；clang-tidy 以 error 报出 14 处
  （`access` 返回值忽略、字符串拼接临时、枚举基类型、结构体填充、无效
  `std::move`、optional 未检查访问、整型乘法加宽），逐项修复并复验
  （`b80966c`），未改变产品行为。
- 最终 CI run
  [34151784269](https://github.com/Linductor-alkaid/yori/actions/runs/34151784269)
  8/8 全绿：clang-format 18、clang-tidy 18、GCC 13/Clang 18 的
  Debug/Release configure/build/test/install/consumer、ASAN/UBSAN/TSAN，以及
  依赖 pin 门禁全部通过。CI 环境每套 ctest 为 18 passed、7 个环境/后续里程碑
  占位用例显式 skipped（含 root-only 降权用例；补跑条件见威胁模型待完成项）。
- 结论：`M2-01`～`M2-06` 的实现、文档与适用门禁证据完整，工作项与退出条件
  勾选完成，M2 里程碑标记 `Completed`。root 环境降权身份断言保持独立补跑项
  （不阻塞 M2 范围，由威胁模型跟踪）。
