# Executor 依赖升级审计记录：2026-09-09

> 状态：Completed（本地全量回归通过；待独立 MR 合入）
> 关联决策：[DEC-001](../decisions/DEC-001-executor-pinning.md)
> 关联策略：[依赖管理与供应链策略](dependency-policy.md) 第 4 节

## 版本差异

| 项 | 旧 pin | 新 pin |
| --- | --- | --- |
| commit | `4fd8e6097879a56c7c3ad33b10f803cfe2e2e4d9` | `e2dc8ca2243345e2e6cf35b395793a58796457b9` |
| ref | origin/master @ 2026-09-02 | origin/master @ 2026-09-09 |
| version 字段 | `v0.4.0-82-g4fd8e60` | `0.4.0+e2dc8ca` |

版本记法变更：上游 `v0.4.0` tag（`92fcae0`）不在 master 提交线上，`git describe`
在 submodule 克隆内对两个 pin 均不可复现（"v0.4.0-82-g…" 的计数来源无法从克隆
推导）。自本次升级起 `version` 字段改用「上游 CMake 项目版本 + 短哈希」记法，
`commit` 字段始终是唯一权威锚点。

上游差值：4fd8e60..e2dc8ca，共 13 个非 merge 提交（PR #180–#184，上游 CI 全绿）：

- `fed4204` fix(executor)：单原子准入门闩消除停机与提交交错的生命周期竞态
  （P-001/P-002）。
- `e0924c4`（PR #181）：Windows processor-group 亲和（>64 CPU）、引用计数进程
  内存锁租约（`d505508`）、lockfree 环容量取整修正（`af0e8f8`）及测试修复。
- `19e008e`（PR #182）：文档同步——2026-09 性能审计收敛计划、`docs/API.md`
  扩充、英文 API reference、changelog 与设计文档对齐。
- `d5af981`（PR #183）：ThreadPool 提交热路径重建（generation-based futex
  parking，`c9003c7`）。
- `e2dc8ca`（PR #184）：P2 lock-free 任务池、MPMC 消费侧、空闲 worker futex
  驻停（替代原 1µs-sleep 轮询，空闲态约 10⁶ syscall/s/核 降为忙碌路径零
  syscall，`118b70a`）。

## 受影响范围

- **公开 API：零影响。** 4fd8e60..e2dc8ca 间公开头文件仅
  `include/executor/lockfree_task_executor.hpp` 变更（+48/-3：注释澄清、
  test-only 钩子、私有位的单原子门闩重构）。Yori 使用的
  `executor/executor.hpp`、`executor/comm/channel.hpp`、
  `executor/comm/phase_gate.hpp`、`executor/comm/double_buffer.hpp` 均零变更，
  自研源码无需任何改动。
- **实现层：全量重建。** `src/executor/thread_pool/`、`src/util/object_pool.hpp`、
  `src/util/lockfree_queue.hpp` 等实现重构，`libexecutor` 以新 pin 全量重编。
  Yori 链接 facade 库 `executor::executor`（ThreadPool 路径），收益为提交热路径
  与空闲驻停的系统调用削减；Yori 不使用 LockFree 组件，P-001/P-002 修复对
  Yori 为无风险随带收敛。
- **反馈台账：无未决记录**（[台账](../executor_feedback/ledger.md)当前为空），
  无缺口影响需要评估。

## 全量回归

环境：Linux x86_64、GCC 13.3.0（Ubuntu 24.04）、Executor pin `e2dc8ca`、
submodule HEAD 与 `dependencies.lock.json` 一致（configure 校验通过）。

| 预设 | 结果 |
| --- | --- |
| `debug` | 31/31 通过 |
| `release` | 31/31 通过 |
| `asan` | 31/31 通过 |
| `ubsan` | 31/31 通过 |
| `tsan` | 31/31 通过（需 `setarch $(uname -m) -R ctest --preset tsan`，已知内核 ASLR 限制，与升级无关） |

各预设 6 项环境受限用例维持既有状态：`m2.security.process-demotion`（需
root）、`m3.platform.gpu-nvml`（需真实 NVIDIA GPU）、`example.*`（后续里程碑
占位）。未执行项不因本次升级改变结论。

## 结论

升级安全且有益：API 层零改动、五预设全量回归通过；随带获得上游停机/提交
竞态修复与提交热路径、空闲驻停性能收益，以及与 pin 一致的最新集成文档。
