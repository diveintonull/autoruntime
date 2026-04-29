# 成员管理、心跳与租约设计

## 目标与非目标

这个控制面只解决一个有限问题：在显式配置的小规模 peer 集合中，用 UDP heartbeat 维护当前可见成员，并在成员沉默后让 lease 到期。成员重启必须携带更高 generation，使接收方能拒绝乱序或延迟到达的旧实例数据。

它不实现 Raft、Paxos、leader election、quorum、持久服务注册或 single-active coordinator。当前架构没有多个 coordinator 争夺同一资源的需求，因此加入共识只会扩大故障面，不能改善现有 Sensor、Planning、Control、Monitor 拓扑。

## 本轮修复的问题

旧实现只在 active member 存在时比较 generation：

```text
ACTIVE(g=2) --lease 到期--> ABSENT
ABSENT + 延迟 heartbeat(g=1) --> ACTIVE(g=1)  // 错误复活
```

一旦 lease 到期并删除 record，接收方就忘记曾见过的最高 generation。仍在发送或被网络延迟的旧 heartbeat 可以重新建立旧实例。`tests/distributed_test.cpp` 的 RED 测试用真实 UDP 稳定复现了该路径。

本轮在 active map 之外增加有界 generation fence。成员 lease 到期时，接收方短期保留该 node id 的最高 generation；旧或相同 generation 不能在 fence window 内复活，更高 generation 可立即加入。

## 状态机

```text
ABSENT
  | valid heartbeat(g, seq)
  v
ACTIVE(g, seq, last_seen)
  | same g and increasing seq    -> refresh last_seen
  | higher g                     -> replace active incarnation
  | lower g / non-increasing seq -> reject, do not refresh lease
  | lease timeout
  v
FENCED(g, expires_at)
  | generation <= g            -> reject, do not extend fence
  | generation > g             -> ACTIVE(new generation)
  | fence timeout              -> ABSENT
  | bounded-capacity eviction  -> ABSENT
```

关键不变量：

1. active lease 只由通过 generation/sequence 检查的 heartbeat 刷新；duplicate、replay 和 stale packet 不刷新。
2. lease 与 fence 都使用接收方 `steady_clock`。wire 中不传 wall clock，也不比较不同 host 的时钟。
3. active record 到期时先写入 fence，再从 active map 删除；观察者不会出现“忘记 generation 后立即接受旧包”的窗口。
4. fence 拒绝不延长 `expires_at`，攻击者或失控旧进程不能靠持续发送永久占住状态。
5. 更高 generation 是唯一能在 fence window 内重新加入同一 node id 的方式。

## 有界资源

`DiscoveryConfig` 明确配置三类上界：

| 配置 | 含义 |
| --- | --- |
| `max_peers` | 本节点发送 heartbeat 的目标数 |
| `max_members` | 同时 active 的远端成员数 |
| `max_generation_fences` | lease 到期后保留的 generation fence 数 |

`generation_fence_timeout` 必须不小于 `lease_timeout`。fence map 满时淘汰 `expires_at` 最早的记录，避免控制面状态无限增长。`generation_fences_created`、`generation_fences_expired`、`generation_fence_evictions` 与 `generation_fence_rejections` 都可从 `DiscoveryStats` 观察。

最早到期淘汰需要在插入时扫描最多 `max_generation_fences` 个条目，复杂度为 O(N)。这里的 N 是部署配置的小常数，且只发生在 lease 到期控制路径；为它引入 heap 和第二份索引会增加一致性负担。

## 所有权与线程模型

`DiscoveryService` 拥有 UDP descriptor、一条 worker `jthread`、peer vector、active member map、generation fence map 和统计计数。

- worker 串行执行 send、receive、active expiry 与 fence expiry；
- public `AddPeer`、`Members`、`Find`、`Stats` 与 worker 共享一个 mutex；
- `MemberRecord::last_seen` 和 fence 的 `expires_at` 都只在接收进程中产生；
- `Stop` 请求 `jthread` 停止并 join，析构后才关闭 socket。

因此没有跨线程无锁 membership protocol，也没有跨 host clock ownership。

## 故障解释

Heartbeat/lease 是 failure suspicion，不是死亡证明：

- heartbeat 可能因调度抖动、UDP 丢包、网络分区或进程退出而缺失；
- lease timeout 只能说明在本地单调时间窗口内没有收到可接受 heartbeat；
- RPC timeout 同样不能证明 peer 已死；
- 网络恢复后，更高 generation 可立即恢复；相同 generation 是否继续使用取决于部署是否保证实例唯一。

`HealthMonitor` 仍负责 process health、no-progress、backlog、deadline 和 recovery hook。Discovery 只负责 endpoint membership，两者不能合并成一个“万能故障检测器”。

## 代际编号的部署契约

generation 必须由 deployment/supervisor 在同一 logical node 每次重启时严格增加。如果两个并发实例错误地使用相同 node id 与 generation，接收方只能看到同一 sequence 空间，不能可靠判断哪一个是合法 owner。

当前 fence 不持久化。超过 `generation_fence_timeout` 或因容量被淘汰后，接收方会再次进入 `ABSENT`，旧 generation 仍可能被接受。需要跨长时间或 observer restart 保持 fencing 的生产系统，应引入持久 incarnation authority 或经过认证的部署 epoch，而不是把内存 tombstone 描述成永久安全保证。

## 基准测试方法

`autoruntime_membership_lease_benchmark` 使用真实 IPv4 loopback UDP，并在每轮保留一个低 generation sender：

1. 当前 generation 已被发现；
2. 启动旧 generation sender并确认 stale packet 被观察；
3. 停止当前实例，测量 `Stop` 返回到 active member 消失的单调延迟；
4. 继续让旧 sender 发送，确认它不能复活；
5. 启动更高 generation，测量启动到重新发现的单调延迟；
6. 重复多轮。

输出包含 lease detection 与 restart discovery 的 P50/P95/P99/P99.9/MAX，以及进程 CPU、上下文切换和 peak RSS。它衡量的是本机 loopback 控制面路径，不代表真实交换机、跨主机时钟、丢包网络或生产硬件。

正式结果必须固定到实现提交后重新生成；未提交工作树上的开发 smoke 不写入性能结论。

## 验证

`tests/distributed_test.cpp` 覆盖：

- generation 2 到期后，持续发送的 generation 1 在 fence window 内不能复活；
- generation 3 可立即越过 generation 2 fence；
- fence capacity 为 1 时第二个过期成员产生可观察 eviction；
- 剩余 fence 按单调时钟到期；
- fence timeout 小于 lease 或 capacity 为 0 时配置被拒绝；
- 既有 generation replacement、bounded membership、fork/SIGKILL、lease expiry 与 RPC failure 测试继续通过。

实现后的完整 case 已连续运行 10 次通过；正式 sanitizer 矩阵和固定 revision benchmark 证据在实现提交后生成。

## 明确限制

- 无 authentication、authorization、encryption 或 anti-spoofing；只适合可信实验网络。
- 无持久 generation、durable registry、gossip、consensus 或 split-brain resolution。
- UDP heartbeat 没有可靠送达；lease 参数必须留出 scheduler/network jitter margin。
- generation fence 是有限时间、有限容量的乱序保护，不是拜占庭安全机制。
- 当前只有 numeric IPv4 与 explicit peer，不支持 NAT traversal、DNS、IPv6 或自动拓扑发现。
