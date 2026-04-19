# 故障注入矩阵

日期：2026-08-20

## 复现

```bash
projects/autoruntime/scripts/run_test_matrix.sh release
ctest --test-dir projects/autoruntime/build-verify-release \
  -L fault --output-on-failure
```

记录的 Release fault run（[日志](evidence/fault-matrix-release.log)）通过全部 20 个独立命名 case；完整 fault label 也用 `--repeat until-fail:5` 连续运行五次。

## AutoRuntime case

| CTest suffix | 注入条件 | 必须观察到的结果 |
| --- | --- | --- |
| `transport_closed` | shutdown 后 publish | typed `Closed`；failure count 增加 |
| `queue_overflow` | hold worker 并超过 task capacity | typed `QueueFull`；overflow 增加 |
| `slow_callback_isolation` | planning 阻塞 300 ms | 独立 control group 在 50 ms 内完成 |
| `deadline_miss` | 10 ms callback、1 ms deadline | completed sample 记录 miss |
| `cancelled_task` | event release 前 cancel | 后续 notify 返回 `Cancelled` |
| `heartbeat_loss` | 推进时间超过 heartbeat lease | `FAILED/HeartbeatLost` |
| `no_progress` | heartbeat 前进、progress 停滞 | `FAILED/NoProgress` |
| `backlog_degraded` | backlog 超出 bound | `DEGRADED/BacklogExceeded` |
| `process_exit` | process probe 变为 dead | `FAILED/ProcessExited` |
| `stale_generation` | generation 1 update 指向 generation 2 | `StaleGeneration`；state 不变 |
| `rpc_disconnect` | 调用 closed TCP endpoint | deadline 内得到 `TransportError` |
| `rpc_corrupt_frame` | 28 B request header 全零 | malformed count 增加 |
| `duplicate_membership` | duplicate id/generation/sequence | 只保留一个 member；duplicate count 增加 |
| `service_timeout` | handler 超过 client deadline | typed `Timeout` |
| `message_delay` | test transport 延迟 40 ms | 至少 30 ms 后成功 delivery |
| `message_drop` | test transport 丢一次 publish | 无 delivery；drop counter 增加 |
| `slow_consumer` | depth 2 时 hold subscriber | bounded watermark 与可见 drop |
| `node_restart` | generation 3 失败后运行 hook | cleanup/start/reconnect；generation 4 |
| `shutdown_during_load` | 1 个 active callback、4 个 queued | 观察 stop，worker join，拒绝新 work |
| `dds_participant_loss` | 关闭真实 participant 后 publish | typed `Closed`；failure count 增加 |

DDS case 只在 `AUTORUNTIME_ENABLE_DDS=ON` 时注册；其余 19 个在轻依赖 Unix build 运行。deterministic delay/drop wrapper 只用于测试。

## 覆盖的故障族

- crash/restart：`process_exit`、`node_restart` 与真实 SIGKILL test；
- heartbeat/progress：`heartbeat_loss`、`no_progress`；
- slow callback/consumer：`slow_callback_isolation`、`slow_consumer`；
- delay/drop/overflow：`message_delay`、`message_drop`、`queue_overflow`；
- disconnect/peer loss：`transport_closed`、`rpc_disconnect`、`dds_participant_loss`；
- corruption：`rpc_corrupt_frame`；
- shutdown load：`shutdown_during_load`；
- stale identity：`stale_generation`、`duplicate_membership`；
- deadline：`deadline_miss`、`service_timeout`。

## 真实 process 与 recovery test

- `autoruntime.fastipc_transport` 把 `before-crash` 传给 generation 7 的 exec planning process，发送 `SIGKILL`，观察 `FAILED/ProcessExited`，执行 cleanup/start/reconnect，启动 generation 8，传输并校验 `after-restart`，最后证明 heartbeat/progress 让 health 回到 `RUNNING`。Release 连续十次通过。
- `autoruntime.distributed` 通过 UDP 发现 fork process，执行 TCP RPC，杀死 child，等待 lease expiry，再验证 membership removal。
- `autoruntime.health_monitor` 用 deterministic probe 覆盖 state ordering、stale update、budget、hook exception 与 deadline。

ASan 发现的 shutdown race 与 operation-lifetime fix 见 [recovery.md](recovery.md)。

## FastIPC substrate

可独立构建的 FastIPC 提供 12 个命名 shared-memory fault：missing peer、producer crash、consumer crash、restart、slow consumer、timeout、malformed header、version mismatch、stale memory、full queue、empty queue、rapid restart。AutoRuntime 负责 policy/adapter behavior；FastIPC 负责 layout、liveness、reclaim 与 close/unmap invariant。

## 局限

这些测试证明 typed outcome 与 state transition，不证明 hard real-time 或 Byzantine guarantee。timing assertion 使用宽裕 margin。delay/drop shim 不模拟 reordering 或 bandwidth。RPC corruption 只覆盖聚焦的 framing case，不等于 fuzzing。target-hardware soak、physical network impairment、DDS status storm、authenticated hostile-peer test 仍属于生产化工作。
