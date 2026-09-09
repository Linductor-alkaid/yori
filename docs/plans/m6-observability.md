# M6：观察面

> 状态：In Progress
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M5（[IPC 与 CLI](m5-ipc-cli.md)）
> 建议发布点：无
> 更新日期：2026-09-10

## 目标

交付 DEC-002 定义的训练观察面：`yori logs -f` 流式跟随（逻辑 offset 续传、
`GAP`/`EOF`/`BACKPRESSURE` 控制帧、慢客户端显式断开）、跟随会话与订阅分发的
Executor 承载（`EXEC-03`/`EXEC-04`）、观察上限配置定稿，以及
[DEC-003](../decisions/DEC-003-tensorboard-cli-hosting.md) 的
`yori tensorboard` 观察入口（CLI 侧拉起 + daemon 只读 logdir 解析）。

## 范围与非目标

范围：

- 协议 v1 扩展（`include/yori/ipc/ipc_protocol.hpp`，version 保持 1，以新
  kind 扩展，M5 计划"协议演进"风险项的落地形态）：
  - 新请求 kind `LOGS_FOLLOW`（=7）：`[u64 job_id][u8 have_since_stdout]
    [u64 since_stdout][u8 have_since_stderr][u64 since_stderr]`。两路流独立
    续传（设计 11.3 的 `--since-offset` 按流具体化）；不携带身份字段。
  - 新请求 kind `TENSORBOARD`（=8）：`[u64 job_id]`。owner/admin 授权后返回
    logdir 解析原料（`optional tensorboard_logdir` + `cwd`），优先级判定
    （`--logdir` 参数 > spec 字段 > cwd）在 CLI 侧执行（DEC-003）。
  - 响应体按 kind 扩展：`LOGS_FOLLOW` ack 携带两会话流的起始 offset 与当前
    Job 状态；`TENSORBOARD` 携带解析字段。
  - 流式帧族（仅 daemon -> CLI，初始响应帧之后）：payload =
    `[u8 version=1][u8 frame_kind][body]`；`LOG_DATA`（stream/begin_offset/
    data）、`LOG_GAP`（stream/from_offset/to_offset）、`LOG_BACKPRESSURE`
    （stream/offset，发送即断开会话）、`LOG_EOF`（job 终态 + 可选退出状态，
    发送后排空关闭）。数据帧上限复用 `kMaxLogTailBytes`（256 KiB ≥ LogPump
    64 KiB 读块）。新增 `decode_stream_frame_payload` 与编码镜像，纳入畸形
    矩阵与 fuzz 集；建立 golden vector（M5 风险项收口）。
- `LogStreamer`（`yori_runtime`，EXEC-04）：每 Job 一个
  `executor::comm::Topic<LogChunk>`（`RejectNewest`，每订阅者有界队列即
  设计 11.4 的发送缓冲）；每流一个内存回看窗口（bounded backlog，默认
  8 MiB/流，配置 64 KiB ~ 64 MiB）承载 `--since-offset` 重连回放；窗口
  淘汰或 daemon 重启造成的不可回放区间以 `GAP` 帧显式告知（从窗口起点
  续传，不伪造数据、不回退 offset）；Job 终态经 `finish_job` 发布 `EOF`
  chunk 并关闭 Topic（订阅者排空后收到 Closed）；会话准入 = 每 Job 8、
  全局 64（配置定稿，总计划第 6 节冻结项）。
- `LogPump` 观察者钩子（`ILogChunkObserver`）：管道数据块经 `LogSink.append`
  接受后，按接受前后的逻辑 offset 发布到 `LogStreamer`；写失败丢弃时发布
  `[yori] dropped N bytes` 标记 chunk（offset 不前进，与落盘标记同格式）；
  落盘主路径不受订阅分发影响（观察不得影响被观察者，设计 11.1）。
- 跟随会话承载（`yori_runtime`，EXEC-03）：`UdsIpcServer` 增加可选流式委派
  （runtime 层接口，不进 Core 公开契约）：`LOGS_FOLLOW` 请求先经
  `IpcService::validate_logs_follow`（owner/admin 授权、Job 存在性与状态），
  拒绝路径回普通错误响应帧；接受路径将连接 fd 移交 `LogFollowService` 的
  单个 blocking worker（poll 会话 fd + 唤醒管道），由它写入初始 ack 帧与
  后续流帧：回放 -> 订阅排空（offset 去重）-> `EOF` 关闭；订阅队列溢出
  （会话写入缓冲有界、socket 写有截止时间）以 offset 间断检测，回
  `BACKPRESSURE` 帧后断开；客户端断开（POLLRDHUP/写失败）静默回收会话，
  不影响落盘与其他会话。停止 = 断开全部会话（客户端见 EOF）。
- daemon 总装：`Daemon` 装配 `LogStreamer` + `LogFollowService`（启动序在
  GPU 观察 之后、IPC 之前；停止序 EXEC-10 = ① IPC -> ② 跟随会话 -> ③ GPU）。
  M6 daemon 仍不启动训练进程（守护总装收口在 M7 前），跟随数据面由
  LogPump 钩子供料，测试直接驱动。
- CLI（`yori`，无状态、不创建 Executor）：
  - `yori logs -f [--since-stdout N] [--since-stderr N] <job-id>`：流式打印
    （stdout 帧到 stdout、stderr 帧到 stderr）；`GAP` 提示到 stderr；
    `BACKPRESSURE` 打印一键重连命令并退出码 4；`EOF` 后退出码与 Job 终态
    对齐（FINISHED -> 0，其余终态 -> 1 并打印状态）。
  - `yori tensorboard [--logdir DIR] [--port N] [--host H] <job-id>`：无
    `--logdir` 时经 IPC 解析（owner/admin；DENIED 即退出 1）；以当前用户
    fork/exec `tensorboard --logdir <dir> --port <p> --host <h>`，前台等待，
    SIGINT/SIGTERM 转发子进程后回收；默认端口 0（OS 分配，TensorBoard 自行
    打印 URL）、默认 host 127.0.0.1（DEC-003 网络边界）；`--port` 冲突由
    TensorBoard 退出码透传。
- 测试：协议新 kind 与流式帧 roundtrip/畸形/golden；streamer 单测（回看
  窗口淘汰 -> GAP、准入上限、EOF/迟到订阅、标记 chunk）；泵钩子单测
  （offset 正确性、写失败标记）；会话单测（六场景：正常 EOF、对端异常
  断开、准入拒绝、执行中客户端取消、写超时、服务 shutdown；慢客户端
  BACKPRESSURE 负向）；tensorboard 服务单测（授权矩阵、脱敏）；E2E 集成
  （真实进程产日志 -> LogPump -> streamer -> UDS -> 真实 CLI `logs -f`
  全链路 + `--since-*` 续传；tensorboard 以 PATH 注入假二进制验证参数与
  生命周期）。
- 文档：设计第 11.3/11.4/11.6/13 节 M6 落地细化、第 13.5 节 CLI 退出码
  契约补 `logs -f`/`tensorboard`；[DEC-008](../decisions/DEC-008-daemon-restart-log-continuity.md)
  M6 复核记录（log-keeper 维持 MVP 外结论）；威胁模型（流式会话与
  tensorboard 查询的基线证据）；总计划第 1/5/6/11 节；本计划。

非目标：

- 守护总装收口（进程启动/退出回收接入 daemon、`EXEC-09` 启动 PhaseGate、
  RUNNING Job 取消）——M7 前独立收口；M6 daemon 的 `logs -f` 对无日志源
  Job 显式报错，不伪造流。
- daemon 托管 TensorBoard / 常驻面板（`POST-10`，DEC-003 明确排除）。
- 磁盘文件回放（跨重启的历史跟随）：M6 回看窗口为内存实现，重启后旧 offset
  以 GAP 跳到当前；磁盘全量历史继续由 `logs` 快照与 owner 直接 `tail` 文件
  覆盖（设计 11.2）。
- 多节点流式转发、日志内容解析（Yori 不理解训练语义）。
- libFuzzer 持续 fuzz 基础设施（M6 只扩展进程内确定性变异集）。

## 设计与决策依据

- [DEC-002](../decisions/DEC-002-mvp-observability.md)：观察面纳入 MVP 的
  范围与帧语义（offset/GAP/EOF/BACKPRESSURE、会话上限、脱敏配套）。
- [DEC-003](../decisions/DEC-003-tensorboard-cli-hosting.md)：TensorBoard 由
  CLI 用户会话拉起；daemon 只读解析 logdir；默认 127.0.0.1 + OS 分配端口。
- [DEC-008](../decisions/DEC-008-daemon-restart-log-continuity.md)：重启窗口
  输出不可恢复、offset 不回退；M6 复核续捕语义并维持结论（GAP 显式告知，
  log-keeper 仍为 MVP 外对照方案）。
- 设计第 11 节（观察面原则：不理解训练语义、观察不影响被观察者、一切有界、
  owner/admin 隔离）、第 13 节（流式帧与协议边界）。
- Executor 承载：`EXEC-03`（`logs -f` 会话 = blocking worker，wakeup 关闭
  会话）、`EXEC-04`（`Topic<LogChunk>` 每 Job 一个，队列满断开订阅回
  BACKPRESSURE，不静默丢弃）。慢客户端检测采用订阅者侧 offset 间断判定：
  `Topic` 发布端只报 rejected 计数（监控可见），精确的溢出会话由会话
  worker 依 `begin_offset` 间断识别并执行设计 11.4 的断开协议。
- 会话 worker 与 M5 IPC 服务器同为单 blocking worker + 唤醒管道模式
  （M2 LogPump/M5 UdsIpcServer 已验证同构）；流式连接的 fd 接管发生在
  runtime 层，`IpcRequestHandler`/协议 Core 契约不暴露平台类型（RULE-02）。
- 上限定稿（总计划第 6 节冻结动作）：每 Job 跟随会话 8、全局 64、回看窗口
  8 MiB/流、订阅队列 64 chunk、会话写出缓冲 2 MiB、单帧写截止 2 s。均为
  配置项并有负向测试（RULE-08/DOD-05）。

## 工作项

- [ ] `M6-01` 协议 v1 扩展：`LOGS_FOLLOW`/`TENSORBOARD` 请求与响应体、
  流式帧族编解码、边界校验与 golden vector；协议单测与 fuzz 集扩展。
- [ ] `M6-02` `LogStreamer`：每 Job `Topic<LogChunk>`、内存回看窗口与 GAP、
  `finish_job` 的 EOF 语义、会话准入计数；单测覆盖窗口淘汰与迟到订阅。
- [ ] `M6-03` 数据面与验证面：`LogPump` 观察者钩子（接受块 offset 发布、
  丢弃标记 chunk）；`IpcService::validate_logs_follow` 与 `TENSORBOARD`
  查询（授权矩阵、脱敏）；配套单测。
- [ ] `M6-04` 会话承载与总装：`UdsIpcServer` 流式委派、`LogFollowService`
  blocking worker（回放/排空/BACKPRESSURE/EOF/停止）、`Daemon` 装配与
  EXEC-10 ①②③ 停止序；六场景 + 慢客户端负向单测。
- [ ] `M6-05` CLI 与 E2E：`yori logs -f`（续传/GAP/BACKPRESSURE/EOF 退出码）、
  `yori tensorboard`（解析优先级、信号转发、默认回环监听）；进程内 daemon
  + 真实 CLI + 真实子进程的全链路集成测试；文档同步与五预设 + PR CI。

## 风险与阻塞

- 单会话 worker 的写入公平性：多会话共享一个 blocking worker，慢客户端由
  写截止时间 + 有界缓冲约束，单会话每轮工作量有上限；若 M6 实测公平性
  不足，再评估多 worker 化并同步 EXEC-03。
- `RejectNewest` 丢弃的检出依赖后续 chunk 的 offset 间断：安静流（发布后
  长期无新数据）的溢出会话在 EOF/下一 chunk 到达前不会被断开——接受为
  有界延迟（缓冲上限仍在，客户端重连语义不变），EOF 事件保证最终检出。
- 本机无 apt 版 clang-tidy-18 与 Clang 编译器（既有环境限制）：以 pip
  clang-tidy 18.1.8 本地复现门禁，PR CI 为最终门禁；TSAN 需 `setarch -R`。
- CI 单用户非 root：socket `root:yori` 收敛与真实多用户准入维持 M7 补跑
  （DEC-010 既定安排）；tensorboard 真实二进制不在 CI，以 PATH 注入假
  二进制验证 CLI 契约。

## 测试与退出条件

- [ ] 协议单测通过（`m5.unit.ipc-protocol` 扩展）：新 kind roundtrip、
  流式帧 roundtrip 与畸形矩阵（坏 kind/坏流标识/超限数据/截断/尾部字节）、
  golden vector 字节级断言。
- [ ] fuzz 集通过（`m5.fuzz.ipc-parser` 扩展）：流式帧解码纳入变异集，
  无 crash、错误稳定、完整解析可再编码 roundtrip。
- [ ] streamer 单测通过（`m6.unit.log-streamer`）：发布/订阅/回看窗口
  淘汰 -> GAP、准入上限（每 Job/全局）、`finish_job` -> EOF 与迟到订阅
  回放、标记 chunk 不前进 offset。
- [ ] 泵钩子单测通过（`m2.unit.log-pump` 扩展）：接受块 offset 与数据
  正确发布；注入写失败后标记 chunk 发布。
- [ ] 会话单测通过（`m6.unit.log-follow`）：正常完成 EOF；对端异常断开
  回收；准入拒绝回 LIMIT；执行中取消（客户端关闭）；写超时会话有界回收；
  服务 shutdown 全会话断开；慢客户端（不读 + 小缓冲）最终收到
  BACKPRESSURE 帧；`--since-*` 回放与 GAP 跳变。
- [ ] 服务单测通过（`m5.unit.ipc-service` 扩展）：`validate_logs_follow`
  授权矩阵与状态判定；`TENSORBOARD` owner/admin 查询、第三方 DENIED、
  非自有字段不泄漏。
- [ ] 集成 E2E 通过（`m6.integration.logs-follow-e2e`）：真实子进程产日志
  -> LogPump -> streamer -> UDS -> 真实 CLI `logs -f` 收到数据帧与 EOF、
  退出码对齐；`--since-*` 续传去重；tensorboard 假二进制参数与 URL 契约。
- [ ] `debug`/`release`/`asan`/`ubsan`/`tsan` 预设全部通过；PR CI 全绿。
- [ ] 设计（11.3/11.4/11.6/13 节）、DEC-008 复核记录、威胁模型、总计划
  （第 1/5/6/11 节）与本计划同步更新。

## 验证记录

### 2026-09-10：M6-01～M6-05 实现与本地验证（PR 前）

- 范围：工作树 `feat/m6-observability`。交付协议 v1 扩展（`LOGS_FOLLOW`/
  `TENSORBOARD` 请求与 ack、流式帧族 `LOG_DATA`/`LOG_GAP`/
  `LOG_BACKPRESSURE`/`LOG_EOF` 编解码、golden vector）、`LogStreamer`
  （每 Job `Topic<LogChunk>`、内存回看窗口与 GAP、`finish_job` EOF 与
  Topic 关闭、两级会话准入、变更监听唤醒）、`LogPump` 观察者钩子（接受块
  按逻辑 offset 发布、丢弃标记 chunk 与落盘标记同格式）、`IpcService` 的
  `validate_logs_follow` 与 `TENSORBOARD` 查询（授权矩阵、敏感字段直接
  拒绝）、`UdsIpcServer` 流式委派（fd 接管在 runtime 层，Core 契约不变）、
  `LogFollowService` 会话 worker（EXEC-03：单 blocking worker + 100ms 有界
  tick + 唤醒管道、回放/GAP/排空去重/BACKPRESSURE/EOF/写截止时间/停止
  断开全部会话）、`Daemon` 装配（启动序 GPU -> 观察面 -> IPC，停止序
  EXEC-10 ①②③）、`UdsIpcClient::follow` 流式客户端与 `yori` CLI 的
  `logs -f`（续传/GAP/BACKPRESSURE 退出码 4/EOF 终态对齐）与
  `tensorboard`（解析优先级、信号转发、默认回环监听、端口 0 语义）；
  测试目标 `m6.unit.log-streamer`、`m6.unit.log-follow`、
  `m6.integration.logs-follow-e2e` 新增，`m5.unit.ipc-protocol`/
  `m5.unit.ipc-service`/`m2.unit.log-pump`/`m5.fuzz.ipc-parser` 扩展。
- 测试：协议（新 kind roundtrip、流式帧 roundtrip 与畸形矩阵：坏版本/坏
  kind/坏流标识/坏终态/截断/尾部字节/超限数据编码与解码拒绝、三组 golden
  vector 字节级断言）；服务（`validate_logs_follow` 的 QUEUED/不存在/未授权/
  已启动/admin/无委派 kUnsupported/store 失败矩阵；TENSORBOARD 的
  owner/admin/第三方 DENIED/不存在与字段原样返回）；streamer（注册幂等、
  发布/订阅/回放窗口淘汰 -> GAP、准入上限与 RAII 归还、EOF 与迟到订阅、
  标记 chunk 不前进 offset、offset 衔接违规显式拒绝）；泵钩子（真实子进程
  输出按 offset 连续发布、注入写失败后 on_drop 且落盘错误不吞）；会话
  （六场景：正常 EOF、对端异常断开、准入拒绝三路、执行中取消、写超时
  有界回收、shutdown 全会话断开；慢客户端 BACKPRESSURE：缩小收发缓冲
  使订阅队列确定性溢出、读发交替检出间断；`--since-*` 回放与 GAP 跳变）；
  E2E（进程内 Daemon + 真实子进程 -> LogPump -> streamer -> UDS -> 真实
  CLI `logs -f` 收到数据帧与 EOF、`--since-stdout 0` 断线重连回放、落盘
  同时完成、tensorboard 假二进制的 `--logdir` 解析优先级/端口/回环默认/
  执行失败退出码、无日志源 Job `logs -f` 显式失败）。
- 实现期发现并处置的问题：① Topic 无 fd 可 poll——发布/终态经 streamer
  变更监听显式唤醒会话 worker（poll 保留 100ms 有界 tick 推进写截止与
  慢客户端检出，事件到达仍即时唤醒）；② 会话 worker 的 tick 路径最初
  只推进写出不排空订阅（安静流卡死）——tick 与事件路径统一为同一推进
  序；③ 慢客户端溢出检出依赖"被拒绝块之后的新块"，安静流的检出延迟
  有界（已记录为风险），会话单测以读发交替规避调度时序依赖；④ E2E 的
  store 播种必须在 daemon 启动前完成（InMemoryStateStore 单 owner，主线程
  与 IPC worker 并发访问有数据竞争），改由恢复语义将种子 Job 收敛为 LOST
  且 log_path 保留；⑤ GCC 13 -O3 对 deque 区间 assign 的
  -Wnull-dereference 误报——回放快照改逐块拷贝。
- 验证：Linux x86_64、内核 `7.0.0-31-generic`、GCC 13.3.0、Executor pin
  `e2dc8ca22433`。`debug`/`release`/`asan`/`ubsan`/`tsan` 五预设全部
  configure/build/ctest 通过（TSAN 以 `setarch -R ctest --preset tsan`
  执行），每套 37 个用例（30 passed + 4 环境占位 skip + M6 新增 3 个全
  passed），无失败、无 race 报告；`m6.unit.log-follow` 在 debug/TSAN 下
  各连续 20 次通过（背压路径时序稳定）。clang-format 18.1.8（pip，含
  索引后全量文件清单）零违规；clang-tidy 18.1.8（pip）全仓库 0 error
  （`WarningsAsErrors` 类别）。`cmake --install build/debug --prefix
  build/debug/install` 后 `tests/consumer` 仅使用安装产物配置/编译/运行
  通过。
- 限制：本机无 Clang 编译器与 apt 版 clang-tidy-18（以 pip 18.1.8 复现
  门禁，CI apt 18.1.3 历史上更严格，以 PR CI 为最终门禁）；PR CI 尚未
  触发，`M6-01`～`M6-05` 与退出条件保持未勾选。root 环境（socket
  `root:yori` 收敛、真实多用户准入）与真实 TensorBoard 二进制无法在 CI
  覆盖，M7 补跑。负责人：Linductor-alkaid；补跑条件：PR CI 全绿后勾选
  工作项与退出条件，root/真机项待 M7。
- 同步：设计（第 11.3/11.4/11.6/13.2/13.3/13.5 节 M6 细化）、
  [DEC-008](../decisions/DEC-008-daemon-restart-log-continuity.md) M6 复核
  记录（维持决策，log-keeper 仍为 MVP 外）、威胁模型（新增基线 24/25、
  基线 7/9 收口、M6 待完成项闭合）、总计划（第 1/5/6/11 节，第 6 节
  日志与跟随上限冻结）、本计划。未修改 `third_party/`，未发现 Executor
  能力缺口（`Topic` 的 RejectNewest + per-subscriber 有界队列与
  blocking worker 模式完全覆盖会话承载；溢出检出由订阅者侧 offset 间断
  承担，属应用语义）。
