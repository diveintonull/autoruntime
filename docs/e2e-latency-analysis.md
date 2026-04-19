# Sensor-to-control latency 实验

日期：2026-08-20

## 问题

sensor 通过彼此独立的 planning/control callback group 发布有限 burst 时，当前 AutoRuntime executor 会产生怎样的 E2E latency 与 backlog？

## 复现

```bash
cmake -S projects/autoruntime -B projects/autoruntime/build-dds-release \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAUTORUNTIME_ENABLE_FASTIPC=ON \
  -DAUTORUNTIME_ENABLE_DDS=ON \
  -DAUTORUNTIME_BUILD_TESTS=ON \
  -DAUTORUNTIME_BUILD_BENCHMARKS=ON
cmake --build projects/autoruntime/build-dds-release \
  --target autoruntime_pipeline_latency_benchmark -j2
projects/autoruntime/build-dds-release/autoruntime_pipeline_latency_benchmark \
  --messages 5000 --payload-bytes 256 \
  --output projects/autoruntime/docs/evidence/pipeline-latency-results.json
```

运行环境：WSL2 Linux 6.6.87.2、Intel Core Ultra 9 275HX、24 个可用 logical CPU、GCC 13.3.0、CMake 3.28.3、Ninja 1.11.1。thread 未绑核，也没有请求 real-time scheduler policy。

## 测量语义

benchmark 使用 production `Node`、`Publisher`、`Subscriber`、`Executor`、`InMemoryTransport`、`MetricsRegistry`、`StructuredLogger`、`TraceRecorder`。

trace 在发布 `sensor/input` 前立即开始，在 control callback 消费对应 `planning/output` 后结束。每条消息产生三个 span：

1. sensor publication，以及投递至 planning callback；
2. planning callback 与 publication；
3. 投递至 control callback 并执行。

workload 是一次 burst，不是 periodic sensor；它有意测量 overload 下 queue build-up。transport 为 process-local，因此数字不包含 serialization、shared-memory、DDS 或 network cost。

## 结果

| 指标 | 观察值 |
| --- | ---: |
| Attempted / completed | 5,000 / 5,000 |
| Payload | 256 B |
| Trace span | 15,000 |
| Publish / trace failure | 0 / 0 |
| Throughput | 76,415.820 msg/s |
| E2E minimum | 83.346 us |
| E2E mean | 26,111.619 us |
| E2E P50 | 19,985.642 us |
| E2E P95 | 60,932.585 us |
| E2E P99 | 62,846.876 us |
| Planning queue high watermark | 4,645 |
| Control queue high watermark | 905 |
| Dropped message | 0 |
| Executor deadline miss | 0 |
| Executor callback P99 | 179.176 us |
| Executor queue-delay P99 | 4.728 us |
| Maximum resident set | 11,460,608 B |
| User / system CPU | 0.056 s / 0.030 s |

machine-readable 原始证据见 [`pipeline-latency-results.json`](evidence/pipeline-latency-results.json)，其中还包含完整 metric snapshot、CPU/context-switch sample 与两条 structured lifecycle log。

## 解读

executor dispatch delay 相比 E2E latency 很小。long tail 来自 admission：producer 几乎把全部 5,000 条消息塞进 planning subscription，单 worker 来不及 drain。这与 planning high watermark 4,645 一致。真实部署应按 sensor freshness 限制 queue，选择显式 drop policy，或按 sensor rate pace/admit work。

本 run 展示 finite-burst 无丢失行为与可观察 backlog，不建立 hard real-time bound。percentile 只来自一次未绑核 WSL2 run，应作为可复现证据，而不是通用 platform claim。
