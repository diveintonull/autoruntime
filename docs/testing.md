# 构建与验证矩阵

日期：2026-08-21

提交身份改写导致较早证据目录中的旧 ID 与当前历史不同；对照见 [提交身份改写映射](../../../COMMIT_IDENTITY_MAP.md)。本页最新一轮完整矩阵固定到 loaned IPC/rclcpp 生命周期修复提交，不把偶发通过写成稳定通过。

## 已验证 revision 与 host

| 字段 | 值 |
| --- | --- |
| 当前实现 revision | `d25967384beb0c01bb47bbdb689220b9a0cee25e` |
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
| Default | FastIPC，DDS OFF | 41/41 | [日志](evidence/test-default-d259673.log) |
| Debug | FastIPC + Cyclone DDS | 46/46 | [日志](evidence/test-debug-d259673.log) |
| Release | FastIPC + Cyclone DDS + benchmark smoke | 50/50 | [日志](evidence/test-release-d259673.log) |
| ASan | DDS ON，address + leak check | 46/46 | [日志](evidence/test-asan-d259673.log) |
| UBSan | DDS ON，首个 UB 即停止 | 46/46 | [日志](evidence/test-ubsan-d259673.log) |
| TSan 当前完整一次 | DDS ON，首个 race 即停止；使用 `setarch` 启动 wrapper | 46/46 | [日志](evidence/test-tsan-d259673.log) |
| TSan DDS RPC 重复 | aggregate RPC test，连续 20 次 | 20/20 次通过 | [日志](evidence/test-tsan-dds-rpc-repeat20-b2bc5c6.log) |
| 历史 TSan DDS 重复 | participant-loss，`until-fail:100` | 第 1 次通过，第 2 次失败 | [失败日志](evidence/test-tsan-dds-race-9af83ee3be0c.log) |

六份当前日志的 SHA-256 依次为：Default `ba477ee5ccec86d0d550aea12566998b3b2d22c880a6d030546dfee7ecefcd5e`、Debug `810b925b81841d602435d8be6d856ba8462608894c931f9872c2334e042e658e`、Release `34d8e99bc92fc71447c09e3d64c2abf66d483351905da9a76491503d25ce061d`、ASan `9542a9124027144f926471a1b8e112f66442be4003f4bef233df080388d11d54`、UBSan `260c544aeb8fdf1ae684af282890f4420bd8f8bd1b0b73ef2500e67eb1037f79`、TSan `dc251b8e6e0db64ace0235530f82b229dc38a4f02c537f42e50715ffc335d40f`。

当前实现的完整 DDS TSan 为 46/46，DDS RPC aggregate test 在 TSan 下连续 20 次通过；但完整 DDS TSan 总体状态仍是 **INCOMPLETE**，不能用当前一次通过抹掉已复现的历史外部竞态。历史失败堆栈位于外部 `libddsc.so.11`：Cyclone DDS 的 SPDP 更新线程释放 buffer 时，另一内部线程仍在 `sendmsg`。当前结果隔离了项目增量，却不能替代 Cyclone DDS 上游修复、升级验证或更长时间 soak。

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

当前注册 41 个测试，其中 24 个带 `fault` label、4 个带 `realtime` label、5 个带 `record_replay` label、3 个带 `zero_copy` label、2 个带 `comparative` label。

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

## DDS Request/Response 证据

实现固定在提交 `b2bc5c6b051736999fc53a5f2db98376e7dd9750`，使用两个真实 Cyclone DDS participant 验证控制面 service/client、request ID correlation、typed remote error、deadline timeout、late response 丢弃、close 唤醒 pending call，以及 16 路并发 correlation：

- Default 35/35；DDS Debug、ASan、UBSan、TSan 各 42/42；Release 含 benchmark smoke 为 43/43，原始日志见上表；
- [并发 correlation 连续 100 次](evidence/test-dds-rpc-concurrent-repeat100-b2bc5c6.log)，每次 16 个精确 payload，共 1600 个并发请求全部匹配；
- [timeout/late-response correlation 连续 50 次](evidence/test-dds-rpc-timeout-repeat50-b2bc5c6.log)，均未把迟到响应误配给后续请求；
- [TSan aggregate RPC test 连续 20 次](evidence/test-tsan-dds-rpc-repeat20-b2bc5c6.log) 全部通过；
- [正式 Release JSONL](evidence/dds-rpc-2026-08-21-b2bc5c6.jsonl) 的 SHA-256 为 `a91f0ab37aabfe4ffd1f6d6cd2b550175abbaa4a9da78cd64d3597ea66221213`：9000/9000 次调用成功，三类错误计数均为 0。完整方法、分位数和解释边界见 [DDS RPC 基准](dds-rpc-benchmark.md)。

## 统一对比基准证据

loaned FastIPC adapter、AutoRuntime 双进程 runner 与可选 rclcpp baseline 固定在 `7af52971199d744cd9468d614c831516ff72e9ec`；连续 case 的 rclcpp responder 在 fork 后改为 exec 新地址空间，修复固定在 `d25967384beb0c01bb47bbdb689220b9a0cee25e`。容器内连续三 case CTest 为 1/1，当前 AutoRuntime 六 profile 结果见上表。

[正式结果报告](comparative-benchmark-results.md) 保存默认 64 B 至 1 MiB、4 MiB 25 Hz 稳态和 4 MiB 100 Hz 压力三组实验，以及六份原始 JSONL 的 SHA-256。默认 AutoRuntime DDS 有 1 个失败 trial，4 MiB 100 Hz 的两条 DDS 路径全部过载失败；这些结果没有被删除或重跑覆盖。报告只从无丢失的组得出稳态观测结论，跨主机比较与硬件 DRAM bandwidth 明确为 **INCOMPLETE**。

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

这些日志不能替代当前 loaned IPC 与统一对比增量后的 41/46/50-test 结果。

## 持续集成

根 [CI workflow](../../../.github/workflows/ci.yml) 构建五个 FastIPC 与五个 AutoRuntime profile，上传 CTest 日志，并在 Release 执行 pipeline、callback isolation、DDS QoS、Record/Replay 和 DDS RPC 五个 benchmark smoke。完整 TSan job 保留 DDS participant-loss test，因此外部竞态可能使 CI 失败；当前没有把它设为 allow-failure。

## 证据边界

production sign-off 仍需 target-hardware soak、真实 network impairment、security review、ABI/package validation、deterministic real-time analysis，以及 Cyclone DDS TSan race 的上游修复或版本验证。本页只证明列出的配置和执行；任何失败都保留为原始日志并标为 **INCOMPLETE**。
