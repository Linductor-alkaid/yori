# DEC-011：提交时执行上下文捕获与训练环境恢复

> 状态：Accepted（2026-09-12 负责人确认依总计划启动 M8，随 M8 启动冻结）
> 日期：2026-09-12（Proposed）/ 2026-09-12（Accepted）
> 负责人：Linductor-alkaid
> 冻结里程碑：M8
> 替代/被替代：部分修订 [DEC-006](DEC-006-launch-environment-policy.md)（用户变量层与保留键集）；需求来源 [#16](https://github.com/Linductor-alkaid/yori/issues/16)

## 背景与问题

MVP 交付后（v0.1.x）的实际使用反馈显示：训练任务几乎都运行在用户已激活的
Conda/venv 环境中，依赖提交时 Shell 的 `PATH`、`CONDA_PREFIX`/`VIRTUAL_ENV`、
`PYTHONPATH`、`LD_LIBRARY_PATH` 与当前工作目录。而 `yorid` 是 systemd 管理的长期
daemon，其环境与用户交互 Shell 完全不同。当前实现只自动捕获 `cwd`（CLI `getcwd`），
环境变量仅接受显式 `--env K=V`，导致"用户终端里能直接跑的命令，经
`yori submit --` 排队启动后失败"，用户被迫把 Shell 已有信息重新描述给 Yori。

核心问题不是"支持 Conda"，而是把**用户提交任务时已经准备好的执行上下文**
（Execution Context）作为 Job 定义的一部分保存，并在任务真正获得 GPU 启动时恢复。
Yori 不应重新执行用户激活环境的过程（`conda activate` 依赖 Shell 初始化与 Shell
function，在 systemd 非交互环境中不可靠，也无法泛化到 venv/uv/pixi），而应直接
捕获"最终状态"：哪个 executable、哪个 working directory、哪些必要环境变量。

## 决策

### 1. JobSpec 扩展为 command + execution context + resource request

`JobSpec` 新增三个字段（协议与持久化同步扩展）：

- `executable`：提交时由 CLI 解析出的 `argv[0]` 绝对路径；
- `env_metadata`：可选环境来源元数据（`environment_type`：
  `conda`/`venv`/`none`，由捕获到的 `CONDA_PREFIX`/`VIRTUAL_ENV` 判定；
  `python_version`：当 executable 为 Python 解释器时可选探测）；
- `cwd`/`env` 语义升级：`env` 从"显式覆盖"升级为"提交时捕获的用户执行上下文"
  （含显式 `--env` 追加项），`cwd` 维持自动捕获不变。

用户身份（uid/gid）**不进入** ExecutionContext——issue #16 提案中的
`uid/gid` 字段不采纳，owner 继续只来自 `SO_PEERCRED`（DEC-010、基线 2/3），
客户端声明的任何身份字段仍不可信。

### 2. CLI 提交时捕获策略（EnvironmentCapturePolicy）

默认自动捕获以下变量（存在才捕获，值来自 CLI 进程环境）：

``` text
PATH  PYTHONPATH  LD_LIBRARY_PATH
CONDA_PREFIX  CONDA_DEFAULT_ENV  VIRTUAL_ENV
CUDA_HOME  CUDA_PATH
OMP_NUM_THREADS  MKL_NUM_THREADS
HTTP_PROXY  HTTPS_PROXY  NO_PROXY（含小写形式）
```

- `yori submit --env K=V`：在捕获结果上显式追加/覆盖（保留键仍被拒绝）。
- `yori submit --inherit-env`：显式 opt-in 捕获全部环境变量（仍过滤保留键，
  仍受 JobSpec env 总量上限约束，超限提交失败并明确报错）。
- GPU 管理键（`CUDA_VISIBLE_DEVICES`、`CUDA_DEVICE_ORDER`）与 `YORI_*` 前缀
  变量**不捕获**：它们属于调度器输出，捕获用户提交时刻的值没有意义。
- 白名单经 daemon 配置可扩展（收敛到配置级变更，不需要新决策）。

### 3. executable 提交时解析（fail-fast）

CLI 在提交时以**捕获后的 PATH** 解析 `argv[0]`：含 `/` 的相对 cwd 解析、裸名
按 PATH 逐段解析；解析失败或目标不可执行则拒绝提交并报错。daemon 启动任务时
直接 `execve(executable, argv, envp)`，不再按用户 PATH 做运行时搜索；`argv[0]`
保持用户输入形式，使 `$0`/`ps` 展示与直接执行一致。这同时把"Python 解释器
选错"类失败从排队数小时后提前到提交瞬间。

### 4. 环境合并 v2：四层合并，优先级显式

`DefaultLaunchAdapter::prepare` 的合并顺序修订为：

``` text
1. 身份块（HOME/USER/LOGNAME/SHELL，passwd 记录）——不可被任何层覆盖
2. daemon 白名单（PATH、LANG、TERM、TZ、LC_*）——基础层
3. 捕获的用户执行上下文（JobSpec.env）——覆盖同名 daemon 键
4. Yori 资源块——最高优先级，最后写入：
   CUDA_VISIBLE_DEVICES（物理索引）、CUDA_DEVICE_ORDER=PCI_BUS_ID、
   YORI_JOB_ID、YORI_GPU_UUID（新引入，保留键）
```

即优先级为 `Yori overrides > captured environment > daemon defaults`，身份块
独立不可覆盖。资源块永远胜出，提交环境中遗留的 GPU 变量不可能干扰分配结果。

### 5. 保留键集修订（修订 DEC-006）

- 维持出现即拒绝：身份四键、`CUDA_VISIBLE_DEVICES`、`CUDA_DEVICE_ORDER`、
  `LD_PRELOAD`，以及新增的 `YORI_*` 前缀（显式设置视为对 Yori 管理面的
  伪造尝试）。
- **`LD_LIBRARY_PATH` 从保留键转入捕获白名单**。风险再评估：子进程在
  `setgroups -> setgid -> setuid` 完成降权**之后**才 exec（DEC-004、基线 4），
  训练进程携带的 `LD_LIBRARY_PATH` 与用户在 Shell 中直接执行完全等价，不再
  构成对特权路径的注入面；而 Conda/Isaac 类环境缺少它往往无法运行，保留拒绝
  会持续破坏"排队执行与直接执行语义一致"的产品目标。`LD_PRELOAD` 维持拒绝：
  训练场景无正当需求，且与 daemon 自身库环境的混淆风险不成比例；出现合法
  需求时按 DEC-006 既定流程新决策放开。

### 6. 命令语义维持 argv，不引入 `--shell`

继续以 argv/execve 语义保存与执行命令，避免 Shell quoting、escaping 与命令
注入面。需要 Shell 语义（管道、`&&`）的用户显式表达为
`yori submit -- bash -c '<command>'`，与默认 argv 语义保持显式区分；独立的
`--shell` 接口不进入 M8（记为 POST-13）。

### 7. `yori inspect` 与脱敏

新增 IPC kind `INSPECT`（owner/admin 授权，同 TENSORBOARD 模式，非 owner 非
admin 直接拒绝）：展示 cwd、executable、argv、环境类型、python 版本、placement、
分配结果与基本 provenance。env 值默认脱敏：变量名全部可见，值仅 owner/admin
可见，且命中敏感名模式（`*TOKEN*`/`*KEY*`/`*SECRET*`/`*PASSWORD*`，可配置）
的值一律掩码；daemon 日志不打印 env 值。

### 8. 持久化（schema v2）

`StateStore` schema 由 1 升至 2，**增量迁移**（新增 `executable` 与 env 元数据
字段，v1 库读入时按缺省补全），DEC-009 的 dlopen/单事务/篡改显式失败原则不变。
GPU placement 的持久化随 DEC-012 另行增量扩展（schema v3）。

## 备选方案

- `--conda-env`/`--venv` 显式声明：与具体环境管理工具耦合，要求用户重复表达
  Shell 已有信息，且每支持一种工具都要扩展 CLI——不采纳。
- daemon 启动时执行 `conda activate`：依赖 Shell 初始化与非交互环境模拟，
  不确定性高，无法泛化——不采纳。
- 维持现状仅靠 `--env` 与文档指导：提交成本高、易错，真机反馈已证明不可用
  ——不采纳。
- 默认全量继承用户环境：敏感值（凭据、session socket）默认进入数据库与
  IPC 面，违反最小化原则——仅以显式 `--inherit-env` 提供。

## 影响与风险

- 捕获的环境变量持久化进入 SQLite（`0600` root）并在 `inspect` 暴露：以
  白名单默认 + 显式 opt-in + 敏感值脱敏收敛（威胁模型新增基线 26-27）。
- 排队期间用户环境变化不跟随（快照语义）：按提交时刻快照执行是本决策的
  显式定义，`inspect` 展示捕获内容以消除歧义。
- `LD_LIBRARY_PATH` 放开的安全论证依赖"降权先于 exec"这一既有不变量，
  必须有回归测试锁住降权顺序（基线 4/25 复核）。
- 协议以 v2 扩展 SUBMIT（新增可选字段）与 INSPECT kind；daemon 同时接受
  v1 SUBMIT（缺省字段按无捕获处理），CLI 与 daemon 同版本发布。
- uv/pixi/poetry 等新环境管理器只需扩展捕获白名单与 `environment_type`
  判定，不触及调度与执行核心。

## 验证方式

M8 测试矩阵：已激活 Conda/venv 环境下提交运行的语义一致性（解释器、cwd、
关键变量与直接执行一致）；executable 解析失败提交即拒；四层合并次序与保留键
负向（含 `LD_PRELOAD` 仍拒、`YORI_*` 拒绝）；`--inherit-env` 超限显式失败；
`inspect` 授权与脱敏断言；schema v1→v2 迁移与 v1 库兼容读；降权顺序回归。

## 关联文档和工作项

[Issue #16](https://github.com/Linductor-alkaid/yori/issues/16)；
[DEC-006](DEC-006-launch-environment-policy.md)（部分修订）；
[DEC-004](DEC-004-privileged-daemon-demotion.md)；
[DEC-009](DEC-009-sqlite-state-store.md)；
[设计文档](../design/yori-project-design.md)第 6.1、8.2、13 节；
[威胁模型](../security/threat-model.md)基线 26-27；
M8 工作项（[总计划](../plans/yori-implementation-plan.md)第 5 节）。
