# M5：IPC 与 CLI

> 状态：Completed
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M3（[NVML 真实 GPU 集成](m3-nvml-gpu-integration.md)）、M4（[持久化与恢复](m4-persistence-recovery.md)）
> 建议发布点：无
> 更新日期：2026-09-09

## 目标

交付 CLI 与 daemon 之间的请求/响应通道：IPC 协议 v1 Core 契约（长度前缀帧、
显式边界与错误语义）、UDS 传输适配（`SO_PEERCRED` 鉴权、端点权限收敛、
EXEC-02 blocking worker 承载）、daemon 侧请求服务（submit/ps/queue/gpu/
cancel/logs 快照，owner/admin 授权与信息脱敏）、M5 子集的 daemon 总装与
`yori` CLI 命令集，并以确定性 fuzz 集起步 IPC parser 覆盖。端点与权限语义
冻结为 [DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md)。

## 范围与非目标

范围：

- 协议 Core 契约（`include/yori/ipc/ipc_protocol.hpp`，`yori_core`）：
  - 帧格式：`[u32 LE payload_length][payload]`，payload =
    `[u8 version=1][u8 kind][kind body]`；负载上限 1 MiB，逐字段长度/计数/
  NUL/尾部字节校验，解码错误为稳定枚举（`kTruncated`/`kOversize`/
    `kBadVersion`/`kBadKind`/`kBadString`/`kTooManyItems`/`kTrailingBytes`/
    `kInvalidValue`）。
  - 请求 kind：`SUBMIT`（argv/cwd/env/gpu_request/launch_profile/
    tensorboard_logdir；**不携带任何身份字段**，owner 只来自 `SO_PEERCRED`）、
    `PS`、`QUEUE`、`GPU`、`CANCEL`、`LOGS`（快照，含每流 max_bytes）。
  - 响应：统一错误码（NONE/PROTOCOL/UNSUPPORTED/DENIED/INVALID_SPEC/
    QUEUE_REJECTED/STORE_FAILED/NOT_FOUND/INVALID_STATE/NOT_AVAILABLE/
    LIMIT/INTERNAL）+ detail 字符串 + 按 kind 的结果体；错误响应也携带可用的
    上下文字段（如 CANCEL 的当前状态），供 CLI 呈现。
- 传输契约与 UDS 适配：
  - `include/yori/ipc/ipc_transport.hpp`（Core 抽象）：`PeerCredentials`、
    daemon 侧 `IpcRequestHandler`、`IpcServerTransport`/`IpcClientTransport`。
  - `yori_ipc` 适配库（无 Executor）：`UdsIpcClient`（同步 connect -> 帧请求
    -> 帧响应 -> close，连接/超时/协议/EOF 显式错误）。
  - `yori_runtime` 的 `UdsIpcServer`（EXEC-02）：单 blocking worker
    （poll listen fd + 唤醒管道），accept 时 `SO_PEERCRED`，串行处理每连接
    一请求，读/写有截止时间；解析失败回 PROTOCOL 错误帧后断开；handler
    异常映射 INTERNAL 响应（不吞）。端点按 DEC-010 收敛权限；陈旧 socket
    仅在确为 socket 文件时替换。停止 = 唤醒 + join + unlink，幂等。
- daemon 侧请求服务（`include/yori/ipc/ipc_service.hpp`，Core）：
  - `IpcService : IpcRequestHandler`，单 owner 同步 Core；数据源（GPU 快照、
    日志尾部读取）经接口注入，不触碰平台类型。
  - submit：JobSpec 以 peer 身份构造 -> `job::validate`（root owner 拒绝等）
    -> JobId 分配（快照最大 id+1，不复用终态 id）-> store 创建 -> 队列准入
    （容量拒绝显式回滚为 CANCELLED）。
  - ps/queue/gpu：快照/队列视图 + lease 合并的 GPU 逻辑状态
    （`derive_logical_state`）；无观测时 GPU 显式 `NOT_AVAILABLE`。
  - cancel：owner/admin 授权；QUEUED -> 持久化 CANCELLED + 队列移除（幂等）；
    已 CANCELLED 幂等成功；其他终态/活动态显式 `INVALID_STATE`/`UNSUPPORTED`。
  - logs 快照：owner/admin 授权；未启动显式 `INVALID_STATE`；有界尾部读取
    （每流上限，请求值取 min），超限置 truncated 标记。
  - 脱敏（设计 11.5）：非 owner 非 admin 的 Job 仅暴露 JobId/状态/revision/
    owner_uid/退出状态，argv/cwd/tensorboard_logdir 置空并打 masked 标记；
    授权全部在 daemon 侧基于 `SO_PEERCRED` 判定。
  - admin 判定按 DEC-010：主 GID 匹配或 UID 属于启动时解析的 admin 组成员集。
- daemon 总装（M5 子集，`yori_runtime` 的 `Daemon`）与 `yorid` 主生命周期：
  启动序 = 恢复（`JobRecovery`，同步有界，RULE-06）-> GPU 观察启动
  （`GpuManager`，EXEC-05/09）-> IPC 服务（EXEC-02）；停止序按 EXEC-10 适用
  子集 = ① IPC 停止 -> ③ GPU 周期停止；Executor owner 仍为 `yorid` 主生命
  周期（`ExecutorRuntime`）。`yorid` 以 `sigwait` 等待 SIGTERM/SIGINT。
- `yori` CLI（无状态，不创建 Executor）：`submit`（`--gpus` 仅 1、
  `--tensorboard-logdir`、`--cwd`、`--env K=V`、`--` 分隔命令）、`ps`、
  `queue`、`gpu`、`cancel`、`logs`（快照；`-f` 显式报 M6）、全局
  `--socket`/`YORI_SOCKET`（默认 `/run/yori/yori.sock`）；退出码 0 成功 /
  1 请求失败 / 2 用法错误 / 3 传输失败。
- 测试：协议单测（roundtrip + 畸形矩阵 + 边界值）、服务单测（授权矩阵/
  脱敏/回滚/幂等）、服务器生命周期与安全负向（陈旧非 socket 文件、超时
  断开、超载帧拒绝）、fuzz 起步（确定性种子变异集，无 crash 且错误稳定）、
  集成 E2E（进程内 `Daemon` + 真实 CLI 子进程走 UDS 全链路）；替换
  `example.ipc.protocol` 与 `example.fuzz.parser` 占位。
- 文档：设计第 13 节（协议 v1 与承载细化）、第 5 节（admin 判定引用
  DEC-010）、[DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md)、威胁模型
  （基线 2/6/9/12 的 M5 证据 + IPC 威胁细化收口 + 新增基线 22/23）、总计划
  （第 1/5/6/11 节）与本计划。

非目标：

- `logs -f` 流式帧、offset 续传、`GAP`/`EOF`/`BACKPRESSURE` 与跟随会话
  背压（EXEC-03/EXEC-04）——M6。
- `yori tensorboard`（DEC-003，CLI 侧拉起）——M6。
- 守护总装的后半部分：调度触发接入（`SchedulerTaskRunner` 与 IPC 的联动）、
  进程启动/退出回收/STOPPING 重取消（M4 计划中的"随总装接入"项因此顺延至
  M7 前的守护总装收口）、`EXEC-09` 启动 `PhaseGate`。M5 daemon 不启动训练
  进程，Job 停留在 QUEUED，cancel 仅覆盖排队期语义。
- systemd unit、安装打包、`/run/yori` 目录创建与生产权限收敛验证——M7
  （CI 无 root/多用户环境，按 DEC-010 记录补跑条件）。
- 持久化写路径的异步串行化：M5 的 IPC 服务为单 worker 串行同步写
  （单写者无竞态）；`StoreTaskRunner` 接入随守护总装收口。
- libFuzzer/持续 fuzz 基础设施——M5 交付进程内确定性 fuzz 集（起步），
  独立 fuzz runner 另行立项。
- 多节点 Heyaki transport（设计第 15 节）。

## 设计与决策依据

- 协议与传输选型：设计第 13 节（UDS、请求/响应先行、流式 M6；全部输入经
  边界校验与 fuzz 覆盖）、第 17 节第 2/6/7/9 条（身份内核机制、socket 权限、
  owner/admin 授权、parser 最小化）。
- 端点与权限：[DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md)（本里程碑
  冻结，替换总计划第 6 节暂定值）。
- 鉴权与脱敏：设计第 11.5 节（owner 查看/跟随自有 Job；admin 查看全部；
  非自有信息脱敏为 JobId/状态/资源占用；判定在 daemon 侧）。
- Executor 承载：总计划 `EXEC-02`（连接接受与请求读取 = blocking worker，
    wakeup 解除 accept/read 阻塞，关闭阶段 ① 停止新连接与请求生产者）。
  串行 accept-服务循环在 M5 的 CLI 负载下足够；慢客户端由请求截止时间
  有界化，超时断开即显式 EOF。CLI 无异步工作，不创建 Executor（AGENTS.md
  owner 语义）。
- 状态与幂等：`RULE-04`（终态幂等——已 CANCELLED 的重复 cancel 幂等成功，
  其余终态显式拒绝）、`RULE-08`（帧/列表/日志尾部全部有界，拒绝与截断显式
  转化为结果与事件）。
- 恢复接入：M4 的 `JobRecovery` 作为 daemon 启动序第一步（RULE-06；活动 Job
  采纳为 RUNNING 后，cancel 返回 `UNSUPPORTED` 直至守护总装收口——语义
  显式而非假装支持）。
- JobId 分配：服务器级单调（快照最大 id + 1），终态 id 不复用；与 DEC-005
  的排序键 `(submit_time, JobId)` 兼容。
- 协议方向：身份字段不出现在请求结构中（结构性防伪造，威胁模型基线 2 的
  负向即"无可消费字段"）；字符串禁止 NUL；解码严格消费全部输入。

## 工作项

- [x] `M5-01` 冻结并实现 IPC 协议 v1 Core 契约：帧编解码、六种请求/响应
  kind、显式边界与错误枚举、协议级上限常量；编解码不触碰平台类型。
- [x] `M5-02` 实现 UDS 传输适配与端点治理：`IpcTransport` Core 抽象、
  `UdsIpcClient`（`yori_ipc`）、`UdsIpcServer`（EXEC-02 blocking worker、
  `SO_PEERCRED`、DEC-010 权限收敛、陈旧端点替换纪律、停止幂等）；冻结
  DEC-010。
- [x] `M5-03` 实现 daemon 侧请求服务：submit/ps/queue/gpu/cancel/logs 快照
  的授权矩阵与脱敏、JobId 分配、容量拒绝回滚、终态幂等、日志有界尾部。
- [x] `M5-04` 交付 M5 子集 daemon 总装与 CLI：`Daemon`（恢复 -> GPU 观察 ->
  IPC，EXEC-10 子集停止序）接入 `yorid` 主生命周期；`yori` 六命令与退出码
  契约。
- [x] `M5-05` 交付测试与文档：协议/服务/服务器/fuzz/E2E 五组测试（替换两个
  占位用例）；设计、DEC-010、威胁模型、总计划与本计划同步；五预设与 PR CI
  通过。

## 风险与阻塞

- 单 worker 串行服务的队头阻塞：慢客户端占用 worker 时其他请求排队等待其
  截止时间（默认 5s）。M5 接受该有界代价（CLI 负载低、行为可预期）；若
  M6 流式会话需要并发，再以 Executor 多 worker/会话化承载并同步 EXEC-02。
- `SO_PEERCRED` 主 GID 与文件权限组语义的差异：由 DEC-010 的补充组成员
  解析弥合；解析依赖系统组数据库，CI 仅能覆盖注入路径，真实 NSS 行为在
  M7 root 环境补跑。
- 协议演进：v1 字段布局不预留可选位，扩展以新 kind 或 version=2 + 显式
  协商（M6 流式帧引入 version 门禁），golden vector 随 M6 建立。
- 本机无 clang-tidy-18 与 Clang 编译器；TSAN 需 `setarch -R`（既有环境
  限制），以 PR CI 为最终门禁（M4 已验证 pip 安装 clang-tidy 18.1.8 可
  本地复现门禁）。

## 测试与退出条件

- [x] 协议单测通过（`m5.unit.ipc-protocol`）：六种请求/响应 roundtrip；
  畸形矩阵（截断、超载、坏版本、坏 kind、坏长度、NUL、计数超限、尾部
  多余字节）逐项稳定错误码；边界值（空 env、单元素、上限字符串）合法。
- [x] 服务单测通过（`m5.unit.ipc-service`）：owner/admin/第三方授权矩阵与
  脱敏字段断言；submit 校验映射（root owner 拒绝、非法 argv/env/cwd/
  logdir）；容量拒绝且回滚可审计；cancel 幂等与终态矩阵；logs 未启动/
  尾部/截断；gpu 快照缺失与 lease 合并；JobId 单调不复用。
- [x] 服务器测试通过（`m5.unit.ipc-server`）：生命周期（重复启动、幂等
  停止、unlink）；非 socket 陈旧文件拒绝；超载帧回 PROTOCOL 错误帧；静默
  连接超时断开（可配置短截止时间）；权限收敛断言（非 root 下降级语义）。
- [x] fuzz 集通过（`m5.fuzz.ipc-parser`）：种子化变异集（翻转/截断/超载/
  随机）解码永不 crash、错误码稳定，sanitizer 下运行。
- [x] 集成 E2E 通过（`m5.integration.ipc-e2e`）：进程内 `Daemon`（内存
  store + 伪 provider）+ 真实 `yori` CLI 子进程：submit -> queue/ps 可见
  -> cancel -> 再查确认；gpu 视图；logs 对未启动 Job 显式报错；断连/
  无 daemon 时 CLI 退出码 3。
- [x] `debug`/`release`/`asan`/`ubsan`/`tsan` 预设全部通过；PR CI
  （GCC 13/Clang 18 的 Debug/Release、clang-format 18、clang-tidy 18、
  sanitizers、依赖门禁）全绿。
- [x] 设计（第 5/13 节）、DEC-010、威胁模型（基线 2/6/9/12 证据、新增
  22/23、M5 细化项收口）、总计划（第 1/5/6/11 节）与本计划同步更新。

## 验证记录

### 2026-09-09：M5-01～M5-05 实现与本地验证（PR 前）

- 范围：工作树 `feat/m5-ipc-cli`。交付 `ipc_protocol`（帧/请求/响应编解码
  与边界校验，`yori_core`）、`ipc_transport`/`ipc_service`（Core 抽象与
  daemon 侧授权/脱敏服务）、`yori_ipc` 适配库（`UdsIpcClient` + 共享帧
  I/O，无 Executor）、`UdsIpcServer`（EXEC-02 单 blocking worker、
  `SO_PEERCRED`、DEC-010 端点治理）、`Daemon`（恢复 -> GPU 观察 -> IPC 的
  M5 子集总装 + EXEC-10 ①③ 停止序）、`yorid` 主生命周期（sigwait、SQLite +
  NVML 后端、admin 组成员解析）与 `yori` CLI 六命令；五个测试目标
  （`m5.unit.ipc-protocol`、`m5.unit.ipc-service`、`m5.unit.ipc-server`、
  `m5.fuzz.ipc-parser`、`m5.integration.ipc-e2e`），替换 M0 的
  `example.ipc.protocol`/`example.fuzz.parser` 占位；consumer 覆盖
  `yori::yori_ipc` 导出。
- 测试：协议（六 kind roundtrip、畸形矩阵逐项稳定错误码、边界值、编码
  拒绝且不留半帧）；服务（owner/admin/第三方授权矩阵、脱敏字段、容量拒绝
  + CANCELLED 回滚审计、终态幂等、JobId 单调不复用、logs 尾部/截断/未启动、
  GPU lease 合并视图、`file_log_snapshot_reader` 的 O_NOFOLLOW/非常规文件/
  缺失文件语义）；服务器（生命周期幂等、非 socket 陈旧文件拒绝、模式与
  属主断言、非 root 配置他人属主显式失败、超载帧回 PROTOCOL 错误帧、静默
  /半帧连接按截止时间断开、handler 异常 -> INTERNAL 且服务存活、
  `SO_PEERCRED` 的 uid/pid 进入 handler）；fuzz（2000 次种子化变异，两方向
  解码仅稳定错误或完整解析，完整解析可再编码 roundtrip）；E2E（进程内
  Daemon + 真实 CLI 二进制：submit/queue/ps/gpu/logs/cancel 全链路、幂等
  取消、用法与传输失败退出码 0/1/2/3、daemon 停止后端点清理）。
- 实现期发现并处置的问题：① Linux 的 `fchmod` 对 socket fd 返回成功但不
  改变 socket inode 权限——单测首轮拦截，改为"收紧 umask 创建 + bind 后
  显式 chmod/chown"；② 传输层与服务层帧封装叠加造成双重长度前缀（服务端
  响应不可解码）——拆分 `encode_*_payload`（纯 payload）与
  `append_*_frame`（含前缀）两级 API，传输层统一传 payload；③ handler
  异常路径误用 `kProtocol` 错误码——独立错误响应构造并补负向测试。
- 验证：Linux x86_64、内核 `7.0.0-31-generic`、GCC 13.3.0、Executor pin
  `e2dc8ca22433`。`debug`/`release`/`asan`/`ubsan`/`tsan` 五预设全部
  configure/build/ctest 通过（TSAN 以 `setarch -R ctest --preset tsan`
  执行），每套 34 个用例为 30 passed、4 个环境占位 skip（GPU、multi-user
  两个、performance；IPC 与 fuzz 占位已被真实用例替换），无失败、无 race
  报告。clang-format 18.1.3（pip，与 CI 一致）全量零违规；clang-tidy
  18.1.8（pip）全仓库 0 error（`WarningsAsErrors` 类别），剩余警告均为
  非升级类别（CLI/POSIX 解析固有复杂度、glibc 内部 typedef 的 include-
  cleaner 误报）。`cmake --install build/debug` 后 `tests/consumer` 仅使用
  安装产物（含 `yori::yori_ipc` 导出）配置/编译/运行通过。
- 限制：本机无 Clang 编译器与 apt 版 clang-tidy-18（以 pip 18.1.8 复现
  门禁）；PR CI 尚未触发，`M5-01`～`M5-05` 保持未勾选。root 环境的
  socket `root:yori` 收敛、真实多用户连接准入与 admin 组 NSS 解析无法在
  CI 覆盖（单用户非 root），M7 root 环境补跑（DEC-010 验证方式节）。
  负责人：Linductor-alkaid；补跑条件：PR CI 全绿后勾选工作项与退出条件，
  root 项待 M7。
- 同步：设计（v0.9：第 5 节 admin 判定引用、第 13.3-13.5 节协议 v1/
  传输承载/授权脱敏契约）、[DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md)、
  威胁模型（基线 2/3/6/7/8/9/12 的 M5 证据、新增基线 22/23、M5 细化项
  收口）、总计划（第 1/5/6/11 节，v1.6）、本计划。未修改 `third_party/`，
  未发现 Executor 能力缺口（IPC 服务器完全复用已验证的 blocking worker
  `start_worker` + 唤醒管道模式，与 M2 LogPump/ExitMonitor 同构）。

### 2026-09-09：M5-01～M5-05 PR CI 收尾

- 范围：PR [#7](https://github.com/Linductor-alkaid/yori/pull/7)，提交
  `3f592f8`～`31dd234`（协议契约、UDS 传输与服务、daemon 总装与 CLI、
  文档、两轮门禁修复）。
- 首轮 CI run [34367784531](https://github.com/Linductor-alkaid/yori/actions/runs/34367784531)
  的 clang-format 失败：本地格式化此前只覆盖已跟踪文件，M5 新文件在加入
  索引前从未进入 dry-run 检查集。教训回写：格式化/静态检查必须以提交后的
  全量文件清单为准（`git ls-files` 在 add 之前不含新文件）。修复提交
  `95c0287`。
- 次轮 run [34368052896](https://github.com/Linductor-alkaid/yori/actions/runs/34368052896)
  的 clang-tidy 失败（12 处 error 级）：CI 的 apt clang-tidy 18.1.3 相对
  本机 pip 18.1.8 对 `performance-move-const-arg`、
  `performance-unnecessary-value-param`、`bugprone-switch-missing-default-case`、
  `bugprone-unchecked-optional-access`、
  `bugprone-implicit-widening-of-multiplication-result`、
  `bugprone-branch-clone` 更严格。逐项修复（`31dd234`）后本地 pip 18.1.8
  复验 0 error、debug/release 复测全绿。
- 最终 CI run
  [34374222682](https://github.com/Linductor-alkaid/yori/actions/runs/34374222682)
  8/8 全绿：clang-format 18、clang-tidy 18、GCC 13/Clang 18 的
  Debug/Release configure/build/test/install/consumer、ASAN/UBSAN/TSAN 与
  依赖 pin 门禁全部通过。CI 环境每套 ctest 为 30 passed、4 个环境/后续
  里程碑占位用例显式 skipped（GPU、multi-user 两个、performance；IPC 与
  fuzz 占位已由 M5 真实用例承接）。
- 结论：`M5-01`～`M5-05` 的实现、文档与适用门禁证据完整，工作项与退出
  条件勾选完成，M5 里程碑标记 `Completed`。root 环境补跑项（socket
  `root:yori` 收敛、多用户连接准入、admin 组 NSS 解析）保持未勾选语义，
  由 M7 打包验收执行（DEC-010）。
