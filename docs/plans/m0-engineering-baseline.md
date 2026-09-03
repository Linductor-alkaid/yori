# M0：工程骨架与基线

> 状态：Completed（实现与退出条件已通过，合并 [PR #1](https://github.com/Linductor-alkaid/yori/pull/1) 待评审）
> 负责人：Linductor-alkaid
> 所属计划：[Yori 实施总计划](yori-implementation-plan.md)
> 前置：无
> 建议发布点：无（内部基线）
> 更新日期：2026-09-03

## 目标

建立可持续交付的工程基线：可构建（多预设）、可测试（ctest 与标签体系）、可校验
依赖（Executor pin 校验）、可协作（文档框架与代码规范工具），使 M1 起的每个功能
增量都有稳定落点。

## 范围与非目标

范围：

- CMake（>= 3.25）+ `CMakePresets.json`：`debug`/`release`/`asan`/`ubsan`/`tsan`
  预设与对应 `ctest` 入口。
- 设计第 18 节目录结构：`apps/yori`、`apps/yorid`、`include/yori/*`、`src`、
  `configs/profiles`、`packaging/systemd`、`tests`。
- Executor 依赖锁定校验（configure 阶段）。
- `.clang-format`、`.clang-tidy` 与 CI 格式/静态检查。
- CI 基线：Linux GCC 与 Clang 矩阵（configure、build、test、安装后最小 consumer
  编译）。
- 测试标签体系与显式 skip 机制。
- 文档框架：总计划、DEC-001 ~ DEC-004、Executor 反馈台账、依赖管理策略、威胁
  模型草案、本里程碑文档。

非目标：

- 任何产品功能（Job、队列、调度、IPC、NVML、进程守护）。
- SQLite、NVML、UDS 等真实依赖接入（属 M1+ 各 Adapter 里程碑）。
- CI 性能优化、多平台矩阵扩展（目标平台仅 Linux，工程规范第 11 节）。

## 设计与决策依据

- 目录结构：[设计文档](../design/yori-project-design.md)第 18 节。
- 依赖锁定：[DEC-001](../decisions/DEC-001-executor-pinning.md)、
  [工程规范](../project/project-standards.md)第 9.1、10.7 节。
- C++ 工程基线要求：工程规范第 11 节。
- Executor 集成约束与文档路由：[AGENTS.md](../../AGENTS.md)。
- 并发承载与关闭顺序：总计划第 4 节（EXEC 条目）。

## 工作项

- [x] `M0-01` 初始化文档框架：总计划（SCOPE/RULE/EXEC/DOD/POST 与里程碑索引）、
  决策记录 DEC-001 ~ DEC-004、Executor 反馈台账、依赖管理策略、威胁模型草案与
  本里程碑文档；编号体系与相对链接符合工程规范。
- [x] `M0-02` 建立可构建的 CMake 骨架与设计第 18 节目录结构，提供五个 sanitizer/
      构建预设与 `ctest` 入口，空目标可编译。
- [x] `M0-03` 实现 Executor 依赖锁定校验：configure 时比对 `third_party/executor`
      HEAD 与 `dependencies.lock.json` 的 commit，不匹配即失败；保留
      `-DYORI_FETCH_DEPENDENCIES=OFF` 的只校验语义。
- [x] `M0-04` 落地代码规范工具：`.clang-format`、`.clang-tidy`，CI 执行格式与
      静态检查，关键警告按仓库策略设为 error。
- [x] `M0-05` 建立 CI 基线：Linux GCC 与 Clang 矩阵执行 configure、build、test
      与安装后最小 consumer 编译；依赖真实环境的用例显式 skip 并记录补跑条件。
- [x] `M0-06` 建立测试标签体系：`unit`/`integration`/`ipc`/`security`/`recovery`/
      `fuzz`/`performance`/`platform`（需要真实 NVML GPU 的用例追加 `gpu`，需要多
      Linux 用户的用例追加 `multi-user`）注册进 ctest，以示例用例验证标签过滤与
      skip 呈现，不以"无测试可运行"冒充成功。

## 风险与阻塞

- ~~CI 运行环境（runner、发行版、编译器版本）尚未确定，可能影响 `M0-05` 的验收
  证据~~ 已解除（2026-09-03）：CI 采用 GitHub Actions `ubuntu-24.04`
  （GCC 13.3 / Clang 18），全绿证据见下方验证记录。

## 测试与退出条件

- [x] 五个构建预设（`debug`/`release`/`asan`/`ubsan`/`tsan`）在至少一个工具链上
  configure + build 通过并留有记录（GCC 13.3 本地全量，含 ctest 9/9）。
- [x] 篡改 submodule commit 后 configure 失败（`M0-03` 验收）（本地与 CI
  `dependency-pin` 任务均验证）。
- [x] CI 在骨架上完整通过一次，安装后最小 consumer 编译通过
  （[run 33680214814](https://github.com/Linductor-alkaid/yori/actions/runs/33680214814)
  8/8 任务通过）。
- [x] 示例测试用例的标签过滤与显式 skip 行为在 CI 输出中可见（build 任务
  "测试标签与显式 skip 演示"步骤：10 个标签列出，`ctest -L` 过滤生效，SKIP
  原因与补跑条件打印）。
- [x] 文档框架相对链接检查通过、编号无冲突（`M0-01` 验收）。

## 验证记录

2026-09-02：

- `M0-01` 完成（本变更）：创建 `docs/plans/yori-implementation-plan.md`、
  `docs/plans/m0-engineering-baseline.md`、
  `docs/decisions/DEC-001-executor-pinning.md` ~
  `DEC-004-privileged-daemon-demotion.md`、`docs/executor_feedback/ledger.md`、
  `docs/supply-chain/dependency-policy.md`、`docs/security/threat-model.md`。
- 环境与工作树：初始化仓库（`master` 尚无提交），Linux；Executor submodule
  `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9`（`v0.4.0-82-g4fd8e60`）与
  `dependencies.lock.json` 一致（`git submodule status`）。
- 验证：提取全部新文档的 Markdown 相对链接并逐一核对目标文件存在（脚本化检查，
  结果全部通过）；`SCOPE`/`RULE`/`EXEC`/`DOD`/`POST`/`DEC`/`M0-NN` 编号在本次
  文档集内无重复。
- 限制：文档为初始版本，随里程碑推进持续更新；本记录不涉及代码与构建验证
  （自 `M0-02` 起）。

2026-09-03（M0-02 ~ M0-06 完成，[PR #1](https://github.com/Linductor-alkaid/yori/pull/1)）：

- 环境：本地 Linux x86_64（Ubuntu 24.04，内核 7.0.0-30），GCC 13.3.0、
  CMake 3.28.3、Ninja 1.13.2、clang-format 18.1.8；CI GitHub Actions
  `ubuntu-24.04`（GCC 13.3 / Clang 18，clang-format/tidy 18）。
- 构建：五个预设（`debug`/`release`/`asan`/`ubsan`/`tsan`）本地 configure +
  build + ctest 全部通过（9/9，其中 6 项为预期显式 skip；含
  `-DYORI_FETCH_DEPENDENCIES=OFF` 只校验路径）。命令：`cmake --preset <p> -DYORI_FETCH_DEPENDENCIES=OFF && cmake --build --preset <p> && ctest --preset <p>`。
- TSAN 限制：新内核高 ASLR 熵（vm.mmap_rnd_bits=32）与 GCC 13 libtsan shadow
  mapping 冲突（`FATAL: ThreadSanitizer: unexpected memory mapping`）；以
  `setarch -R ctest --preset tsan`（关闭 ASLR）运行通过，CI 同；补跑条件：
  无需 setarch 的环境可直接 `ctest --preset tsan`。原因属平台/运行时组合，
  不是 Executor 或 Yori 缺陷。
- `M0-03` 验收（本地与 CI）：将 submodule 切到 pinned commit 的父提交后
  configure 以非零退出失败，错误信息指向 `dependencies.lock.json` 并列出
  期望/实际 commit 与修复指引；恢复 pin 后 OFF 只校验路径 configure 成功。
- `M0-05` 验收（CI run
  [33680214814](https://github.com/Linductor-alkaid/yori/actions/runs/33680214814)，
  8/8 任务通过）：clang-format（--Werror）、clang-tidy（关键类别
  WarningsAsErrors）、gcc-13/clang-18 × debug/release 矩阵（configure、
  build、ctest、install、安装后最小 consumer 编译运行，输出
  `consumer linked against yori 0.1.0`）、asan/ubsan/tsan、依赖锁定篡改
  验收。clang-tidy 本地未安装（无 sudo 权限），静态检查结论以 CI 为准。
- `M0-06` 验收（CI 输出可见）：`ctest -N --print-labels` 列出全部 10 个标签
  （8 基础 + gpu + multi-user）；`ctest -L 'gpu|multi-user|ipc|recovery|fuzz|
performance' -V` 过滤生效，skip 用例打印原因与补跑条件（如
  `SKIP: 需要真实 NVIDIA GPU（NVML）；补跑条件：在带 GPU 的 Linux 主机运行
  ctest -L gpu`）。
- 供应链骨架：`cmake --build build/release --target yori_supply_chain_report`
  在构建树生成 `license-manifest.md` 与 `sbom.spdx.json`（executor v0.4.0-82
  MIT），源码树无生成物。
- 已知未执行项：`gpu`、`multi-user` 示例用例在本地与 CI 均 skip（无 NVML
  GPU、非 root）；负责人 Linductor-alkaid；补跑条件：在带 NVIDIA GPU 的
  Linux 主机运行 `ctest -L gpu`，在 root + 多用户环境运行
  `sudo ctest -L multi-user`。M0 示例仅验证 skip 机制，真实用例随 M2/M3/M5
  落地。
- Git：空仓库引导——`master` 以两个 M0-01 基线提交初始化（依赖锁定 + 文档
  框架，均已在 M0-01 验证记录中核实；该推送时仓库无任何 CI 工作流，未产生
  CI 结果）；`M0-02` ~ `M0-06` 及两个 CI 修复提交在
  `build/m0-engineering-baseline` 分支，经 PR #1 触发上述全绿 CI。合并由
  维护者执行（Agent 不合并 PR，AGENTS.md Git 纪律）。
- CI 修复过程（同一请求内的后续提交）：runner git 2.55 对
  `git checkout --detach HEAD~1` 解析不同 + submodule 浅克隆（depth=1）
  导致 `HEAD~1` 不可解析；改为 `fetch --depth=2` 补取父提交后切换，
  语义不变（run 33679893987 失败 -> 33680076218 部分失败 ->
  33680214814 全绿）。
