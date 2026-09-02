# 依赖管理与供应链策略

> 状态：Active
> 版本：1.0
> 日期：2026-09-02
> 负责人：Linductor-alkaid

## 1. 原则

- 直接依赖最小化；一切 `third_party/` 依赖 pin 到精确 commit 并在 lock 文件
  登记，CMake configure 阶段校验，不匹配即失败（[工程规范](../project/project-standards.md)
  第 10.7 节）。
- 所有直接依赖可离线校验；许可证清单与 SBOM（SPDX 或 CDX）由构建目标再生成，
  不以手工维护的清单作为唯一来源。
- 依赖升级走独立 MR，并在本目录留审计记录。

## 2. 锁定机制

采用 git submodule + 仓库根 `dependencies.lock.json`（决策依据：
[DEC-001](../decisions/DEC-001-executor-pinning.md)；工程规范第 9.1 节）。

- lock 字段：`name`、`source`、`path`、`ref`、`ref_date`、`version`、`commit`、
  `license`、`license_file`、`introduced_by`。
- `dependencies.lock.json` 是唯一权威来源；本文表格仅为人类可读摘要，两者不一致
  时以 lock 文件为准并修正本文。
- `-DYORI_FETCH_DEPENDENCIES=OFF`：不触发任何拉取，仅执行一致性校验（离线/CI
  默认路径）。
- 新克隆：`git clone --recursive` 或 `git submodule update --init`。

## 3. 当前依赖清单

| 依赖 | 引入方式 | 版本 | commit | source | 许可证 |
| --- | --- | --- | --- | --- | --- |
| executor | submodule `third_party/executor` | `v0.4.0-82-g4fd8e60` | `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9` | `https://github.com/Linductor-alkaid/executor.git` | MIT（许可文件 `third_party/executor/LICENSE`） |

引入说明：初始引入直接固定 origin/master（2026-09-02），未等待上游发布 tag，
理由与风险见 [DEC-001](../decisions/DEC-001-executor-pinning.md) 与
[设计文档](../design/yori-project-design.md)第 14 节。

## 4. 升级流程

1. 升级走独立 MR（`build(deps): ...` 或 `chore(deps): ...`）；升级 pinned
   executor 前先核对[反馈台账](../executor_feedback/ledger.md)状态，未决缺口
   需在 MR 中说明升级对缺口的影响。
2. 更新 submodule commit 与 `dependencies.lock.json`；MR 描述记录旧版本、新
   commit、能力变化与受影响范围。
3. 执行全量回归（含 sanitizer 预设）；失败则回退 pin，不带病升级。
4. 审计记录写入本目录，命名 `upgrade-executor-YYYYMMDD.md`，包含版本差异、
   回归结果与证据链接。

## 5. 新依赖准入

- 论证必要性：无法以少量自研代码在正确的层（Core/Adapter）解决。
- 许可证与项目许可证兼容；可锁定到精确 commit；可离线校验。
- 仓库自身许可证尚未确定（[总计划](../plans/yori-implementation-plan.md)第
  6 节暂定项，最迟 M7 发布前确定）；在此之前新增依赖须逐个审查许可兼容性并在
  MR 中说明结论。

## 6. SBOM 与许可证清单

- M0 提供再生成构建目标的骨架；M7 发布前以发布配置再生成并存档证据。
- 生成物（SBOM、许可证清单、fuzz corpus 等）约束在构建树内，不进入源码树
  （工程规范第 10.7 节）。
