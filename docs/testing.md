# 构建与验证矩阵

日期：2026-08-21

提交身份改写导致较早证据目录中的旧 ID 与当前历史不同；对照见 [提交身份改写映射](../../../COMMIT_IDENTITY_MAP.md)。本页最新一轮证据固定到 Record/Replay 实现提交，不把偶发通过写成稳定通过。

## 已验证 revision 与 host

| 字段 | 值 |
| --- | --- |
| 当前实现 revision | `9af83ee3be0cc7f1cba7aaccd6a8a5186d7f42ef` |
| Host | WSL2 下 Ubuntu 24.04.4 |
| Kernel | `6.6.87.2-microsoft-standard-WSL2` |
| Architecture | x86_64 |
| Compiler | GNU 13.3.0 |
| Generator | Ninja 1.11.1 |
| CMake | 3.28.3 |
| External DDS | Cyclone DDS 11.0.1，固定 bootstrap |

## 当前验证结果

| Profile | 配置 | 结果 | 原始日志 |
| --- | --- | ---: | --- |
| Default | FastIPC，DDS OFF | 35/35 | [日志](evidence/test-default-9af83ee3be0c.log) |
| Debug | FastIPC + Cyclone DDS | 37/37 | [日志](evidence/test-debug-9af83ee3be0c.log) |
| Release | FastIPC + Cyclone DDS | 37/37 | [日志](evidence/test-release-9af83ee3be0c.log) |
| ASan | DDS ON，address + leak check | 37/37 | [日志](evidence/test-asan-9af83ee3be0c.log) |
| UBSan | DDS ON，首个 UB 即停止 | 37/37 | [日志](evidence/test-ubsan-9af83ee3be0c.log) |
| TSan 隔离 | DDS OFF，首个 race 即停止 | 35/35 | [日志](evidence/test-tsan-nodds-9af83ee3be0c.log) |
| TSan Record/Replay | DDS ON，只跑 `record_replay` label | 5/5 | [日志](evidence/test-tsan-record-replay-9af83ee3be0c.log) |
| TSan 完整一次 | DDS ON | 37/37 | [通过日志](evidence/test-tsan-pass-9af83ee3be0c.log) |
| TSan DDS 重复 | participant-loss，`until-fail:100` | 第 1 次通过，第 2 次失败 | [失败日志](evidence/test-tsan-dds-race-9af83ee3be0c.log) |

因此完整 DDS TSan 状态是 **INCOMPLETE**，不能因某一次 37/37 就写成稳定通过。失败堆栈位于外部 `libddsc.so.11`：Cyclone DDS 的 SPDP 更新线程释放 buffer 时，另一内部线程仍在 `sendmsg`。不链接 Cyclone 的完整 TSan 35/35，DDS 构建中的 Record/Replay 专项也为 5/5，这些证据隔离了当前增量，但不能替代修复或升级外部 DDS。

## 轻依赖默认构建

在仓库根目录执行：

```bash
cmake -S projects/autoruntime \
  -B projects/autoruntime/build-verify-default -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAUTORUNTIME_ENABLE_DDS=OFF
cmake --build projects/autoruntime/build-verify-default --parallel 2
ctest --test-dir projects/autoruntime/build-verify-default \
  --output-on-failure
```

当前注册 35 个测试，其中 23 个带 `fault` label、4 个带 `realtime` label、5 个带 `record_replay` label。

## 完整 DDS 与 sanitizer 命令

先 bootstrap 固定 DDS 一次，再运行 profile：

```bash
bash projects/autoruntime/scripts/bootstrap_cyclonedds.sh
bash projects/autoruntime/scripts/run_test_matrix.sh debug
bash projects/autoruntime/scripts/run_test_matrix.sh release
bash projects/autoruntime/scripts/run_test_matrix.sh asan
bash projects/autoruntime/scripts/run_test_matrix.sh ubsan
bash projects/autoruntime/scripts/run_test_matrix.sh tsan
```

脚本明确启用 FastIPC、真实 Cyclone DDS、distributed socket、example 与全部 test。Release 还构建 experiment。由于当前工作树中这两个脚本存在用户保留的 executable-bit 变更，文档使用 `bash script`，没有改回其 mode。

## Record/Replay 证据

Release 实验命令：

```bash
projects/autoruntime/build-verify-release/autoruntime_record_replay_experiment \
  --messages 5000 --payload-size 256 --period-us 100 \
  --output projects/autoruntime/docs/evidence/record-replay-2026-08-21-9af83ee3be0c.json
```

[原始 JSON](evidence/record-replay-2026-08-21-9af83ee3be0c.json) 的 SHA-256 是 `448c8ce07159700dadd5ddcb0476b72ebde6c9c13662cabf4e68e5d6ea39553c`。关键结果：

- source revision：`9af83ee3be0c`，`passed=true`；
- accepted/written/read/delivered 均为 5000，payload 共 1,280,000 bytes；
- drop、enqueue timeout、I/O error、checksum mismatch、sequence mismatch、truncation、format error 均为 0；
- 原始与重放 digest 均为 `0xda6d7cb476c5ec00`；
- 5000 个 timing observation 的平均 drift 62.628 µs、最大 drift 231.330 µs；
- trace 2,000,040 bytes，record 516.566 ms，原速 replay 500.024 ms；
- 这些是 WSL2 单次测量，不是 target-hardware hard real-time 保证。

集成测试还使用 5000 个 Sensor 输入，记录 Sensor/Planning/Control 共 15000 条 record，销毁首个 Runtime 后在新 Runtime 中重放，并逐字段检查 trace、逐 payload 比较 5000 个 Control 输出与 digest。

## Sanitizer 范围

`AUTORUNTIME_SANITIZER` 只接受 `ASan`、`UBSan`、`TSan` 或空值。它 instrument AutoRuntime、生成的 DDS type、in-tree FastIPC library、test 和 benchmark target。固定的外部 Cyclone DDS shared library 不是按每种 sanitizer 重建的，因此 DDS 库内报告必须单独归因，不能算作项目代码通过，也不能静默 suppress。

GCC TSan 在该 WSL2 host 需要以下 wrapper 避免 `unexpected memory mapping`：

```bash
setarch "$(uname -m)" -R ctest \
  --test-dir BUILD --output-on-failure
```

wrapper 只处理 virtual-address collision，不过滤 race 或 deadlock report。

## 历史基线

实时调度提交 `0c0935c63b40e8919bd10333ff0be4cace524d77` 的旧 32-test DDS 矩阵仍保留为历史证据：

- [Debug](evidence/test-debug-0c0935c63b40.log)
- [Release](evidence/test-release-0c0935c63b40.log)
- [ASan](evidence/test-asan-0c0935c63b40.log)
- [UBSan](evidence/test-ubsan-0c0935c63b40.log)
- [TSan](evidence/test-tsan-0c0935c63b40.log)

这些日志不能替代新增 Record/Replay 后的 35/37-test 结果。

## 持续集成

根 [CI workflow](../../../.github/workflows/ci.yml) 构建五个 FastIPC 与五个 AutoRuntime profile，上传 CTest 日志，并在 Release 执行 pipeline、callback isolation、DDS QoS 和 Record/Replay 四个 benchmark smoke。完整 TSan job 保留 DDS participant-loss test，因此外部竞态可能使 CI 失败；当前没有把它设为 allow-failure。

## 证据边界

production sign-off 仍需 target-hardware soak、真实 network impairment、security review、ABI/package validation、deterministic real-time analysis，以及 Cyclone DDS TSan race 的上游修复或版本验证。本页只证明列出的配置和执行；任何失败都保留为原始日志并标为 **INCOMPLETE**。
