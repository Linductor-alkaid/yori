# Yori 安全威胁模型（草案）

> 状态：Draft（骨架版；完整 STRIDE 分析随 M2/M5/M6 工作项完成，完成后升级为
> Active）
> 日期：2026-09-04
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
| 2 | 身份来自 `SO_PEERCRED` 等内核机制 | 伪造身份 | M5 | 负向：客户端声明 uid 被忽略 |
| 3 | Job owner 不得由客户端指定 | 越权操作 | M5 | 负向：伪造 owner 被拒 |
| 4 | `exec` 前完成 supplementary groups、`setgid`、`setuid` | 文件所有权逃逸 | M2 | `multi-user` 标签测试 |
| 5 | 环境变量白名单继承 | 凭据泄漏到用户 Job | M2 | 白名单外变量不出现 |
| 6 | `/run/yori/yori.sock` 权限收敛 | 未授权连接 | M5/M7 | 权限断言 |
| 7 | 查询/日志/取消/观察执行 owner/admin 授权 | 跨用户越权 | M5/M6 | 越权负向测试 |
| 8 | 日志路径、cwd、runtime 与持久化目录防符号链接攻击 | 路径逃逸/文件覆盖 | M2/M4 | 负向：符号链接用例 |
| 9 | 特权 daemon 的 IPC parser 与 launch path 保持最小 | root 进程 RCE | M5 | fuzz + 设计评审 |
| 10 | 外部 GPU 进程只影响资源状态，不主动终止或接管 | 误杀用户进程 | M3 | `EXTERNAL_BUSY` 测试 |
| 11 | 长期拆分 privileged launcher（`yori-launch-helper`） | 缩小 TCB | `POST-09` | 非本 MVP |
| 12 | Job 创建拒绝 root owner，并在 IPC 前以固定上限校验 argv/env/cwd/profile/logdir | root workload、内存耗尽、路径逃逸 | M1/M5 | `JobSpec` 上限与 root/路径负向测试；M5 parser 边界测试 |
| 13 | GPU snapshot 与 StateStore mutation 有固定条目上限；Job/lease 以 revision 原子提交 | 内存耗尽、状态篡改、部分写导致错误资源归属 | M1/M4 | Provider 边界、revision 冲突、容量与事务回滚负向测试 |
| 14 | 全局队列只保存稳定排序键，默认 1024、硬上限 4096；所有拒绝返回结构化结果与事件 | 批量提交耗尽内存、静默丢弃或用户私有队列绕过全局顺序 | M1/M5 | 无效配置、容量、重复 Job、多用户稳定排序和恢复回滚负向测试 |

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

- [ ] M2 实施前：launch path 与降权序列的完整威胁细化（含 env 白名单、cwd 与
  日志路径处理）。
- [ ] M5 实施前：IPC 协议、鉴权与 parser 的威胁细化（含 fuzz 范围声明）。
- [ ] M6 实施前：观察面（流式跟随、背压、脱敏、tensorboard 网络边界）细化。
- [ ] M7 发布前：全模型复查、残留风险清单定稿，本文件升级为 Active。
