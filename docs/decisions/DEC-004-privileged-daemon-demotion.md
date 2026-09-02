# DEC-004：MVP 以 root 运行 yorid 并在 exec 前降权

> 状态：Accepted
> 日期：2026-09-02
> 负责人：Linductor-alkaid
> 冻结里程碑：M0（决策方向已定；实现细节冻结见"影响与风险"）
> 替代/被替代：无

## 背景与问题

Yori 必须能以任意授权提交用户的身份启动训练进程。Linux 上为其他 UID 创建子进程
需要特权。安全是核心功能而非后续补丁（[设计文档](../design/yori-project-design.md)
第 17 节），特权面必须最小化并有明确的演进路径。

## 决策

- `yorid` 以 systemd 系统服务启动；MVP 允许 daemon 以 root 运行，root 权限严格
  限制在 daemon 自身与进程身份切换阶段。
- 连接身份只信内核机制（`SO_PEERCRED`）；客户端声明的目标 UID 不可信，Job
  owner 不得由客户端指定。
- 训练进程在 `exec` 前完成 supplementary groups（`initgroups`）、`setgid`、
  `setuid` 切换为提交用户；绝不以 root 执行用户提交的命令。
- `/run/yori/yori.sock` 收敛为 `root:yori 0660`；建议建立 `yori` 系统组，仅组内
  用户可连接。
- Job 查询、日志读取、取消与观察接口在 daemon 侧执行 owner/admin 授权。
- 长期演进：拆分 `yori-launch-helper`（最小特权 launcher），显著缩小 root 攻击
  面（总计划 `POST-09`，非 MVP 范围）。

## 备选方案

- 无特权 daemon：无法以其他用户身份 spawn 子进程；可行替代（setuid helper、
  PAM 等）实质上就是"拆分 launcher"方案的提前实现，MVP 复杂度过高。
- 训练以 daemon 同一非特权身份运行：破坏多用户文件所有权隔离（设计目标 4）。
- 依赖 setuid 位的多用户工具链：审计面更大、更难测试。

## 影响与风险

- root daemon 是最大攻击面：IPC parser 与 launch path 必须保持最小（设计第 17
  节第 9 条）；日志路径、cwd、runtime 与持久化目录需防符号链接攻击；由
  [威胁模型](../security/threat-model.md)跟踪并在 M2/M5 细化。
- 实现细节未冻结，按总计划第 6 节暂定默认值跟踪：取消 grace period、环境变量
  白名单初版（最迟 M2 冻结）。

## 验证方式

M2：降权后进程 UID/GID/supplementary groups 正确；训练产出文件 owner 正确；
非 `yori` 组用户连接被拒；`exec` 前后身份断言测试（`multi-user` 标签）；设计
第 19 节对应判据。

## 关联文档和工作项

设计第 5、17、19 节；[威胁模型](../security/threat-model.md)；
[总计划](../plans/yori-implementation-plan.md) `RULE-09`、M2、`POST-09`。
