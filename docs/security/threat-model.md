# Yori 安全威胁模型（草案）

> 状态：Draft（骨架版；完整 STRIDE 分析随 M5/M6 工作项完成，完成后升级为
> Active）
> 日期：2026-09-10
> 负责人：Linductor-alkaid
> 依据：[设计文档](../design/yori-project-design.md)第 5、11.5、17 节、
> [AGENTS.md](../../AGENTS.md) 安全条款、[DEC-004](../decisions/DEC-004-privileged-daemon-demotion.md)

## 1. 范围

覆盖 MVP 攻击面：

- Unix Domain Socket 上的任意客户端输入（IPC 协议、帧边界、身份声明）。
- 以 root 运行的 `yorid`（IPC parser、launch path、身份切换、文件路径处理）。
- 以提交用户身份运行的训练进程（输出字节流、退出状态、文件产出）。
- 日志与状态文件（`/var/lib/yori`、`/run/yori`）。
- NVML 只读访问。

不在范围：训练程序内部安全、TensorBoard 自身漏洞（用户会话内进程）、多节点
面（总计划 `POST-07` 后扩展）。

## 2. 资产

- 用户训练代码、数据、checkpoint 与环境凭据（daemon 侧环境变量可能含敏感值，
  白名单外的变量不得进入用户 Job 环境）。
- 日志与指标数据（可能含路径、超参数、实验结果等敏感信息）。
- GPU 资源公平性（防占队、防资源滥用）。
- SQLite 状态完整性与审计记录。
- `yorid` 进程完整性（root 特权）。
- 系统资源（日志磁盘预算、IPC 带宽）。

## 3. 信任边界

```text
yori CLI（用户态，输入不可信）
    ==== UDS /run/yori/yori.sock（root:yori 0660）====
yorid（root；可信计算基：IPC parser、JobManager、Scheduler、launch path）
    |-- fork + initgroups/setgid/setuid 降权 --> 训练进程（提交用户；输出不可信）
    |-- NVML 只读访问 --> GPU 驱动
    |-- 读写 --> /var/lib/yori（状态、日志；路径攻击面）
    |-- 读写 --> /run/yori（socket、runtime；权限收敛）
yori CLI（用户会话） --> tensorboard 子进程（用户身份，默认 127.0.0.1）
```

## 4. 强制安全要求基线（设计第 17 节）

| # | 要求 | 主要对抗的威胁 | 落点里程碑 | 验证方式 |
| --- | --- | --- | --- | --- |
| 1 | 绝不以 root 执行用户命令 | 特权提升 | M2 | 降权后身份断言测试 |
| 2 | 身份来自 `SO_PEERCRED` 等内核机制 | 伪造身份 | M5（已落地） | 结构性保证：请求协议不存在身份字段（`IpcSubmitRequest`）；accept 时 `getsockopt(SO_PEERCRED)`，失败即断开；Job owner 恒等于 peer uid（`m5.unit.ipc-service`/`m5.integration.ipc-e2e`） |
| 3 | Job owner 不得由客户端指定 | 越权操作 | M5（已落地） | 同基线 2 的结构性保证 + 授权矩阵负向测试（非 owner 非 admin 的 cancel/logs 被拒，`m5.unit.ipc-service`） |
| 4 | `exec` 前完成 supplementary groups、`setgid`、`setuid` | 文件所有权逃逸 | M2 | `multi-user` 标签测试 |
| 5 | 环境变量白名单继承 | 凭据泄漏到用户 Job | M2 | 白名单外变量不出现 |
| 6 | `/run/yori/yori.sock` 权限收敛 | 未授权连接 | M5/M7（M5 已落地非 root 部分） | bind 以收紧 umask 创建 + 显式 chmod/chown（Linux fchmod 对 socket fd 不生效）；非 socket 陈旧文件拒绝替换；模式/属主断言（`m5.unit.ipc-server`）；root+yori 组的完整收敛在 M7 root 环境补跑（DEC-010） |
| 7 | 查询/日志/取消/观察执行 owner/admin 授权 | 跨用户越权 | M5/M6（M5 已落地查询/日志快照/取消） | owner/admin/第三方授权矩阵与脱敏字段断言（`m5.unit.ipc-service`）；admin 组成员启动时解析、主 GID+补充组双路径（DEC-010）；M6 补流式观察 |
| 8 | 日志路径、cwd、runtime 与持久化目录防符号链接攻击 | 路径逃逸/文件覆盖 | M2/M4/M5/M7（守护总装已落地日志根目录校验） | 日志 `O_NOFOLLOW` 断言（M2）；数据库文件为符号链接时拒绝打开、打开后收敛 `0600`（M4 已落地：`m4.unit.sqlite-state-store`）；logs 快照尾部读取 `O_NOFOLLOW`、非常规文件拒绝（M5 已落地：`m5.unit.ipc-service`）；M7 守护总装落地日志根目录与 Job 子目录的属主/写位链校验（`JobManager::start` 的 `prepare_log_root`：每级真实目录、属主 root 或 daemon euid、无组/其他写位——带 sticky 的 `/tmp` 例外；Job 子目录 mkdir `0750` 后 `lstat` 复核拒绝符号链接）；cwd 的 `chdir` 在子进程降权（`setuid`）之后执行（M2 顺序），符号链接逃逸只影响提交用户自身，不构成 daemon 侧威胁 |
| 9 | 特权 daemon 的 IPC parser 与 launch path 保持最小 | root 进程 RCE | M5（已落地） | parser 为纯字节解码、无分配前未验证计数、无解释执行；确定性 fuzz 集（种子化翻转/截断/超载变异）覆盖请求与响应两个方向（`m5.fuzz.ipc-parser`，sanitizer 下运行） |
| 10 | 外部 GPU 进程只影响资源状态，不主动终止或接管 | 误杀用户进程 | M3 | `EXTERNAL_BUSY` 测试（M3 已落地：`m3.unit.nvml-gpu-provider` 外部占用仅改变观测状态、无信号/接管路径；适配器只读 NVML；2026-09-10 真实 RTX 4080 SUPER 补跑以临时 CUDA 进程验证 `EXTERNAL_BUSY -> FREE`，进程仅由测试 owner 自行清理） |
| 11 | 长期拆分 privileged launcher（`yori-launch-helper`） | 缩小 TCB | `POST-09` | 非本 MVP |
| 12 | Job 创建拒绝 root owner，并在 IPC 前以固定上限校验 argv/env/cwd/profile/logdir | root workload、内存耗尽、路径逃逸 | M1/M5（M5 已落地） | `JobSpec` 上限与 root/路径负向测试（M1）；IPC 入口：协议层帧/字符串/计数上限 + root owner 提交 `DENIED` + 非法 spec 映射 `INVALID_SPEC`（`m5.unit.ipc-protocol`/`m5.unit.ipc-service`） |
| 13 | GPU snapshot 与 StateStore mutation 有固定条目上限；Job/lease 以 revision 原子提交 | 内存耗尽、状态篡改、部分写导致错误资源归属 | M1/M4（M4 已落地） | Provider 边界、revision 冲突、容量负向测试（M1）；SQLite 单事务回滚、目录只读/外部写锁/篡改行/篡改 lease 矩阵负向测试（M4 已落地：`m4.unit.sqlite-state-store`） |
| 14 | 全局队列只保存稳定排序键，默认 1024、硬上限 4096；所有拒绝返回结构化结果与事件 | 批量提交耗尽内存、静默丢弃或用户私有队列绕过全局顺序 | M1/M5 | 无效配置、容量、重复 Job、多用户稳定排序和恢复回滚负向测试 |
| 15 | 调度只选未 lease 的 `FREE` GPU，并以单个 StateStore mutation 提交 `STARTING + lease`；失败恢复队首 | 重复分配 GPU、绕过 FIFO、写失败后丢失 Job | M1/M4 | 队首阻塞、确定性 GPU 选择、lease 冲突、写失败回滚和队列/存储分歧测试 |
| 16 | launch path 的 argv/envp/组列表全部在 fork 前构造；子进程仅执行 `setpgid`/信号处置/`dup2`/`close_range`/`setgroups`/`setgid`/`setuid`/`chdir`/`execve` 等 syscall 封装 | fork-exec 窗口内的非 async-signal-safe 调用（NSS/malloc 锁）导致挂起或死锁 | M2 | 设计评审 + 守护进程引擎测试（M2 已落地，见 `M2-02`） |
| 17 | 子进程继承描述符经 `close_range` 收敛；exec 报告管道以 `FD_CLOEXEC` 在成功 exec 时自动关闭 | daemon 内部 fd（socket、DB、日志）泄漏进训练进程 | M2 | 引擎审查；`M2-02` 集成路径无 fd 泄漏断言 |
| 18 | 环境保留键（身份块、`CUDA_VISIBLE_DEVICES`、`CUDA_DEVICE_ORDER`、`LD_PRELOAD`、`LD_LIBRARY_PATH`）在 JobSpec.env 中出现即拒绝启动计划 | 用户覆盖 GPU 隔离或以动态链接注入攻击 root daemon 路径 | M2 | `M2-01` 保留键负向测试 |
| 19 | exec 前 `SIGPIPE` 置为忽略（DEC-008）；日志文件 `O_APPEND|O_NOFOLLOW` 打开，`0640` 与属主显式设置 | daemon 退出误杀训练（可用性）；符号链接替换日志文件（路径逃逸） | M2 | 管道断裂存活测试；`M2-05` 权限与打开方式断言 |
| 20 | NVML 适配以 count-only 查询检测外部计算进程，不读取进程身份字段；NVML 库路径只接受管理员配置或测试注入，不接受用户输入；驱动返回数据按 SPI 校验（UUID 合法性、遥测边界、设备数上限），非法即整次观测失败 | 跨用户进程信息泄露；恶意/异常驱动数据污染调度事实；任意库注入 root daemon | M3 | 基线 10 `EXTERNAL_BUSY` 测试（`m3.unit.nvml-gpu-provider`）；身份零采集代码审查（`src/gpu/nvml_gpu_provider.cpp`）；非法快照不发布（`m3.unit.gpu-manager`）；2026-09-10 真实 RTX 4080 SUPER/NVML 570.211.01 补跑覆盖设备发现、UUID、遥测和 v3 count-only 外部占用查询，证据见 M3 计划验证记录；v2-only 与错误注入由 stub 矩阵覆盖 |
| 21 | 恢复绝不无条件重启数据库中的 RUNNING/STARTING Job：核验 PID/PGID/启动 ticks 三元组，进程消失、身份不符（PID reuse）或身份缺失一律 `LOST` 并释放 lease；`LOST` 残留进程由 `EXTERNAL_BUSY` 兜底而非接管/终止；SQLite 行数据篡改（非法状态、revision 矛盾、编码越界、lease 矩阵破坏）使 `load()` 显式失败；SQLite 库路径只接受管理员配置或测试注入 | PID reuse 错误接管（特权路径作用到无关进程）；崩溃窗口孤儿进程导致 GPU 双重分配；状态文件篡改注入非法调度事实 | M4 | 恢复决策矩阵与重入幂等（`m4.unit.job-recovery`）；PID reuse 防护与重启闭环（`m4.integration.recovery-restart`）；篡改/故障注入（`m4.unit.sqlite-state-store`） |
| 22 | IPC 帧边界全部显式：负载 1 MiB、单字符串 64 KiB（禁 NUL）、请求计数 256、列表 1024、日志尾部每流 256 KiB；超限/截断/坏版本/坏 kind/值域非法均映射稳定错误码，帧界违规回 PROTOCOL 错误帧后断开；每连接请求总预算（默认 5s）到期断开；handler 异常映射 INTERNAL 响应；响应列表超限截断并置 LIMIT（不静默） | 内存耗尽与队头阻塞（超长帧、慢客户端）；解析器内存安全漏洞成为 root RCE 入口；异常吞噬掩盖故障 | M5 | 协议畸形矩阵（`m5.unit.ipc-protocol`）；超载帧/静默连接/半帧断开与 handler 异常路径（`m5.unit.ipc-server`）；确定性 fuzz 集（`m5.fuzz.ipc-parser`，`fuzz` 标签、sanitizer 常规运行） |
| 23 | IPC 授权与脱敏仅在 daemon 侧判定：owner = `SO_PEERCRED` uid 相等；admin = 主 GID 匹配配置组或 UID 属于启动时解析的组成员集（客户端声明不可信）；非 owner 非 admin 的 `ps`/`queue` 仅见 JobId/状态/revision/owner_uid/退出状态，argv/cwd/tensorboard_logdir 不出现在响应；admin 组成员解析失败降级为仅主 GID 匹配 | 跨用户信息泄露（argv/日志路径/env 探测）；伪造 admin 身份绕过授权 | M5 | 授权矩阵（owner/admin/第三方 x ps/queue/gpu/cancel/logs）与脱敏字段断言（`m5.unit.ipc-service`）；协议无身份字段的结构性保证（基线 2/3）；真实多用户环境补跑见 M5 计划 |
| 24 | `logs -f` 流式会话有界：会话数每 Job 8/全局 64、每订阅者队列 64 chunk、会话写出缓冲 2 MiB、单帧写截止 2 s（全部可配）；溢出以订阅者侧 offset 间断检出并回 BACKPRESSURE 帧后断开（含当前 offset，不静默丢弃）；写超时/对端断开的会话有界回收；流式帧解码与请求/响应共用畸形矩阵与 fuzz 集（含 golden vector）；无日志源的 Job 显式 `NOT_AVAILABLE`，不伪造流；fd 接管发生在 runtime 层流式委派，Core 契约不暴露平台类型 | 慢客户端拖垮 daemon（队头阻塞/内存耗尽）；流式 parser 漏洞成为 root 入口；会话资源耗尽 | M6 | 六场景会话单测（正常 EOF/对端断开/准入拒绝/执行中取消/写超时/shutdown）+ 慢客户端 BACKPRESSURE 负向 + `--since-*` 回放与 GAP（`m6.unit.log-follow`）；流式帧畸形矩阵与 golden vector（`m5.unit.ipc-protocol` 扩展）；fuzz 集三方向变异（`m5.fuzz.ipc-parser` 扩展）；E2E 全链路（`m6.integration.logs-follow-e2e`） |
| 25 | `TENSORBOARD` 查询仅对 owner/admin 返回 spec.tensorboard_logdir 与 cwd（非 owner 非 admin 直接 DENIED，无脱敏视图）；`yori tensorboard` 由 CLI 以当前用户身份拉起，默认仅监听 127.0.0.1、端口默认 OS 分配，绑定更大范围必须显式 `--host`（DEC-003）；TensorBoard 进程属于用户观察会话，不占 GPU lease、不进队列 | 训练曲线与日志目录路径泄露给同机其他用户；指标面板默认暴露给网络邻居 | M6 | 授权矩阵与敏感字段不泄漏断言（`m5.unit.ipc-service` 扩展）；CLI 参数/解析优先级/回环默认 E2E（PATH 注入假二进制，`m6.integration.logs-follow-e2e`）；真实 TensorBoard 二进制不在 CI，M7 真机补跑 |

## 5. 初步威胁清单（待细化）

按 STRIDE 分类，条目在对应里程碑细化为"威胁/资产/入口/对策/残留风险/测试"：

- **Spoofing**：伪造连接身份；伪造 JobId 归属；CLI 侧伪造 tensorboard 查询者。
- **Tampering**：SQLite 状态文件篡改；日志文件注入与符号链接替换；提交内容
  （argv/env/cwd）作为不可信输入注入 launch path；IPC 帧篡改。
- **Repudiation**：取消、强杀进程、管理员干预缺少可追溯审计记录。
- **Information Disclosure**：跨用户日志读取；`ps`/`queue` 非自有 Job 信息脱敏
  不足（设计第 11.5 节）；TensorBoard 默认监听配置错误导致指标暴露。
- **Denial of Service**：慢 `logs -f` 客户端拖垮 daemon（背压策略）；超长 IPC
  帧；提交洪水占满队列；训练日志洪水耗尽磁盘（落盘预算与轮转）。
- **Elevation of Privilege**：IPC parser 漏洞导致 root 代码执行；降权顺序错误
  （setuid/setgid 顺序、supplementary groups 遗漏）；PID reuse 导致错误接管
  特权路径。

## 6. 待完成项

- [x] M2 实施前：launch path 与降权序列的威胁细化 —— 已随 M2 落地为基线条目
  16-19（fork 前构造、fd 收敛、保留键、SIGPIPE/日志打开方式）；cwd 的父目录链
  属主校验经 M7 复核收敛：`chdir` 位于子进程 `setuid` 之后，目录选择只影响
  提交用户自身，不构成特权侧攻击面（基线 8 已更新）。
- [x] M4 实施前：持久化与恢复的威胁细化 —— 已随 M4 落地为基线条目 21
  （恢复不重启、PID reuse 核验、篡改显式失败），并更新基线 8/13 的 M4 部分
  （数据库符号链接拒绝、事务回滚负向测试）。
- [x] M5 实施前：IPC 协议、鉴权与 parser 的威胁细化 —— 已随 M5 落地为
  基线条目 22（帧边界/慢客户端/异常映射 + 确定性 fuzz 范围声明：请求与
  响应两个方向、种子化变异集、sanitizer 常规运行；独立 libFuzzer 基础设施
  另行立项）与 23（daemon 侧授权/脱敏/admin 判定），并收口基线 2/3/6/7/9/12
  的 M5 证据。root 环境（socket 属主收敛、多用户连接准入）在 M7 补跑。
- [x] M6 实施前：观察面（流式跟随、背压、脱敏、tensorboard 网络边界）细化
  —— 已随 M6 落地为基线条目 24（流式会话有界性、BACKPRESSURE 显式断开、
  流式帧 fuzz 与 golden vector、无日志源不伪造流）与 25（TENSORBOARD 查询
  授权、CLI 侧拉起与回环默认监听），并收口基线 7/9 的 M6 部分（流式观察
  纳入 owner/admin 授权；fuzz 集扩展至流式帧方向）。
- [x] M7 实施中：守护总装（JobManager、恢复采纳、取消升级、日志目录创建）
  的威胁细化 —— 已收口基线 8 的父目录链校验（日志根目录 + Job 子目录属主/
  写位/符号链接拒绝）；采纳进程退出状态不可得（恢复后自然退出为 FAILED +
  显式原因，pidfd 增强见 `POST-08`）记入残留风险；daemon 关闭 abandon 不发
  信号（RULE-10）不扩大攻击面。
- [ ] M7 发布前：全模型复查、残留风险清单定稿，本文件升级为 Active（含
  root/systemd/双用户真机验收后的基线 6 root 收敛复核）。
- [ ] root 环境补跑：`m2.security.process-demotion`（DEC-004 降权身份断言，
  非 root 环境显式 skip；补跑条件：root 下设置 `YORI_DEMOTION_TEST_UID`/
  `YORI_DEMOTION_TEST_GID` 后运行 `ctest -L multi-user`）。
