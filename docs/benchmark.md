# AutoRuntime 对比基准

## 目标

这个实验回答两个问题：

1. 同一台机器、同一 request/echo 拓扑下，AutoRuntime + FastIPC copy、AutoRuntime + FastIPC loan 与 `rclcpp + DDS` 的端到端成本分别是什么；
2. AutoRuntime DDS adapter 与 `rclcpp + DDS` 在相同消息负载下表现如何。

当前 runner 只产生同机双进程证据。真正跨主机或独立 network namespace 的 DDS 对比属于第 9 项 Cross-machine Stability，在完成独立 initiator/responder role 前明确为 **INCOMPLETE**。同机 DDS 结果不能改名为 network result。

基准不证明某个 middleware 普遍更快。结果同时包含 runtime API、消息表示、DDS/RMW 版本、容器用户态、调度噪声与机器状态，必须结合环境记录解释。

## 被测路径

| mode | 进程 | 数据路径 | 复制语义 |
| --- | --- | --- | --- |
| `autoruntime_fastipc_copy` | AutoRuntime initiator + responder | FastIPC shared memory | runtime payload -> staging wire -> shared chunk；接收再复制到 Message |
| `autoruntime_fastipc_loan` | AutoRuntime initiator + responder | FastIPC shared memory | 应用直接写 chunk，callback-scoped sample 直接读 chunk |
| `autoruntime_dds` | AutoRuntime initiator + responder | Cyclone DDS | bounded IDL sequence，DDS 序列化路径 |
| `rclcpp_dds` | rclcpp initiator + responder | ROS 2 RMW + DDS | `ByteMultiArray` 与 ROS 2 publish/take 路径 |

四种 mode 的 `comparison_group` 都是 `same_host_two_process_request_echo`。不要用不同 group 隐藏同一实验中的不利结果；具体实现由 `mode` 字段区分。

## 公平性契约

| 维度 | 固定值 |
| --- | --- |
| 硬件 | 同一 WSL2 host；environment record 捕获 CPU、kernel、内存与 hostname |
| 拓扑 | 两个真实进程；initiator 定时发送，responder 校验并 echo |
| QoS | Reliable、KeepLast、depth 64 |
| 默认 payload | 64 B、1 KiB、64 KiB、1 MiB |
| 联合大载荷 | 1 MiB、4 MiB |
| 默认频率 | 100 Hz |
| 默认测量窗口 | 每 case 3000 ms |
| warmup | 每 case 20 次，不计入结果 |
| 重复 | 每 payload/mode 3 个 trial |
| payload 工作 | producer 完整写，responder 完整校验并生成 response，initiator 完整校验 |
| 时钟 | initiator `steady_clock` RTT；不比较跨进程 wall clock |
| 发布节拍 | open-loop absolute release；不使用 response 到达时间安排下一次发送 |

FastIPC pool depth 也设为 64，与 QoS depth 对齐。4 MiB case 不进入默认 payload 列表，必须显式运行；这避免日常 smoke test 无条件创建大 shared-memory pool。
rclcpp runner 每个 case 都让 responder 在 `fork` 后立即 `exec /proc/self/exe`，并只在新地址空间初始化 ROS context。父进程曾经运行过 DDS thread 后，不能让下一次 fork child 直接继续调用 RMW；连续三 case 的 CTest 专门防止这个生命周期回归。


## 为什么是 full-touch

payload 前 8 byte 编码 sequence，其余字节由确定性 pattern 填满。接收端逐字节验证完整 payload，再生成同尺寸 response；initiator 再逐字节验证。

因此 1 MiB/4 MiB 的结果包含真实内存写、读和验证成本。它不会把一个共享 offset 当作“大消息”然后宣称不现实的带宽。字段 `logical_payload_bytes` 按成功 round-trip 的 request + response payload 计算，不把 header、DDS discovery 或协议开销伪装成业务带宽。

## 调度与计数

每个 trial 在独立的固定发送窗口执行：

```text
absolute release n
  -> 生成并完整写 payload
  -> publish request
  -> responder 完整校验
  -> publish response
  -> initiator 完整校验
  -> 记录 RTT
```

warmup sequence 使用独立高位 namespace。进入测量边界时父子两进程都清空 warmup counter；迟到 warmup response 仍按 namespace 排除，不能污染 `responder_requests` 或 `unexpected_responses`。

成功结果必须同时满足：

- `scheduled_requests == published_requests == completed_round_trips == responder_requests`；
- publish failure、lost response、payload mismatch、unexpected response 全为 0；
- latency sample 数等于 completed round trip；
- result 数等于 mode × payload × trial 的笛卡尔积。

只要一个 case 不满足，进程退出码就是 1，JSON `status` 为 `failed`。setup/参数错误退出码为 2。不能只截取其中一个成功 trial。

## 指标定义

- `latency_us.p50/p95/p99/p99_9/max`：每个 trial 内排序后的 nearest-rank RTT；
- `completed_round_trips_per_second`：成功 RTT 数除以配置的测量时长；
- `logical_payload_mib_per_second`：成功 request + response 的业务 payload 除以配置时长；
- `user_cpu_ms/system_cpu_ms`：initiator 与 responder 的 `getrusage` 增量之和；
- `cpu_utilization_percent`：两进程 CPU time / 实际测量加 drain wall time，可超过 100%；
- context switch：两个进程 voluntary/involuntary 增量之和；
- RSS：分别记录 initiator 与 responder peak RSS，不相加后冒充单进程 footprint。

P99.9 在 300 个样本的默认单 trial 中接近尾部最大值，统计置信度有限；报告必须同时展示三个 trial 和 MAX，不把 P99.9 当成稳定 SLO 估计。

## JSONL schema

每个文件第一行是 `record_type=environment`，其后是 `record_type=result`。共同键包括：

- `schema_version`、`benchmark`、`run_id`；
- build type、compiler、source revision、kernel 与机器；
- topology、QoS、clock、payload validation、频率和持续时间；
- exact success/failure counters；
- latency、throughput、CPU、context switch 与 RSS。

rclcpp 环境另外记录：

- `rmw_implementation`：`rmw_get_implementation_identifier()` 的真实返回值；
- `requested_rmw_implementation`：环境变量请求值；
- `ros_domain_id`。

不能只检查 `RMW_IMPLEMENTATION` 环境变量；插件加载失败或 fallback 时，实际实现可能不同。

## 构建 AutoRuntime runner

```bash
cmake -S projects/autoruntime \
  -B projects/autoruntime/build-comparative-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUTORUNTIME_ENABLE_DDS=ON \
  -DAUTORUNTIME_BUILD_TESTS=ON
cmake --build projects/autoruntime/build-comparative-release \
  --target autoruntime_comparative_benchmark
```

正式同机四路径套件中的 AutoRuntime 部分：

```bash
projects/autoruntime/build-comparative-release/autoruntime_comparative_benchmark \
  --mode fastipc-copy \
  --mode fastipc-loan \
  --mode dds \
  --payload 64 \
  --payload 1024 \
  --payload 65536 \
  --payload 1048576 \
  --frequency 100 \
  --duration-ms 3000 \
  --warmup 20 \
  --trials 3 \
  --domain 187 \
  --run-id <同一运行编号> \
  --output autoruntime.jsonl
```

联合 4 MiB case：

```bash
projects/autoruntime/build-comparative-release/autoruntime_comparative_benchmark \
  --mode fastipc-copy --mode fastipc-loan --mode dds \
  --payload 4194304 \
  --frequency 100 --duration-ms 3000 --warmup 20 --trials 3 \
  --domain 188 --run-id <同一运行编号> \
  --output autoruntime-4m.jsonl
```

## 构建 rclcpp + Cyclone DDS 基线

`benchmarks/rclcpp_baseline/Dockerfile` 固定官方 `ros:jazzy-ros-base` 镜像 digest，并安装 Jazzy 的 `rmw_cyclonedds_cpp`。镜像只在本机构建：

```powershell
docker build `
  -t autoruntime-rclcpp-baseline:local `
  projects/autoruntime/benchmarks/rclcpp_baseline
```

构建 package 时注入被测 AutoRuntime commit：

```bash
source /opt/ros/jazzy/setup.bash
colcon build --base-paths /ws/src/rclcpp_baseline \
  --cmake-args \
  -DCMAKE_BUILD_TYPE=Release \
  -DRCLCPP_BASELINE_SOURCE_REVISION=<commit>
source /ws/install/setup.bash
```

运行时必须显式设置并核对实际 RMW：

```bash
RMW_IMPLEMENTATION=rmw_cyclonedds_cpp \
ROS_DOMAIN_ID=187 \
/ws/install/autoruntime_rclcpp_baseline/lib/autoruntime_rclcpp_baseline/rclcpp_comparative_benchmark \
  --payload 64 --payload 1024 --payload 65536 --payload 1048576 \
  --frequency 100 --duration-ms 3000 --warmup 20 --trials 3 \
  --run-id <同一运行编号> \
  --output rclcpp.jsonl
```

## 当前环境差异与解释边界

本地 AutoRuntime 固定 Cyclone DDS 11.0.1；Jazzy binary package 当前提供 Cyclone DDS 0.10.5 与 `rmw_cyclonedds_cpp` 2.2.3。两条 DDS 路径使用同一 DDS 家族，但不是同一版本，也不是同一 public API。

rclcpp baseline 在 Docker 用户态运行，AutoRuntime runner 在 WSL2 用户态运行；二者共享同一 WSL2 kernel 与硬件，但容器文件系统、ROS 2 executor、message type 和 allocator 都是混杂变量。报告可以比较“完整路径”，不能把差值全部归因于 AutoRuntime 或 zero-copy。

若未来需要隔离具体因素，应增加：

1. 同一 Cyclone DDS 版本下 direct DDS C API 与 rclcpp 的分层实验；
2. 固定 allocator 与预分配策略；
3. CPU pinning、idle/stress 两组机器状态；
4. 独立 transport-only 与 full-touch workload。

这些额外实验未完成前，不声称框架自身带来某个百分比提升。

## 结果发布规则

README 或简历只能引用同时满足以下条件的数据：

- Release binary 的 `source_revision` 等于已提交实现；
- environment 与所有 result 原始 JSONL 均已保存并有 SHA-256；
- 四个默认 payload、全部 mode、三个 trial 都存在；
- 4 MiB 联合实验单独完整保存；
- 无 failed/missing trial；
- 报告列出机器、容器、DDS/RMW 版本和已知混杂变量；
- 结论同时展示不利结果，不只挑最快 payload。

smoke test 数字只证明 runner 可用，不进入性能结论。Debug、sanitizer 或运行中被其他负载干扰的数据也不与正式 Release 结果混用。

## 当前完成边界

- 同机 AutoRuntime FastIPC copy/loan 与 DDS 双进程 runner：已实现；
- rclcpp + Cyclone DDS 双进程 runner：已实现；
- 64 B、1 KiB、64 KiB、1 MiB full-touch：已实现；
- 4 MiB AutoRuntime 联合实验：已实现 runner 支持，正式结果随固定提交证据记录；
- 跨主机 AutoRuntime DDS vs rclcpp DDS：**INCOMPLETE**，由第 9 项完成；
- iceoryx/rclcpp loaned-message baseline：不在本实验范围，不得用缺失数据填零。
