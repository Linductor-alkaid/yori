# M8：提交时执行上下文捕获与训练环境恢复

> 状态：In Progress
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：M7（打包与 MVP 端到端验收，PR [#11](https://github.com/Linductor-alkaid/yori/pull/11)）
> 决策依据：[DEC-011](../decisions/DEC-011-execution-context-capture.md)（Accepted，2026-09-12 随 M8 启动冻结）
> 需求来源：[Issue #16](https://github.com/Linductor-alkaid/yori/issues/16)
> 建议发布点：`v0.2.0`（tag 与发布动作需负责人明确授权后执行）
> 更新日期：2026-09-12

## 目标

1. **执行上下文作为 Job 定义的一部分**：把用户提交任务时已准备好的执行上下文
   （哪个 executable、哪个 working directory、哪些必要环境变量）在提交时捕获、
   持久化，并在任务获得 GPU 启动时恢复，使"终端里能直接跑的命令，经
   `yori submit` 排队启动后语义一致"（Conda/venv/uv 等激活环境快照语义）。
2. **fail-fast**：executable 在提交时以捕获后的 PATH 解析，解析失败或不可执行
   即拒绝提交，把"解释器选错"类失败从排队数小时后提前到提交瞬间。
3. **可观察与可审计**：`yori inspect` 展示执行上下文与基本 provenance，env 值
   仅 owner/admin 可见且敏感名模式一律掩码（威胁模型基线 26）。

## 范围与非目标

范围（DEC-011 决策 1-8）：

- `JobSpec` 扩展：`executable`（提交时解析的 `argv[0]` 绝对路径，可选——
  v1 提交缺省为无捕获）与 `env_metadata`（`environment_type`：
  `conda`/`venv`/`none`；`python_version` 可选探测）；`env` 语义升级为
  "提交时捕获的用户执行上下文（含显式 `--env` 追加项）"；`cwd` 自动捕获
  维持不变。用户身份不进入执行上下文（owner 只来自 `SO_PEERCRED`）。
- 保留键集修订（修订 DEC-006）：`LD_LIBRARY_PATH` 从保留键转入捕获白名单
  （安全性依赖"降权先于 exec"不变量，基线 27 回归锁定）；`LD_PRELOAD` 与
  `YORI_*` 前缀维持出现即拒绝；新增 `YORI_GPU_UUID` 资源键。
- 环境合并 v2（四层）：身份块（不可覆盖）-> daemon 白名单 -> 捕获的用户
  执行上下文 -> Yori 资源块最后写入（`CUDA_VISIBLE_DEVICES`、
  `CUDA_DEVICE_ORDER`、`YORI_JOB_ID`、`YORI_GPU_UUID`）。
- CLI 捕获策略 `EnvironmentCapturePolicy`：默认白名单捕获（PATH、
  PYTHONPATH、LD_LIBRARY_PATH、CONDA_PREFIX、CONDA_DEFAULT_ENV、
  VIRTUAL_ENV、CUDA_HOME、CUDA_PATH、OMP/MKL_NUM_THREADS、代理三键含
  小写形式）；`--env K=V` 显式追加/覆盖；`--inherit-env` 显式 opt-in 全量
  捕获（仍过滤保留键，受 JobSpec env 总量上限约束，超限显式失败）；
  GPU 管理键与 `YORI_*` 不捕获。
- executable 提交时解析：含 `/` 的相对 cwd 解析、裸名按捕获后 PATH 逐段
  解析、目标必须可执行；daemon 启动时直接 `execve(executable, argv, envp)`，
  `argv[0]` 保持用户输入形式。
- IPC 协议 v2：SUBMIT 增量扩展可选字段（executable、env 元数据）；新 kind
  `INSPECT`（=9，owner/admin）；daemon 同时接受 v1 SUBMIT（缺省字段按无捕获
  处理）；帧格式与边界纪律不变。
- 持久化 schema v2：新增 `executable`/`environment_type`/`python_version`
  列，v1 库单事务增量迁移与兼容读；DEC-009 的 dlopen/单事务/篡改显式失败
  原则不变。
- 测试与文档：DEC-011 验证矩阵、设计文档（§6.1/§8.2/§13）、威胁模型
  基线 26-27、总计划同步。

非目标：

- GPU placement（`--gpu N` 硬亲和、候选集过滤、schema v3）：属 M9
  （DEC-012）。
- 独立 `--shell` 提交接口（POST-13）；Shell 语义继续以 `-- bash -c` 表达。
- 完整环境 provenance（git commit/PyTorch/CUDA 版本探测与复现报告）：
  POST-14。
- daemon 运行期动态配置重载；执行 `conda activate` 类环境重建（DEC-011
  备选方案，不采纳）。
- uv/pixi/poetry 专属捕获判定：仅按 `VIRTUAL_ENV`/`CONDA_PREFIX` 通用判定，
  新管理器经白名单与判定扩展承接。

## 设计与决策依据

- [DEC-011](../decisions/DEC-011-execution-context-capture.md)（决策 1-8 与
  验证方式）；部分修订
  [DEC-006](../decisions/DEC-006-launch-environment-policy.md)（用户变量层与
  保留键集）。
- [DEC-004](../decisions/DEC-004-privileged-daemon-demotion.md)：`LD_LIBRARY_PATH`
  放开的安全论证依赖降权先于 exec；[DEC-009](../decisions/DEC-009-sqlite-state-store.md)
  schema 演进纪律；[DEC-010](../decisions/DEC-010-uds-ipc-endpoint.md) 身份
  只信 `SO_PEERCRED`。
- 设计文档 §6.1（JobSpec）、§8.2（LaunchProfile/环境合并）、§13（IPC 协议
  v2 与 `yori inspect`）；威胁模型基线 26-27。
- ISSUE 真机反馈：[#16](https://github.com/Linductor-alkaid/yori/issues/16)。

## 工作项

- [ ] `M8-01` Core 契约与环境合并 v2：`JobSpec` 新增 `executable`/`env_metadata`
  与校验（绝对路径/字节上限/枚举值域）；保留键集修订（`LD_LIBRARY_PATH`
  出保留集、`YORI_*` 前缀拒绝）；`DefaultLaunchAdapter::prepare` 四层合并
  （接收 JobId，资源块最后写入 `YORI_JOB_ID`/`YORI_GPU_UUID`）；
  `LaunchPlan.executable` 与 `ProcessSupervisor` 直接 `execve` 路径
  （`argv[0]` 保持用户输入）；单测覆盖合并次序与保留键负向。
- [ ] `M8-02` 捕获与解析组件（Core，CLI 复用）：`EnvironmentCapturePolicy`
  （默认白名单 + 可扩展键 + `--inherit-env` 全量模式；保留键/GPU 管理键/
  `YORI_*` 不捕获；总量上限预检）；`resolve_executable`（相对 cwd/PATH 逐段/
  可执行校验/失败结构化报错）；`detect_env_metadata`（CONDA_PREFIX/
  VIRTUAL_ENV 判定 environment_type、python 版本 best-effort 探测）；单测
  覆盖白名单边界、超限、代理小写、解析失败。
- [ ] `M8-03` IPC 协议 v2：版本协商（接受 v1/v2，响应回显请求版本）；
  SUBMIT v2 尾部追加可选 executable/env 元数据（v1 帧按无捕获解码）；
  新 kind `INSPECT`（请求 job_id；响应 cwd/executable/argv/环境类型/
  python 版本/env 名全可见 + 值掩码标记/placement 与分配结果/provenance）；
  协议单测（v1/v2 矩阵、畸形、golden vector）与 fuzz 语料扩展。
- [ ] `M8-04` daemon 侧：`IpcService::handle_submit` 映射新字段（v1 缺省）；
  `handle_inspect`（owner/admin 授权、敏感名模式默认
  `TOKEN`/`KEY`/`SECRET`/`PASSWORD` 可配置、掩码在 daemon 侧完成、daemon
  日志不打印 env 值）；`JobManager` 调度落地链路把 JobId 传入 prepare 并
  使用 `LaunchPlan.executable`；单测覆盖授权矩阵与脱敏断言。
- [ ] `M8-05` 持久化 schema v2：`yori_jobs` 新增 `executable`（TEXT NULL）、
  `environment_type`（INTEGER NULL）、`python_version`（TEXT NULL）；打开时
  schema_version 1 -> 2 单事务增量迁移（ALTER TABLE ADD COLUMN）；v1 库
  迁移后按缺省补全读取；round-trip 与迁移/篡改负向单测。
- [ ] `M8-06` CLI：`submit` 接入捕获策略（默认白名单、`--env`、`--inherit-env`、
  `--capture-env KEY` 白名单运行时扩展点）、executable 解析 fail-fast 与
  本地保留键预检；新增 `yori inspect <job-id>` 展示；用法文本与退出码契约
  维持（本地拒绝 = 用法错误 2）。
- [ ] `M8-07` 集成测试与文档：Conda/venv 语义一致性（模拟激活环境提交 ->
  解释器/cwd/关键变量与直接执行一致）、executable 解析失败提交即拒、
  `YORI_JOB_ID`/`YORI_GPU_UUID` 注入断言、`--inherit-env` 超限、inspect
  授权与脱敏 E2E、降权顺序回归（基线 4/16/27 复核）；同步设计文档
  （§6.1/§8.2/§13）、威胁模型（基线 26-27 状态与验证证据）、EXEC 表
  （如涉及）、总计划与本文档。

## 风险与阻塞

- `RISK-2026-014` 捕获环境持久化进入 SQLite 并在 `inspect` 暴露：以白名单
  默认 + 显式 opt-in + 敏感值脱敏 + 数据库 `0600` 收敛（基线 26）；daemon
  日志不打印 env 值。
- `RISK-2026-015` `LD_LIBRARY_PATH` 放开的安全论证依赖"降权先于 exec"不变量：
  降权顺序回归测试锁住（基线 4/16/27）；`LD_PRELOAD`/`YORI_*` 维持拒绝。
- `RISK-2026-016` 协议 v2 与 v1 混部：daemon 接受 v1 SUBMIT（无捕获语义），
  CLI 与 daemon 同版本发布；v1 客户端对 v2 daemon 行为不回退（M5 起六命令
  契约不变）。
- 白名单"daemon 配置可扩展"（DEC-011 决策 2）：当前 yorid 无配置文件机制
  （仅 CLI flags），且捕获发生在 CLI 进程内；M8 以策略结构可配置化 +
  `--capture-env KEY` 运行时扩展点落地，daemon 配置文件统一机制出现后再
  收敛（配置级变更，不需新决策）。已记入本文档，验收时复核。
- 排队期间用户环境变化不跟随（快照语义）：DEC-011 显式定义，`inspect`
  展示捕获内容消除歧义；E2E 断言快照语义。

## 测试与退出条件

- [ ] `unit`：JobSpec 新字段校验（`m1.unit.job-spec` 扩展）；四层合并次序、
  保留键修订负向（`LD_PRELOAD` 仍拒、`YORI_*` 拒绝、`LD_LIBRARY_PATH`
  放行、资源块永远胜出）（`m2.unit.launch-adapter` 扩展）；捕获白名单/
  `--inherit-env` 超限/解析失败矩阵（新增 `m8.unit.environment-capture`）；
  executable 直 exec 路径（`m2.unit.process-supervisor` 扩展）；协议 v1/v2
  矩阵与 INSPECT 编解码（`m5.unit.ipc-protocol` 扩展）；submit 新字段映射
  与 inspect 授权/脱敏（`m5.unit.ipc-service` 扩展）；schema v1->v2 迁移、
  v1 兼容读与 round-trip（`m4.unit.sqlite-state-store` 扩展）。
- [ ] `integration`：已激活环境语义一致性（模拟 Conda/venv：PATH 注入假
  解释器 + CONDA_PREFIX/VIRTUAL_ENV 提交 -> 运行时解释器、cwd、关键变量
  与直接执行一致；`YORI_JOB_ID`/`YORI_GPU_UUID` 注入断言）；executable
  解析失败提交即拒（CLI 本地拒绝路径）；`--inherit-env` 超限显式失败；
  inspect E2E（owner 可见 + 敏感值掩码；非 owner DENIED）。
- [ ] 降权顺序回归：`m2.security.process-demotion` 维持通过（基线 4/16/27
  复核；root 补跑条件沿用 M2 记录）。
- [ ] fuzz：`m5.fuzz.ipc-parser` 语料纳入 v2 SUBMIT 与 INSPECT 帧（三方向）。
- [ ] 五预设构建与 CI 全绿（format/tidy/gcc/clang/sanitizers/依赖锁定）。
- [ ] 文档同步矩阵（工程规范第 8 节）核对：设计文档（§6.1/§8.2/§13，
  版本号递增）、DEC-006 修订标注、威胁模型基线 26-27 落地状态与证据、
  总计划（§1/§5/§6/§11）、本文档验证记录、CHANGELOG（`## Unreleased`）。

## 验证记录

（实施中随轮次补记。）
