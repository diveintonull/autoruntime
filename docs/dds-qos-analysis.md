# DDS QoS 分析

## 范围

AutoRuntime 通过 Eclipse Cyclone DDS 11.0.1 的 public C API 与生成的 IDL type 使用 DDS。Cyclone 是外部依赖；仓库不 vendor、也不修改其源码。`scripts/bootstrap_cyclonedds.sh` 下载固定 tag archive，校验 SHA-256 `c25d46075ad6b5cee564bda9e5f49509e9a6dbb1a8b858e708eb360d335cf973`，再安装到被忽略的 `.deps/`。

Cyclone 在[官方仓库](https://github.com/eclipse-cyclonedds/cyclonedds)中将自己描述为 DDS 与 DDSI-RTPS 的开放实现。项目固定使用 [11.0.1 release](https://github.com/eclipse-cyclonedds/cyclonedds/releases/tag/11.0.1)，不跟随 moving branch。这里遵循其官方模式：先用 CMake 安装，再由 downstream application 消费。

## AutoRuntime 到 DDS 的映射

| AutoRuntime QoS | Cyclone C API | Adapter 行为 |
| --- | --- | --- |
| `Reliability::Reliable` | `dds_qset_reliability(..., DDS_RELIABILITY_RELIABLE, max_blocking_time)` | history 满时，writer 最多按 configured bound 阻塞 |
| `Reliability::BestEffort` | `DDS_RELIABILITY_BEST_EFFORT` | 不请求 reliable-history blocking contract |
| `HistoryKind::KeepLast` | `dds_qset_history(..., DDS_HISTORY_KEEP_LAST, depth)` | 校验范围后传递 configured depth |
| `HistoryKind::KeepAll` | `DDS_HISTORY_KEEP_ALL` | 有意忽略 depth；DDS resource limit 仍由实现/配置负责 |
| 正数 `deadline` | `dds_qset_deadline` | 同时作用于 DataWriter 与 DataReader |
| automatic/manual liveliness | `dds_qset_liveliness` | kind 与 lease 同时作用于两端；manual writer 写前 assert liveliness |

这些 setter 来自 Cyclone 的 [C QoS API](https://cyclonedds.io/docs/cyclonedds/latest/api/qos.html)。文档说明 KeepLast 单独使用 `depth`，bounded KeepAll 需要 ResourceLimits；Reliable 的 max-blocking time 是 history 满时 writer 的上界。Deadline、Liveliness、Reliability、History 对 DataWriter 与 DataReader 的适用范围见 [QoS overview](https://cyclonedds.io/docs/cyclonedds/latest/about_dds/qos.html)。

IDL message 携带 AutoRuntime envelope version、trace/span lineage、sequence、source/publish monotonic timestamp、source generation、priority，以及最大 1 MiB 的 bounded octet sequence。service 明确返回 `Unsupported`；request/reply 属于 AutoRuntime distributed RPC layer，不会静默伪装成 topic pair。

## 实验问题

实验回答两个窄问题：

1. 在本机 compatible endpoint 下，matched Reliable 与 matched BestEffort 分别得到怎样的 send-call rate、delivery count 和 callback-observed latency？
2. 是否如 DDS requested/offered compatibility model 预期，BestEffort writer 无法满足 Reliable reader 的请求？

第二点很重要：`dds_write` 成功并不能证明存在 compatible reader。Cyclone 在 [entity status API](https://cyclonedds.io/docs/cyclonedds/latest/api/status.html) 暴露 offered/requested incompatible-QoS status。

## 方法

Snapshot date：2026-08-20。

- Host：WSL2 Linux 6.6.87.2，x86-64。
- CPU：Intel Core Ultra 9 275HX，可见 24 个 logical CPU。
- Toolchain：GCC 13.3.0、CMake 3.28.3、Ninja 1.11.1。
- Build：`Release`，Cyclone DDS 11.0.1。
- Topology：同一 process/host 内两个独立 DDS participant；每个 scenario 使用唯一 domain 与 topic。
- Payload：128 B；先做一次不计入测量的 discovery write，再 warm-up 750 ms。
- Compatible scenario：尝试 write 5,000 次，KeepAll history，deadline 2 s，automatic liveliness lease 3 s。
- Incompatible scenario：BestEffort writer 向 Reliable reader write 500 次。
- Send rate：成功 synchronous `Publish` call 数除以 producer loop 时间；它不是 E2E throughput。
- Latency：receive-callback monotonic time 减 envelope pre-write monotonic timestamp；percentile 使用 nearest-rank。
- Completion：等待全部 published sample 到达，或 750 ms 内 delivery 无进展。

可执行程序要求 compatible scenario 至少交付一条，且 incompatible scenario 一条也不能交付，否则 run 失败。原始 machine output 保留于 [`docs/evidence/dds-qos-results.json`](evidence/dds-qos-results.json)。

## 观察结果

| Scenario | Published / received | Loss | Send calls/s | P50 | P95 | P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Reliable writer / Reliable reader | 5,000 / 5,000 | 0.000% | 682,864.858 | 683.289 us | 1,074.968 us | 1,118.266 us |
| BestEffort writer / BestEffort reader | 5,000 / 5,000 | 0.000% | 853,467.776 | 985.819 us | 1,122.428 us | 1,135.278 us |
| BestEffort writer / Reliable reader | 500 / 0 | 100.000% | 2,215,094.540 | n/a | n/a | n/a |

本次 run 中，matched BestEffort 的 producer call 完成速度约高 25.0%；其 P50 callback latency 高约 44.3%，P95/P99 分别高约 4.4%/1.5%。两个 matched scenario 都交付全部 sample。这不支持“某个 policy 永远更快”的笼统结论：run 是本地、短时、未绑核的 bursty writer + polling receive thread。

incompatible pair 虽然 500 次 write 全部成功，却交付 0 条。这是最重要的行为结果：数据能否流动取决于 endpoint compatibility，而不是 writer return status 本身。

## Runtime policy 决策

- control command、state transition、health event、RPC-related message 默认用 Reliable；漏一条会改变行为。
- 仅当应用有明确 queue/drop metric 且容忍 loss 时，高频可替代 sensor stream 才用 BestEffort。
- 把 History 与 ResourceLimits 视为 reliability decision 的一部分；Reliable + unbounded backlog 不是有效 overload strategy。
- 通过 HealthMonitor 与 observability 暴露 incompatible-QoS、deadline-missed、sample-lost、liveliness status change，而不是把沉默统一解释为 timeout。

## 局限与后续

这是 controlled integration experiment，不是 network benchmark。它没有注入 packet loss、跨 physical network、绑核、隔离 DDS thread、设置显式 ResourceLimits 或比较多种 DDS 实现。BestEffort 零丢包不等于 BestEffort 一般无损。后续 multi-host run 应复用同一程序并加入 network impairment、DDS status counter、重复试验和 confidence interval。

## 复现

```bash
cd projects/autoruntime
./scripts/bootstrap_cyclonedds.sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUTORUNTIME_ENABLE_DDS=ON \
  -DAUTORUNTIME_BUILD_BENCHMARKS=ON
cmake --build build-release --target autoruntime_dds_qos_experiment
./build-release/autoruntime_dds_qos_experiment \
  --messages 5000 \
  --output docs/evidence/dds-qos-results.json
```
