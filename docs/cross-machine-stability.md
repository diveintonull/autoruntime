# 跨机稳定性与故障恢复

## 目标与边界

本切片验证 AutoRuntime 的 DDS pub/sub、UDP discovery 与 TCP RPC 能否在两个独立 Linux 网络栈之间工作，并在延迟、断链、peer 消失和重启后恢复。它不是 loopback 双进程测试，也不把同一台主机伪装成两台物理机器。

当前可复现实验环境是 WSL2 Ubuntu 24.04.4 上的两个 rootless network namespace：

| 逻辑机器 | 地址 | 角色 |
| --- | --- | --- |
| Machine A | 10.88.0.2 | Sensor DDS publisher、Planning RPC server、discovery |
| Machine B | 10.88.0.3 | Control DDS subscriber、Planning RPC client、Monitor RPC server、discovery 与 health tracker |

外层 unshare -Urn 创建一次性 user/network namespace，内部再创建两个 child network namespace、Linux bridge 与两对 veth。所有网络对象在外层 namespace 退出时消失；脚本只终止自己记录的 PID。数据面直接使用 DDS/UDP/TCP，shell 只负责拓扑和故障编排。

真实双物理机、跨交换机、WAN、NAT 和 hostile network 仍为未完成范围。

## 数据流

    Machine A / 10.88.0.2                  Machine B / 10.88.0.3
    Sensor -> DDS BestEffort  ------------> Control subscriber
    Planning RPC server       <------------ Planning RPC probe
    UDP discovery             <-----------> UDP discovery
                                                |
                                                +-> health/metrics JSONL

Sensor payload 固定为 sequence 与按位取反值，DDS envelope 同时携带 sequence 和 source generation。Machine B 逐条校验 payload，按 generation 跟踪 DDS 序号缺口与重复；Planning RPC 回显同一 payload，client 检查 deadline、typed status 和完整响应。

## 故障序列

每个循环严格按下列顺序执行：

1. warm-up，等待 DDS discovery、UDP membership 与 RPC 正常；
2. 在 A、B 两端 veth 注入 20 ms netem delay；
3. 关闭 bridge 上 A 端口，形成双向 disconnect；
4. 恢复 bridge 端口并观察 discovery/RPC/DDS 恢复；
5. SIGKILL Machine A，使 DDS participant、Planning RPC 与 heartbeat 同时消失；
6. 等待 lease expiry 与 DDS stall；
7. 以相同 logical node id、更高 generation 重启 Machine A；
8. 最终 warm-up，要求 Machine B 回到 RUNNING。

短 smoke 至少执行一个完整循环；长时测试重复循环直到达到 minimum-duration-ms。每次 A 重启都写入独立 JSONL，避免覆盖 crash 前证据。

持续时间门使用 /proc/uptime 的单调毫秒值，不使用可能被宿主校时调整的 wall clock。orchestrator summary 同时写 requested_fault_duration_ms 与 actual_fault_duration_ms，并把实际值小于请求值视为失败；完整故障循环可能让实际值略微超出请求值。

## 健康状态与通过条件

NetworkHealthState 是跨机观测状态，不替代进程内 HealthMonitor：

- STARTING：尚无足够信号；
- RUNNING：discovery 可见、RPC 成功、DDS 最近有消息；
- DEGRADED：peer 仍可发现，但 RPC 不可用或 DDS stall；
- FAILED：membership 不可见。

FAILED 到 RUNNING 计为一次 recovery。require-faults 模式必须同时观察到：

- discovery loss 与 recovery；
- peer generation 变化；
- RPC timeout 或 transport error；
- DDS stall 与 DDS message loss；
- health transition 与 FAILED -> RUNNING recovery；
- 最终状态 RUNNING；
- RPC unexpected/parse error 与 payload error 均为 0。

JSONL 还记录 observation count、DDS message/loss/duplicate、RPC 分类、bounded latency samples、baseline/final P99 与 drift。latency baseline 和尾窗口各最多保留 latency-window 个样本；DDS 序号状态为常数空间，因此 soak 时不会保存无界请求历史。

## 构建与运行

先准备固定 Cyclone DDS 11.0.1，再构建：

    bash projects/autoruntime/scripts/bootstrap_cyclonedds.sh
    cmake -S projects/autoruntime \
      -B projects/autoruntime/build-cross-machine -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DAUTORUNTIME_ENABLE_DDS=ON
    cmake --build projects/autoruntime/build-cross-machine \
      --target autoruntime_cross_machine_stability

自动 smoke：

    ctest --test-dir projects/autoruntime/build-cross-machine \
      -R autoruntime.cross_machine --output-on-failure

30 分钟阶段：

    bash projects/autoruntime/scripts/run_cross_machine_stability.sh \
      --binary projects/autoruntime/build-cross-machine/autoruntime_cross_machine_stability \
      --output-dir projects/autoruntime/docs/evidence/cross-machine-30m \
      --minimum-duration-ms 1800000 \
      --warmup-ms 2000 --phase-ms 700 --domain-id 73

2 小时或 overnight 只需提高 minimum-duration-ms，并使用新的 output-dir；不得复用短跑结果宣称长时通过。

## 分阶段证据状态

| 阶段 | 当前状态 | 说明 |
| --- | --- | --- |
| 自动 namespace smoke | PASS | Debug 开发构建已完成真实 delay/disconnect/reconnect/crash/restart；正式矩阵结果在 testing.md 更新 |
| 30 分钟 | **INCOMPLETE** | 等待固定 implementation commit 后正式运行 |
| 2 小时 | **INCOMPLETE** | 尚未运行 |
| Overnight | **INCOMPLETE** | 尚未运行 |
| 双物理机 | **INCOMPLETE** | 当前环境只有单 host 双隔离网络栈 |

报告只把实际运行过的阶段标为 PASS。任何 sanitizer、解析、负载、最终 health 或 runner exit 失败都保留原始证据并标为失败，不通过重跑覆盖。

## 解释限制

- DDS 使用 BestEffort/KeepLast 32；断链期间 message loss 是预期观测，不等于 correctness error。
- RPC timeout 与 transport error 取决于断链发生在 connect、send 或 receive 的哪一阶段，二者都属于预期 availability failure。
- namespace 共享同一 kernel、CPU、内存与时钟，不能代表物理 NIC、交换机、MTU、clock skew 或跨机资源竞争。
- Cyclone DDS 使用明确 unicast peer 和接口地址，未验证 multicast discovery。
- discovery/RPC 当前无认证、加密与授权，只能用于可信隔离网络。
- P99 drift 是 bounded 首窗与尾窗的观测差，不是统计置信区间或硬实时保证。
