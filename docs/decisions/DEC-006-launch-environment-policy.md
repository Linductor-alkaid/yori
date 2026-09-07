# DEC-006：训练进程环境变量继承白名单

> 状态：Accepted
> 日期：2026-09-08
> 负责人：Linductor-alkaid
> 冻结里程碑：M2
> 替代/被替代：无

## 背景与问题

`yorid` 以特权身份运行（[DEC-004](DEC-004-privileged-daemon-demotion.md)），其进程环境包含
systemd 注入的变量、daemon 自身配置与潜在敏感信息。训练进程以提交用户身份运行，但其
环境由 daemon 构造：若直接继承 daemon 全量环境，会把特权上下文泄漏给不可信的用户命令；
若完全清空，训练程序将缺少基础变量而难以运行。总计划第 6 节要求 M2 冻结环境变量继承
白名单初版（设计第 17 节第 5 条）。

## 决策

MVP 冻结三层合并规则，由 `LaunchAdapter` 在构建 `LaunchPlan` 时执行，产物为按键排序的
唯一环境条目集合：

1. **身份块（始终设置，值来自提交用户的 passwd 记录）**：`HOME`、`USER`、`LOGNAME`、
   `SHELL`。身份块不允许被其他层覆盖。
2. **daemon 白名单继承（默认固定，可配置收紧）**：`PATH`、`LANG`、`TERM`、`TZ`，以及
   前缀匹配的 `LC_*`。白名单之外的一切 daemon 变量（含 `SUDO_*`、凭据、IPC 端点等）
   一律不进入训练环境。
3. **GPU 映射块（由调度结果生成）**：`cuda_visible_devices` 模式设置
   `CUDA_VISIBLE_DEVICES=<物理 GPU 索引>` 与 `CUDA_DEVICE_ORDER=PCI_BUS_ID`
   （NVML 索引按 PCI 总线序枚举，与 CUDA 默认"最快优先"序不一致，不设置该变量会导致
   映射错卡）。

用户通过 `JobSpec.env` 提供的变量最后应用，但以下键视为保留键，出现即拒绝整个
`LaunchPlan`（显式错误，不静默丢弃）：

- 身份块四个键；
- `CUDA_VISIBLE_DEVICES` 与 `CUDA_DEVICE_ORDER`（用户覆盖会破坏 GPU 隔离，见
  [威胁模型](../security/threat-model.md)）；
- `LD_PRELOAD`、`LD_LIBRARY_PATH`（多用户共享服务器上的提权/注入面）。

补充实施约束：supplementary groups 在 fork 前由父进程经 `IdentityResolver` 解析
（`getpwuid_r` + `getgrouplist`），子进程仅执行 `setgroups`/`setgid`/`setuid` 三个
syscall 封装，保证 fork-exec 窗口内不调用非 async-signal-safe 函数（NSS/malloc）。

## 备选方案

- 全量继承 daemon 环境：泄漏特权上下文，安全不可接受。
- 空环境 + 仅用户变量：训练程序缺少 `PATH`/locale 基础变量，可用性差，用户会用
  `env ...` 包装命令绕过，白名单失去意义。
- 允许用户覆盖 GPU/身份变量：破坏 GPU 隔离与文件所有权语义，等于放弃调度权威。

## 影响与风险

- 白名单过窄会迫使部分训练脚本自带环境包装；先以基础变量落地，扩展白名单属于
  低风险配置变更（无需新决策，更新本文与配置默认值即可）。
- 保留键拒绝可能让既有脚本迁移时报错；错误信息必须指明冲突键名。
- `LD_*` 拒绝在单机多用户场景下是安全收敛；若未来出现合法需求（如用户自装库），需以
  新决策记录放开并配套审计。

## 验证方式

M2 单元测试覆盖：合并顺序与唯一性、白名单外变量不进入、用户保留键被拒绝、
`CUDA_VISIBLE_DEVICES`/`CUDA_DEVICE_ORDER` 值正确；集成测试断言子进程实际环境与
`LaunchPlan` 一致。

## 关联文档和工作项

设计第 8、17 节；[威胁模型](../security/threat-model.md)；总计划第 6 节暂定默认值表；
M2 工作项 `M2-01`。
