# Changelog

本文件记录 Yori 的版本化变更（工程规范第 10.5 节）。日期为 YYYY-MM-DD；
条目按版本倒序排列。未发布条目置于 `## Unreleased`。

## v0.1.2 - 2026-09-10

### 修复

- **发布包改为在 Ubuntu 22.04 工具链上构建**（v0.1.1 已知问题）：此前在
  ubuntu-24.04 runner 上构建的产物引用 `GLIBC_2.38` 与 `GLIBCXX_3.4.31`
  符号版本，Ubuntu 22.04（glibc 2.35 / GLIBCXX 3.4.30）上运行报
  "version not found"。现 release 与 CI 的 deb 任务固定在 ubuntu-22.04 +
  gcc-12 构建并真机冒烟，产物向后兼容 22.04 及更新发行版；打包脚本加入
  符号版本红线自检（GLIBC ≤ 2.35、GLIBCXX ≤ 3.4.30，超限即打包失败），
  deb 显式声明 `Depends: libc6 (>= 2.35), libstdc++6 (>= 12)`，不满足时
  apt 给出明确错误而非运行时加载失败。

## v0.1.1 - 2026-09-10

### 修复

- **deb 包不再推荐安装 `nvidia-driver`**（v0.1.0 已知问题）：apt 默认安装
  Recommends，会拉入最新系列驱动组件并与已安装的版本化驱动栈
  （如 `nvidia-driver-570`）冲突，导致 apt 计划**卸载既有驱动**。Yori 的
  NVML 为运行期 `dlopen`，GPU 服务器必然已有驱动，deb 不应携带任何 NVIDIA
  依赖——现仅 `Depends: libsqlite3-0`，并在打包脚本中加入防回归自检
  （control 的 Depends/Recommends/Suggests 不得引用 nvidia 包）。
  v0.1.0 release 上的问题安装包已移除，请使用 v0.1.1。

## v0.1.0 - 2026-09-10

首个 MVP 版本（M0-M7 全部完成）：单节点多用户 GPU 训练任务排队、调度与
进程守护系统。

### 功能

- **队列与调度**：服务器级全局 FIFO 队列（DEC-005，容量默认 1024/上限
  4096）；事件驱动 FIFO 调度器，GPU lease 记账与状态机（QUEUED/STARTING/
  RUNNING/STOPPING/FINISHED/FAILED/CANCELLED/LOST，终态幂等）。
- **进程守护**：以提交用户身份 spawn（fork 前 NSS 解析、exec 前
  setgroups→setgid→setuid 降权，DEC-004）；独立进程组；取消升级
  SIGTERM→grace（默认 10s）→SIGKILL（DEC-007）；退出回收与 lease 释放；
  日志捕获落盘与轮转（单文件 256 MiB 默认）。
- **GPU 集成**：NVML 适配（dlopen，发现/UUID 身份/遥测/外部占用检测
  EXTERNAL_BUSY）；周期采样（默认 5s）与状态迁移事件触发调度。
- **持久化与恢复**：SQLite StateStore（DEC-009，schema 1，单事务原子
  apply）；daemon 重启恢复（身份三元组核验，RULE-06：RUNNING 采纳不重启、
  无法核验转 LOST）；STOPPING 恢复后重发取消。
- **IPC 与 CLI**：UDS 协议 v1（帧边界校验 + fuzz；SO_PEERCRED 鉴权，
  DEC-010）；`yori submit/ps/queue/gpu/cancel/logs`；`yori logs -f`
  流式跟随（offset 续传、GAP/EOF/BACKPRESSURE 帧）；`yori tensorboard`
  （CLI 侧拉起，默认仅监听 127.0.0.1）。
- **观察面**：每 Job `Topic` 订阅分发与内存回看窗口（默认 8 MiB/流）；
  慢客户端显式 BACKPRESSURE 断开；daemon 重启日志原位续写（DEC-008）。
- **打包**：systemd unit（RULE-10 守护语义）与 deb 包（安装即启用服务，
  `yori`/`yorid` 进 PATH）；GitHub Actions 在 tag 上构建 deb 并发布
  release。

### 安全

- 绝不以 root 执行用户命令；身份只信 SO_PEERCRED；环境变量白名单继承
  （DEC-006）；IPC 端点 root:yori 0660 收敛；日志与持久化路径的符号链接
  防护（O_NOFOLLOW、目录链属主校验）；ps 脱敏与 owner/admin 授权。

### 兼容性与依赖

- Linux（NVML/systemd/UDS/SO_PEERCRED）；C++20、CMake ≥ 3.25。
- 直接依赖：Executor（git submodule pin，MIT）、SQLite 与 NVML（运行期
  dlopen，不引入构建依赖）。锁定与校验见 `dependencies.lock.json`。

### 已知限制

- daemon 重启窗口内的训练输出丢失（管道断裂，DEC-008）；采纳进程自然
  退出的状态不可得（终态 FAILED + 显式原因；pidfd 增强为 POST-08）。
- 多 GPU Job、优先级/配额、自动重试、多节点为延后项（总计划第 9 节）。
