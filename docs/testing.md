# 构建与验证矩阵

日期：2026-08-20
提交身份改写导致当前提交 ID 与原始证据目录中的旧 ID 不同；对照见 [提交身份改写映射](../../../COMMIT_IDENTITY_MAP.md)。原始测试日志未改写。

## 已验证 revision 与 host

| 字段 | 值 |
| --- | --- |
| 完整矩阵 revision | `fc8150d88226a68392105cd6e071f8073acc0353f` |
| Host | WSL2 下 Ubuntu 24.04.4 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| Architecture | x86_64 |
| Compiler | GNU 13.3.0 |
| Generator | Ninja 1.11.1 |
| CMake | 3.28.3 |
| External DDS | Cyclone DDS 11.0.1，固定 bootstrap |

## 轻依赖默认构建

普通 workflow：

```bash
cmake -S projects/autoruntime -B projects/autoruntime/build -G Ninja
cmake --build projects/autoruntime/build
ctest --test-dir projects/autoruntime/build --output-on-failure
```

DDS 默认关闭。全新的 Debug directory 构建全部默认 target，并通过 26/26：[原始日志](evidence/test-default.log)。它包含 FastIPC、distributed socket、19 个 non-DDS fault case，以及可运行 pipeline example。

## 完整本地命令

先 bootstrap 固定的外部 DDS 实现一次，再运行单 profile 或完整矩阵：

```bash
projects/autoruntime/scripts/bootstrap_cyclonedds.sh
projects/autoruntime/scripts/run_test_matrix.sh all
# 或：debug | release | asan | ubsan | tsan
```

每个完整 profile 都启用 FastIPC adapter、真实 Cyclone DDS adapter、distributed socket、example 与全部 test。Release 还构建 experiment。

## 已记录的完整矩阵

| Profile | 配置 | 结果 | 原始日志 |
| --- | --- | ---: | --- |
| Debug | FastIPC + Cyclone DDS 11.0.1 | 28/28 | [日志](evidence/test-debug.log) |
| Release | FastIPC + Cyclone DDS 11.0.1 | 28/28 | [日志](evidence/test-release.log) |
| ASan | Debug，address + leak check | 28/28 | [日志](evidence/test-asan.log) |
| UBSan | Debug，首个 UB 即停止 | 28/28 | [日志](evidence/test-ubsan.log) |
| TSan | Debug，首个 race/deadlock report 即停止 | 28/28 | [日志](evidence/test-tsan.log) |

五个 profile 共执行 140/140 个 registered CTest entry。每个 profile 包含 20 个独立命名 fault case。真实 FastIPC SIGKILL/restart/data-flow test 还连续通过十次 Release：[重复日志](evidence/recovery-repeat-10.log)。

compiler warning 使用 `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`；记录的 build 无 warning。

## Sanitizer 范围

`AUTORUNTIME_SANITIZER` 只接受 `ASan`、`UBSan`、`TSan` 或空值。它 instrument AutoRuntime、生成的 DDS type、in-tree FastIPC library、test 和 benchmark target。固定的外部 Cyclone DDS shared library 不会在每种 sanitizer 下重建。

更强的 crash/restart integration 在 ASan 下暴露真实 FastIPC receive-versus-`munmap` 缺陷。commit `31b2107` 加入 active-operation lease 与直接 regression；本矩阵记录发生在修复之后。sanitizer 通过只表示已执行 schedule 没有报告，不证明缺陷不存在。

## WSL2 TSan 说明

GCC TSan 最初会在该 WSL2 host 的 `main` 前因 `unexpected memory mapping` 退出。在以下 wrapper 下运行 CTest 可避免 virtual-address collision：

```bash
setarch "$(uname -m)" -R ctest --test-dir BUILD --output-on-failure
```

script 只在 WSL 使用 wrapper；native Linux 直接运行 CTest。没有过滤 race 或 deadlock report。

## 持续集成

根 [CI workflow](../../../.github/workflows/ci.yml) 运行五个 FastIPC 与五个 AutoRuntime Ubuntu 24.04 job，缓存已验证 Cyclone DDS install，上传 CTest log，并运行全部三个 Release benchmark smoke。嵌套的上游 workflow file 只是 provenance artifact，不是 active monorepo entry point。

## 证据边界

production sign-off 仍需 target-hardware soak、真实 network impairment、security review、ABI/package validation 和 deterministic real-time analysis。该矩阵只证明已检查配置下的可复现行为。
