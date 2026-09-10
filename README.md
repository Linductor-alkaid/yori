# Yori

单节点、多用户的 GPU 训练任务排队、调度与进程守护系统。解决多人共享一台
多 GPU Linux 服务器时的 GPU 争抢与人工排队问题。

```text
yori submit 提交 Job -> yorid 全局队列排队 -> Scheduler 匹配空闲 GPU 并建立 lease
    -> 以提交用户身份启动训练进程并守护 -> 用户运行时观察（状态/日志/TensorBoard）
    -> 进程退出、释放 lease 并触发下一轮调度
```

- 一台服务器只运行一个 `yorid`（唯一权威调度器）；`yori` CLI 为无状态客户端。
- 训练以**提交用户的身份**运行（IPC 身份来自内核 `SO_PEERCRED`，daemon 为
  root、exec 前完成降权），用户文件属主正确。
- daemon 正常停止/重启**不终止**运行中的训练；重启后核验进程身份（PID/PGID/
  启动时间）重新采纳，绝不盲目重启。
- Yori 不理解训练框架语义，也不要求训练程序链接 Yori。

## 功能一览

| 能力 | 说明 |
| --- | --- |
| 队列与调度 | 服务器级全局 FIFO（公平排队，单卡 MVP）；GPU lease 记账；事件驱动调度（提交/退出/取消/GPU 状态变化/恢复完成） |
| 进程守护 | 独立进程组；取消升级 `SIGTERM -> 宽限（默认 10s）-> SIGKILL`；退出回收并释放 GPU 后自动调度下一个 Job |
| GPU 管理 | NVML 发现/遥测/外部占用检测（`EXTERNAL_BUSY`，不接管不误杀）；物理 GPU 对训练透明（`CUDA_VISIBLE_DEVICES` 或物理参数模板） |
| 持久化与恢复 | SQLite 状态库；daemon 重启恢复队列与 RUNNING Job（身份核验，无法确认转 `LOST`） |
| CLI | `submit` `ps` `queue` `gpu` `cancel` `logs`（快照与 `-f` 流式跟随，支持 offset 断线续传）`tensorboard` |
| 观察面 | 日志捕获/落盘/轮转；慢跟随客户端显式 `BACKPRESSURE` 断开，daemon 与训练不受影响 |

## 安装

### 方式一：deb 包（推荐，Ubuntu/Debian）

从 [Releases](https://github.com/Linductor-alkaid/yori/releases) 下载
`yori_<version>_<arch>.deb`，在服务器上安装：

```bash
sudo apt install ./yori_0.1.2_amd64.deb
```

安装即完成：`yori`/`yorid` 进入 PATH、创建 `yori` 系统组、写入
`/run/yori` 端点配置并**自动启用并启动** `yori.service`（systemd）。
运行时依赖为 `libsqlite3-0` 与 `libc6 (>= 2.35)`/`libstdc++6 (>= 12)`；
NVML 库随机器上已有的 NVIDIA 驱动提供，deb 不会安装或改动任何驱动包。

发布包在 Ubuntu 22.04 工具链上构建（glibc 2.35 / GLIBCXX 3.4.30 符号
上限），支持 Ubuntu 22.04 及更新版本。

把允许使用 Yori 的用户加入 `yori` 组（重新登录生效）：

```bash
sudo usermod -aG yori <user>
```

### 方式二：源码构建

要求 Linux、CMake ≥ 3.25、GCC 13 或 Clang 18、Ninja。

```bash
git clone --recurse-submodules https://github.com/Linductor-alkaid/yori.git
cd yori
cmake --preset release -DYORI_FETCH_DEPENDENCIES=OFF
cmake --build --preset release
ctest --preset release                       # 可选：运行测试
sudo cmake --install build/release --prefix /usr/local
```

源码安装不自动启用服务，部署前置（`yori` 组、`--socket-group`、状态目录）
见 [packaging/systemd/README.md](packaging/systemd/README.md)。

## 快速上手

```bash
yori submit -- python train.py --epochs 10        # 提交；返回 "Submitted job 1"
yori queue                                        # 查看排队
yori ps                                           # 查看全部 Job（他人脱敏）
yori gpu                                          # GPU 占用与 lease 视图
yori logs 1                                       # 日志快照
yori logs -f 1                                    # 实时跟随（断线可 --since-* 续传）
yori tensorboard 1                                # 以本人身份拉起 TensorBoard（127.0.0.1）
yori cancel 1                                     # 取消（排队期或运行期）
```

提交选项：`--gpus N`（MVP 仅 1）、`--cwd DIR`、`--env K=V`、
`--tensorboard-logdir DIR`；命令以 `--` 分隔。CLI 全局 `--socket` 或
`YORI_SOCKET` 指定端点（默认 `/run/yori/yori.sock`）。

## 运维要点

- **服务管理**：`systemctl status|stop|start yori`。停止 daemon 不终止训练；
  重启后自动恢复（采纳 RUNNING、重排 QUEUED）。
- **状态与日志目录**：SQLite 状态库 `/var/lib/yori/state.db`；每 Job 日志
  目录 `/var/lib/yori/jobs/<job-id>/`（stdout.log/stderr.log，单文件上限
  默认 256 MiB，可轮转）；服务日志经 journald。
- **管理员**：`yorid --admin-gid <GID>`（组成员可查看/取消任何 Job）；
  其余守护参数见 `yorid --help`（`--log-root`、`--state-db`、
  `--gpu-library`、`--sqlite-library`）。
- **多 GPU 服务器**：默认采样 `libnvidia-ml.so.1`；被外部进程占用的 GPU
  标记 `EXTERNAL_BUSY`，不会被分配。

## 架构与文档

Core（调度/状态机/权限）与平台适配（NVML/SQLite/UDS/systemd）严格分层；
全部并发与生命周期由 [Executor](third_party/executor)（pinned submodule）
承载。

- [项目设计文档](docs/design/yori-project-design.md)——架构、协议与判据的唯一权威
- [实施总计划](docs/plans/yori-implementation-plan.md)与里程碑文档（M0-M7）
- [工程规范](docs/project/project-standards.md)与[决策记录](docs/decisions/)
- [威胁模型](docs/security/threat-model.md)与[供应链策略](docs/supply-chain/dependency-policy.md)
- 部署细节：[packaging/systemd/README.md](packaging/systemd/README.md)

## 开发

```bash
cmake --preset debug -DYORI_FETCH_DEPENDENCIES=OFF && cmake --build --preset debug
ctest --preset debug          # 五预设：debug/release/asan/ubsan/tsan
```

CI 覆盖 clang-format/clang-tidy、GCC/Clang 双编译器矩阵、sanitizers 与
安装后 consumer 编译；贡献请遵循 [AGENTS.md](AGENTS.md) 与工程规范
（分支/MR/提交纪律）。

## 许可证

[MIT](LICENSE)。直接依赖：Executor（MIT，git submodule pin）；SQLite 与
NVML 为运行期 `dlopen` 的系统库。
