# Sensor-to-control latency experiment

Date: 2026-08-20

## Question

What end-to-end latency and backlog does the current AutoRuntime executor
produce when a sensor publishes a finite burst through separate planning and
control callback groups?

## Reproduction

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

The run used Linux 6.6.87.2 under WSL2, an Intel Core Ultra 9 275HX with
24 available logical CPUs, GCC 13.3.0, CMake 3.28.3, and Ninja 1.11.1.
Threads were not pinned and no real-time scheduler policy was requested.

## Measurement semantics

The benchmark uses the production `Node`, `Publisher`, `Subscriber`,
`Executor`, `InMemoryTransport`, `MetricsRegistry`,
`StructuredLogger`, and `TraceRecorder` implementations.

A trace starts immediately before `sensor/input` is published and finishes
after the control callback consumes the corresponding `planning/output`
message. Each message produces three spans:

1. sensor publication and delivery to the planning callback;
2. planning callback and publication;
3. delivery to and execution of the control callback.

The workload is a single burst, not a periodic sensor. It intentionally
measures queue build-up under overload. The transport is process-local, so
these figures do not include serialization, shared-memory, DDS, or network
cost.

## Result

| Measure | Observed |
| --- | ---: |
| Attempted / completed | 5,000 / 5,000 |
| Payload | 256 bytes |
| Trace spans | 15,000 |
| Publish / trace failures | 0 / 0 |
| Throughput | 76,415.820 messages/s |
| E2E minimum | 83.346 us |
| E2E mean | 26,111.619 us |
| E2E P50 | 19,985.642 us |
| E2E P95 | 60,932.585 us |
| E2E P99 | 62,846.876 us |
| Planning queue high watermark | 4,645 |
| Control queue high watermark | 905 |
| Dropped messages | 0 |
| Executor deadline misses | 0 |
| Executor callback P99 | 179.176 us |
| Executor queue-delay P99 | 4.728 us |
| Maximum resident set | 11,460,608 bytes |
| User / system CPU | 0.056 s / 0.030 s |

The raw, machine-readable evidence is
[`pipeline-latency-results.json`](evidence/pipeline-latency-results.json).
It also contains the complete metric snapshot, CPU/context-switch sample, and
two structured lifecycle log records.

## Interpretation

The executor dispatch delay is small compared with end-to-end latency.
The long tail comes from admission: the producer places nearly the entire
5,000-message burst into the planning subscription before its single worker
can drain it. This is visible in the 4,645-message planning high watermark.
A real deployment should bound that queue according to sensor freshness,
select an explicit drop policy, or pace/admit work at the sensor rate.

This run demonstrates lossless finite-burst behavior and observable backlog;
it does not establish a hard real-time bound. Reported percentiles are one
run on an unpinned WSL2 host and should be treated as reproducible evidence,
not a universal platform claim.
