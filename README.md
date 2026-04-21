# AutoRuntime

AutoRuntime 是面向 Linux 的 C++20 机器人运行时，基于 [eclipse-ecal/tcp_pubsub](https://github.com/eclipse-ecal/tcp_pubsub) 深度派生。项目保留固定的上游历史和 MIT 声明，同时用 transport-neutral runtime、明确的调度策略、健康监督、可观测性和有限 distributed control-plane primitive 替换面向应用的原有架构。


## 已实现能力

| 领域 | 当前行为 |
| --- | --- |
| Runtime API | `Node`、`Publisher`、`Subscriber`、`Service`、`Client`、`Timer`、`Executor`、`Transport`、`HealthMonitor` |
| 调度 | Periodic、Event、Async task；有界优先级队列；callback group；配置化 CPU affinity；可选 `SCHED_FIFO` 与显式权限降级；绝对周期释放；Warn/Degrade/Drop；P50/P95/P99/P99.9/MAX |
| Pub/sub | 每 subscription 独立 queue、DropNewest/DropOldest overflow、slow-callback isolation、versioned message envelope |
| Service | in-memory transport 上的 deadline-bounded request/reply；bounded service queue |
| Transport | in-memory、FastIPC shared memory，以及可选的真实 Cyclone DDS 11.0.1 adapter |
| 恢复 | heartbeat、progress、backlog、deadline miss、process exit evaluation；generation-aware cleanup/start/reconnect hook |
| 可观测性 | counter、gauge、bounded histogram、JSON log、trace span、E2E latency、CPU/RSS/context-switch snapshot |
| Record/Replay | versioned little-endian trace、CRC32、bounded async recorder、strict/continue failure policy、原速/加速/最快/单步重放 |
| 分布式切片 | 带 bounded membership 与 lease expiry 的 explicit-peer UDP discovery；带 framing、deadline 和 cancellation 的 TCP RPC |
| 验证 | 固定提交 `9af83ee3be0c`：轻依赖 35/35；DDS Debug/Release/ASan/UBSan 各 37/37；TSan 无 DDS 35/35、Record/Replay 专项 5/5；完整 DDS TSan 有外部 Cyclone 竞态，标记 **INCOMPLETE** |

## 架构

```text
Node API
  |-- Publisher / Subscriber / Service / Client / Timer
  |          |
  |          +--> Executor callback groups and bounded queues
  |
  +--> Transport interface
         |-- InMemoryTransport
         |-- FastIpcTransport --> ../fastipc
         +-- DdsTransport -----> Cyclone DDS 11.0.1

HealthMonitor   Metrics / Logs / Traces   Record / Replay   Discovery / RPC
     \                 |                         |                /
      +---------------- application policy ----------------------+
```

runtime core 不依赖任何 concrete transport type。导入的 `tcp_pubsub` 仅作为 opt-in baseline target 保留，不链接进默认 AutoRuntime library。详见 [architecture.md](docs/architecture.md) 与 [UPSTREAM_DIFF.md](UPSTREAM_DIFF.md)。

## 轻依赖构建

在仓库根目录执行：

```bash
cmake -S projects/autoruntime -B projects/autoruntime/build -G Ninja
cmake --build projects/autoruntime/build
ctest --test-dir projects/autoruntime/build --output-on-failure
```

DDS 默认为关闭，因此上述命令只需要 CMake、Ninja、C++20 compiler、pthreads 和相邻 FastIPC 项目。当前轻依赖构建注册 35 个测试，并通过 35/35。

运行 sensor -> planning -> control 示例：

```bash
projects/autoruntime/build/autoruntime_pipeline_demo
```

## 完整 DDS 与 sanitizer 矩阵

bootstrap script 会下载 Cyclone DDS 11.0.1，校验固定 SHA-256，并安装到被忽略的 `.deps` 目录：

```bash
projects/autoruntime/scripts/bootstrap_cyclonedds.sh
projects/autoruntime/scripts/run_test_matrix.sh all
```

矩阵明确设置 `AUTORUNTIME_ENABLE_DDS=ON`，并运行 Debug、Release、ASan、UBSan、TSan。固定 revision 的日志与 WSL2 TSan 启动说明见 [testing.md](docs/testing.md)。当前完整 DDS TSan 可在不同调度下分别得到 37/37 或因 Cyclone DDS 11.0.1 库内 SPDP 竞态得到 36/37；该项未过滤，状态为 **INCOMPLETE**。

## 实验与证据

Release build 提供以下程序：

```bash
projects/autoruntime/build-verify-release/autoruntime_pipeline_latency_benchmark
projects/autoruntime/build-verify-release/autoruntime_scheduling_isolation_experiment
projects/autoruntime/build-verify-release/autoruntime_realtime_scheduling_benchmark
projects/autoruntime/build-verify-release/autoruntime_priority_inversion_experiment
projects/autoruntime/build-verify-release/autoruntime_record_replay_experiment
projects/autoruntime/build-verify-release/autoruntime_dds_qos_experiment
```

测量结果与原始 JSON：

- [E2E latency 分析](docs/e2e-latency-analysis.md)
- [callback-group isolation 分析](docs/scheduling-analysis.md)
- [实时感知调度设计](docs/realtime-scheduling.md)
- [实时调度基准](docs/realtime-scheduling-benchmark.md)
- [优先级反转实验](docs/priority-inversion.md)
- [Record/Replay 设计与实测](docs/record-replay.md)
- [确定性边界](docs/determinism.md)
- [DDS QoS 分析](docs/dds-qos-analysis.md)
- [故障注入矩阵](docs/fault-injection.md)
- [恢复设计与 crash test](docs/recovery.md)

## 上游与归属

- Primary upstream：`eclipse-ecal/tcp_pubsub`
- 固定提交：`1540876ee8aad623a9b089baaf3f948579b466d9`
- 导入提交：`f00857d`
- 许可证：MIT，保留于 [LICENSE](LICENSE)
- 额外保留声明：`tcp_pubsub/src/portable_endian.h`

完整上游源码、测试、sample、历史和声明均保留在 tree 中。AutoRuntime 新模块位于独立目录和 target。准确的 Keep/Rewrite/Add 边界见 [UPSTREAM_DIFF.md](UPSTREAM_DIFF.md)。

## 已知局限

- `HealthMonitor` 提供 policy 与 recovery hook，不是 privileged process supervisor 或 deployment daemon。
- FastIPC 与 DDS adapter 目前只实现 pub/sub。runtime service 使用 `InMemoryTransport`；跨机器 service call 使用独立 distributed RPC slice。
- `Executor::Stop(Deadline)` 会请求 cooperative stop 并 join 所有 worker，但尚未执行传入的 deadline。忽略 stop token 的 callback 可无限拖延 shutdown。
- task priority 只决定 callback group 内已排队 job 的顺序；worker 的 CPU affinity 与可选 `SCHED_FIFO` 是另一层配置，仍不提供 callback 抢占、准入控制或 WCET 证明。
- 实时配置当前只覆盖 Executor scheduler 与 callback-group worker；FastIPC/DDS receiver、HealthMonitor、Discovery、RPC 线程尚未纳入。普通用户请求 `SCHED_FIFO` 时通常因 `EPERM` 回退，实际状态可查询。
- discovery 与 RPC 只支持 numeric IPv4 endpoint 和 explicit peer，不提供 authentication、encryption、consensus、NAT traversal 或 Byzantine protection。
- trace 与 histogram store 位于进程内，只在文档明确处有上界；没有 OpenTelemetry exporter 或 durable metrics backend。
- Record/Replay 当前只记录 pub/sub publish attempt，不记录 receive、delivery result 或 service/RPC；Recorder 会复制 payload 并竞争入队锁，尚未与 FastIPC loan 打通。
- trace 的 CRC32 用于损坏检测，不提供认证或抗恶意篡改；当前也没有 rotation、index、compression 或跨版本 schema migration。
- WSL2 benchmark 仅是比较证据，不代表 target-hardware 或 hard real-time guarantee。
