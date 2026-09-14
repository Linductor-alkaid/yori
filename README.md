# Yori

单节点、多用户的 GPU 训练任务排队、调度与进程守护系统。多人共享一台多 GPU
Linux 服务器时，不再需要nvidia-smi看哪张卡空闲——提交即排队，Yori 替你
盯着卡、抢到空闲 GPU 就以**你本人身份**启动训练，并守护到结束。

```text
yori submit 提交 Job -> yorid 全局队列排队 -> Scheduler 匹配空闲 GPU 并建立 lease
    -> 以提交用户身份启动训练进程并守护 -> 用户运行时观察（状态/日志/TensorBoard）
    -> 进程退出、释放 lease 并触发下一轮调度
```

- **以你本人身份运行**：训练进程属主是你（不是 root），身份来自内核
  `SO_PEERCRED`，不依赖用户自报。
- **断电/重启不怕**：daemon 正常停止或重启**不终止**运行中的训练；重启后
  核验进程身份（PID/PGID/启动时间）重新接管，绝不盲目重启或误接管。
- **零侵入**：Yori 不理解 PyTorch/Isaac Lab 等训练框架语义，训练程序无需
  链接或感知 Yori；GPU 对训练透明（自动注入 `CUDA_VISIBLE_DEVICES`）。
- **一台服务器一个 `yorid`**（唯一权威调度器）；`yori` CLI 是无状态客户端。

## 快速上手

管理员装好后（见[安装](#安装)），把你加入 `yori` 组，然后在激活了
conda/venv 的终端里：

```bash
yori submit -- python train.py --epochs 10   # 提交，立即返回 "Submitted job 1"
yori queue                                   # 排到第几位、为什么还在等
yori ps                                      # 全部 Job 的状态
yori logs -f 1                               # 实时看日志
yori tensorboard 1                           # 拉起 TensorBoard（127.0.0.1）
yori cancel 1                                # 不想跑了
```

**在已激活 conda/venv 的终端里直接提交即可**：提交瞬间的解释器路径与关键
环境变量（`PATH`、`CONDA_PREFIX`、`CUDA_HOME` 等）被快照捕获，排队期间你
改环境、切分支都不影响已提交的任务。已激活环境里能直接跑的命令，排队执行
语义一致。

## 挑卡跑：GPU 亲和

GPU 不总是同质可互换的（长期验证稳定的一张卡、复现排障、Isaac 类多设备
路径）。Yori 提供从硬约束到软偏好的三档选择：

| 方式 | 语义 | 示例 |
| --- | --- | --- |
| （缺省） | ANY：任意空闲卡 | `yori submit -- python train.py` |
| `--gpu` | REQUIRED 硬亲和：只跑指定卡，绝不换卡 | `yori submit --gpu 2 -- python train.py` |
| `--gpu-any-of` | REQUIRED 集合：只跑集合内的卡（1..8 个，逗号分隔） | `yori submit --gpu-any-of 0,1 -- python train.py` |
| `--gpu-preferred` | PREFERRED 软偏好：目标空闲就优先用，否则回落任意卡 | `yori submit --gpu-preferred 3 -- python train.py` |

- 指定目标可用 **物理 index**（`yori gpu` 的 INDEX 列）或稳定 **UUID**
  （`GPU-f3c1aa88-...`），daemon 在提交时解析；设备枚举顺序变化不会迁移错卡。
- REQUIRED 任务的目标被占用/外部使用/不可用时**保持排队，绝不 fallback**；
  等待原因在 `yori queue`/`yori ps` 的 WAIT 列可见（`NO_FREE_GPU`、
  `AFFINITY_GPU_ALLOCATED`、`AFFINITY_GPU_EXTERNAL`、`AFFINITY_GPU_STATE`）。
- 硬亲和任务**不阻塞别人**：调度按 FIFO 顺序做有界跳过，排在你后面的可满足
  任务会先启动，你的任务保持队列位置，目标空闲后自动开跑。
- 缺省 ANY 任务会**避开**同队列窗口内等待中 REQUIRED 任务指定的卡（软保护，
  不产生预留），减少"他要的卡被我占了、其他卡全闲着"的碎片。
- `--gpu` 与 `--gpus N` 计数语义互斥（MVP 单卡，`--gpus` 仅接受 1）。

## 命令速查

| 命令 | 作用 |
| --- | --- |
| `yori submit [选项] -- CMD...` | 提交任务；命令以 `--` 分隔 |
| `yori ps` | 全部 Job 状态（他人脱敏） |
| `yori queue` | 排队视图，含 WAIT 等待原因 |
| `yori gpu` | GPU 占用与 lease 视图 |
| `yori logs <id>` | 日志快照 |
| `yori logs -f <id>` | 实时跟随（`--since-offset/--since-stdout/--since-stderr` 断线续传） |
| `yori tensorboard <id>` | 以本人身份拉起 TensorBoard（默认 127.0.0.1，端口由 OS 分配并打印 URL；`--logdir/--port/--host` 可调） |
| `yori cancel <id>` | 取消（排队期或运行期，SIGTERM → 宽限 10s → SIGKILL） |
| `yori inspect <id>` | owner/admin：执行上下文与 provenance（敏感 env 值自动掩码） |

常用 submit 选项：`--cwd DIR`（缺省取当前目录）、`--env K=V`、
`--inherit-env`（显式全量继承）、`--capture-env KEY`（扩展捕获白名单，
如 `JAX_PLATFORMS`）、`--tensorboard-logdir DIR`、`--gpus N`（MVP 仅 1）。
CLI 全局 `--socket` 或 `YORI_SOCKET` 指定端点（默认 `/run/yori/yori.sock`）。

提交时的执行上下文捕获（DEC-011）：白名单变量（`PATH`/`PYTHONPATH`/
`LD_LIBRARY_PATH`、conda/venv 三键、`CUDA_HOME`/`CUDA_PATH`、
`OMP/MKL_NUM_THREADS`、代理变量含小写）被快照持久化；`argv[0]` 以捕获后的
`PATH` 解析为绝对路径——解析失败**拒绝提交**，把"解释器选错"从排队数小时
后提前到提交瞬间。GPU 管理键（`CUDA_VISIBLE_DEVICES` 等）与 `YORI_*` 变量
不捕获、不可经 `--env` 设置；调度器注入的 `YORI_JOB_ID`/`YORI_GPU_UUID`
永远胜出。

### 关于 Shell 语义与旧写法

- Yori 不执行 `conda activate`，也不解释管道/`&&`：需要 Shell 语义时写成
  `yori submit -- bash -c '<command>'`。
- 兼容旧写法：绝对路径解释器（`~/miniconda3/envs/rl/bin/python`）与
  `bash -lc 'conda activate ... && ...'` 依然可用；对接 < 0.2.0 的旧 daemon
  时需要这些配方或显式 `--env`。

## 安装

### 方式一：deb 包（推荐，Ubuntu/Debian）

从 [Releases](https://github.com/Linductor-alkaid/yori/releases) 下载
`yori_<version>_<arch>.deb`，在服务器上安装：

```bash
sudo apt install ./yori_0.5.0_amd64.deb
```

安装即完成：`yori`/`yorid` 进入 PATH、创建 `yori` 系统组、写入
`/run/yori` 端点配置并**自动启用并启动** `yori.service`（systemd）。
运行时依赖为 `libsqlite3-0` 与 `libc6 (>= 2.35)`/`libstdc++6 (>= 12)`；
NVML 库随机器上已有的 NVIDIA 驱动提供，deb 不会安装或改动任何驱动包。

发布包在 Ubuntu 22.04 工具链上构建，支持 Ubuntu 22.04 及更新版本。

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

## 常见问题

| 现象 | 原因与处理 |
| --- | --- |
| `yori` 报 permission denied | 当前会话尚未生效 `yori` 组：重新登录，或 `newgrp yori` 临时切换 |
| Job 一直 QUEUED，WAIT 列 `NO_FREE_GPU` | 所有卡都被占用（含外部进程），排队等待即可 |
| WAIT 列 `AFFINITY_GPU_EXTERNAL` | 指定的卡被他人直接占用（`nvidia-smi` 可见）；等它空闲，或取消后换卡 |
| Job 转为 `LOST` | daemon 重启后无法确认进程身份（如进程已死且被回收核验失败）；查看日志确认训练实际结局 |
| 想跑在某台机器的特定 conda 环境 | 在该环境激活的终端提交即可，无需写 activate 脚本 |
| 需要 Shell 管道/多命令 | `yori submit -- bash -c '...'` |

## 运维要点（管理员）

- **服务管理**：`systemctl status|stop|start yori`。停止 daemon 不终止训练；
  重启后自动恢复（采纳 RUNNING、重排 QUEUED）。
- **状态与日志目录**：SQLite 状态库 `/var/lib/yori/state.db`；每 Job 日志
  目录 `/var/lib/yori/jobs/<job-id>/`（stdout.log/stderr.log，单文件上限
  默认 256 MiB，可轮转）；服务日志经 journald。
- **管理员授权**：`yorid --admin-gid <GID>`（组成员可查看/取消任何 Job）；
  其余守护参数见 `yorid --help`（`--log-root`、`--state-db`、
  `--gpu-library`、`--sqlite-library`、`--scheduler-scan-window` 调度跳过
  扫描窗口，默认 32）。
- **外部占用**：被外部进程直接占用的 GPU 标记 `EXTERNAL_BUSY`，不会被分配，
  也不会被 Yori 干预或误杀。

## 功能一览

| 能力 | 说明 |
| --- | --- |
| 队列与调度 | 服务器级全局 FIFO（公平排队）；GPU lease 记账；事件驱动调度（提交/退出/取消/GPU 状态变化/恢复完成）；GPU 亲和三档（REQUIRED `--gpu`/`--gpu-any-of`、PREFERRED `--gpu-preferred`、亲和感知 ANY 选择） |
| 进程守护 | 独立进程组；取消升级 `SIGTERM -> 宽限（默认 10s）-> SIGKILL`；退出回收并释放 GPU 后自动调度下一个 Job |
| GPU 管理 | NVML 发现/遥测/外部占用检测（`EXTERNAL_BUSY`，不接管不误杀）；物理 GPU 对训练透明 |
| 持久化与恢复 | SQLite 状态库；daemon 重启恢复队列与 RUNNING Job（身份核验，无法确认转 `LOST`）；placement 随任务恢复不漂移 |
| 观察面 | 日志捕获/落盘/轮转；流式跟随断线续传；慢跟随客户端显式 `BACKPRESSURE` 断开，daemon 与训练不受影响 |

## 架构与文档

Core（调度/状态机/权限）与平台适配（NVML/SQLite/UDS/systemd）严格分层；
全部并发与生命周期由 [Executor](third_party/executor)（pinned submodule）
承载。

- [项目设计文档](docs/design/yori-project-design.md)——架构、协议与判据的唯一权威
- [实施总计划](docs/plans/yori-implementation-plan.md)与里程碑文档
- [工程规范](docs/project/project-standards.md)与[决策记录](docs/decisions/)
- [变更历史](CHANGELOG.md)——各版本行为细节与决策引用
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
