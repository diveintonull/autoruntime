# DDS Request/Response 基准方法

## 目的

`autoruntime_dds_rpc_benchmark` 测量 AutoRuntime 自定义 DDS Request/Response 的同机端到端代价。它回答“通过真实 Cyclone DDS participant 和 Node Service/Client 完成一次 echo 调用需要多久、消耗多少进程资源”，不回答与 ROS 2 `rclcpp` 的相对优劣。

比较基准属于下一阶段；在共享 runner、payload、迭代数与环境控制对齐之前，本页不发布不公平的跨框架结论。

## 被测路径

每次 invocation 创建：

- 同一进程内两个真实 Cyclone DDS 11.0.1 participant；
- server/client 各一个 `DdsTransport`；
- 一个 `Node::Service` 和一个 `Node::Client`；
- 一个带两个 worker 的 service callback group；
- `AutoRuntimeDdsRpcBenchmark` echo service。

完整路径是：

```text
Client::Call
 -> DDS request writer
 -> DDS request reader thread
 -> Node bounded service queue
 -> Executor callback
 -> DDS response writer
 -> DDS response reader thread
 -> pending request waiter
```

这不是 transport-only ping-pong。数字同时包含 AutoRuntime envelope、IDL 编解码、DDS、线程唤醒、Executor dispatch 和 response correlation。

## 正确性护栏

payload 前 8 B 存放 sequence，其余每一字节由 sequence 与 offset 的确定公式生成。server 收到 request 后遍历完整 payload 验证，再原样 echo；client 对 response 做逐字节比较并再次完整验证。

每个结果记录同时输出：

- `expected_calls`、`completed_calls`；
- `call_failures`；
- `payload_mismatches`；
- `service_validation_failures`；
- 双向逻辑 payload byte 数。

任何计数不相等，record 的 `status` 都是 `failed`，进程返回非零；不会只输出看似漂亮的 latency。

## 计时与 trial

默认 case：

| Payload | Warmup | Measurement | Trials |
| ---: | ---: | ---: | ---: |
| 64 B | 100 | 1000 | 3 |
| 1 KiB | 100 | 1000 | 3 |
| 64 KiB | 100 | 1000 | 3 |

discovery probe 和每个 trial 的 warmup 都在计时窗口之外。measurement 使用单个 outstanding request，因而是顺序 request/response latency，不代表最大并发吞吐或 fan-out capacity。

每个成功调用记录 steady-clock E2E latency，输出 P50、P95、P99、P99.9、MAX。吞吐字段包括 calls/s 与 request+response 两个方向的逻辑 payload MiB/s。

## 资源指标

measurement 前后调用 `SampleProcessCpu`，记录：

- user/system/total CPU time；
- 以 total CPU time / wall time 计算的进程 CPU utilization；
- voluntary/involuntary context switch 增量；
- process peak RSS。

server/client 位于同一进程，因此 CPU 与 RSS 是两端和 Executor/DDS 线程的合计，不是单端成本。外部 Cyclone DDS shared library 没有被 sanitizer 重新构建，但其线程消耗包含在进程指标中。

## JSONL schema

输出第一行是 `environment`，随后每个 payload/trial 一行 `result`。所有行共享：

- `schema_version=1`；
- `benchmark=autoruntime_dds_rpc`；
- 同一 `run_id`；
- build-time source revision。

environment 还包含 UTC、host、kernel、CPU、compiler、build type、内存、DDS domain、topology。result 包含 case/trial、精确计数、latency、throughput 与资源指标。

## 构建与运行

先准备固定 Cyclone DDS，再建立 Release build：

```bash
bash projects/autoruntime/scripts/bootstrap_cyclonedds.sh

cmake -S projects/autoruntime \
  -B projects/autoruntime/build-dds-rpc-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUTORUNTIME_ENABLE_DDS=ON \
  -DAUTORUNTIME_BUILD_TESTS=ON \
  -DAUTORUNTIME_BUILD_BENCHMARKS=ON

cmake --build projects/autoruntime/build-dds-rpc-release \
  --target autoruntime_dds_rpc_benchmark
```

运行默认正式 case：

```bash
projects/autoruntime/build-dds-rpc-release/autoruntime_dds_rpc_benchmark \
  --output projects/autoruntime/docs/evidence/dds-rpc-RESULT.jsonl
```

小规模可重复调用：

```bash
projects/autoruntime/build-dds-rpc-release/autoruntime_dds_rpc_benchmark \
  --payload 64 --iterations 40 --warmup 10 --trials 2
```

CTest 的 `autoruntime.dds.rpc.benchmark_smoke` 会验证 self-test 的 50/50 exact count、完整 JSONL、统一 run ID/source revision、分位数顺序，并再验证双 trial 输出。

## 解释边界

- 同进程 participant 仍走真实 DDS writer/reader 和 discovery，但不代表跨主机网络；
- WSL2 的 scheduler、虚拟 CPU 与 host load 会影响尾延迟；
- 单 outstanding latency 与高并发 throughput 是不同实验；
- logical payload throughput 不含 DDS/RTPS/envelope wire overhead；
- peak RSS 是进程生命周期峰值，不是 trial 独占增量；
- 没有独立 core pinning、频率锁定或 kernel real-time tuning；
- 正式 Release 原始数据在实现提交固定后生成；在 evidence 文件和 hash 入库前，该项为 **INCOMPLETE**。
