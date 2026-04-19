# 有限分布式运行时

AutoRuntime 的 distributed module 有意限制范围。它只提供让小型 autonomous-system graph 跨 host 运行所需的 control-plane behavior，不声称是通用 cluster manager。

## 范围

Linux 实现有两条独立路径：

- `DiscoveryService`：向显式 bounded peer list 发送 UDP heartbeat announcement，维护 generation-aware membership 和 lease expiry。
- `RpcServer` / `RpcClient`：one-request-per-connection TCP RPC，带 versioned frame、request correlation、bounded method/payload size、deadline、cancellation 与 typed status response。

explicit peer list 是 deployment seed list。node 可在运行时加入 peer，但 `max_peers` 与 `max_members` 都由配置固定。没有 unbounded gossip state、leader election、consensus 或 durable service registry。

## Discovery 状态

每个 announcement 包含：

| 字段 | 用途 |
| --- | --- |
| magic + version | 拒绝无关或不兼容 datagram |
| node id | 稳定 logical identity |
| generation | 区分重启后的 process 与旧 instance |
| heartbeat sequence | 拒绝 duplicate/replayed announcement |
| RPC IPv4 address + port | 定位 member control endpoint |

receiver 用自己的 monotonic clock 为已接受 announcement 加 timestamp，绝不跨 host 比较 clock。更高 generation 替换 current record；更低 generation 属于 stale，不能刷新 lease。同 generation 下 non-increasing sequence 属于 duplicate，也不能刷新 lease。record 在 `lease_timeout` 后移除。

capacity exhaustion、stale/duplicate packet、parse error、send failure 和 expired member 都计入 `DiscoveryStats`。

## RPC framing

request header 为 28 B：

| Offset | 宽度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | `ARRQ` magic |
| 4 | 2 | protocol version |
| 6 | 2 | reserved flags |
| 8 | 8 | request id |
| 16 | 4 | 剩余 deadline，单位 ms |
| 20 | 4 | method length |
| 24 | 4 | payload length |

response header 为 24 B，携带 `ARRS`、version、typed `StatusCode`、相同 request id、detail length 与 payload length。整数使用 network byte order。method 最大 128 B、error detail 最大 1 KiB；payload 受配置约束，并有 hard 16 MiB client ceiling。bound 或 correlation 非法时关闭 connection。

client 使用 nonblocking connect/send/receive，并以短间隔 poll，让 `Deadline` 或 `std::stop_token` 能终止每个 I/O phase。server 有意 single-dispatch，以保持 bounded concurrency 并让 overload 显现为 connection/backlog delay。若生产部署需要并行 RPC，应加入 fixed worker pool 与 admission metric，而不是 detached per-request thread。

## Crash 与 restart sequence

以 planning-process restart 为例：

1. missed UDP heartbeat 让旧 membership lease 到期；
2. `HealthMonitor` 独立观察 process exit 或 heartbeat loss；
3. cleanup 移除旧 transport resource；
4. supervisor 启动 generation `N + 1`；
5. 新 process 用相同 node id、更高 generation 广播；
6. peer 替换旧 record，并重连 advertised RPC endpoint；
7. stale generation `N` packet 不能刷新或覆盖 generation `N + 1`。

Discovery 只提供 endpoint membership；`HealthMonitor` 拥有 recovery policy。分离二者避免把 process supervision 塞进 wire protocol。

## 验证

`tests/distributed_test.cpp` 使用真实 UDP/TCP socket，验证：

- generation 1 -> 2 replacement，并拒绝继续到来的 generation 1 heartbeat；
- bounded membership 与可观察 capacity drop；
- lease expiry；
- parent 与 fork 出的 planning process 之间 discovery；
- RPC endpoint propagation 与 cross-process echo round trip；
- `SIGKILL` 后 membership expiry；
- RPC timeout、pre-request cancellation、missing-method status。

该测试只在 Unix 启用，CTest timeout 为 20 s；记录的 WSL2 环境中正常约 0.5 s 完成。

## 安全与部署边界

协议当前没有 authentication、encryption、authorization，除 generation/sequence check 外也没有 anti-spoofing。必须限制在 trusted network namespace 或受保护 network segment，不能把端口暴露给不可信网络。hostile-network deployment 前必须加入 TLS 或 mutual authentication、identity provisioning、rate limit 与 source allow-list。

目前只接受 numeric IPv4 address。NAT traversal、IPv6、multicast auto-join、任意 subnet routing、persistence 与 split-brain resolution 均不在范围内。
