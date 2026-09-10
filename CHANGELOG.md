# Changelog

本文件记录 Yori 的版本化变更（工程规范第 10.5 节）。日期为 YYYY-MM-DD；
条目按版本倒序排列。未发布条目置于 `## Unreleased`。

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
