# DEC-001：Executor 依赖引入与锁定策略

> 状态：Accepted
> 日期：2026-09-02
> 负责人：Linductor-alkaid
> 冻结里程碑：M0（已随初始仓库落地）
> 替代/被替代：无

## 背景与问题

[AGENTS.md](../../AGENTS.md) 强制 Yori 的全部并发任务与生命周期管理基于
`third_party/executor`。引入方式必须满足：可离线构建、commit 精确可审计、升级
可回归、不把上游代码复制进自研源码树。

## 决策

- 以 git submodule 引入（`.gitmodules` -> `third_party/executor`）。
- 仓库根 `dependencies.lock.json`（schema_version 1）记录 name、source、path、
  ref、ref_date、version、精确 commit、许可证及许可文件路径。
- CMake configure 阶段校验 submodule HEAD 与 lock 文件的 commit 一致，不匹配即
  失败；`-DYORI_FETCH_DEPENDENCIES=OFF` 为只校验路径（离线/CI 使用），不触发
  任何拉取。
- 初始 pin：`v0.4.0-82-g4fd8e60`
  （`4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`，origin/master @ 2026-09-02，
  MIT），不等待上游发布 tag。
- 升级：独立 MR，记录旧版本、新 commit、能力变化、受影响范围与全量回归结果，
  并在 `docs/supply-chain/` 留审计记录；升级前先核对
  [反馈台账](../executor_feedback/ledger.md)状态。

## 备选方案

- FetchContent / 包管理器拉取：引入网络依赖与不可控版本解析，commit 校验弱，
  离线与可复现构建困难。
- 源内 vendored copy：升级 diff 噪声大，上游历史与许可证追踪断裂。
- 系统安装：版本漂移，无法逐 commit 审计，多机部署不一致。

## 影响与风险

- Pin 的是 origin/master 上的非发布 commit，上游演进可能引入行为变化。缓解：
  依赖只随显式升级 MR 前进，升级必须全量回归并留审计记录，不自动跟随上游。
- 新克隆必须 `git clone --recursive`（或 `git submodule update --init`）；在
  CI 脚本与仓库说明中固化（`M0-05` 落地）。

## 验证方式

- `M0-03`：configure 校验失败路径测试记录（篡改 submodule commit 后 configure
  失败）。

## 关联文档和工作项

[工程规范](../project/project-standards.md)第 9.1、10.7 节；
[依赖管理与供应链策略](../supply-chain/dependency-policy.md)；
[设计文档](../design/yori-project-design.md)第 14 节；
[M0 工程骨架与基线](../plans/m0-engineering-baseline.md) 工作项 `M0-03`。
