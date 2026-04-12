# Limited distributed runtime

AutoRuntime's distributed module is deliberately finite. It provides enough
control-plane behavior to run a small autonomous-system graph across hosts
without claiming to be a general cluster manager.

## Scope

The Linux implementation contains two independent paths:

- `DiscoveryService`: UDP heartbeat announcements to an explicit bounded
  peer list, generation-aware membership, and lease expiry.
- `RpcServer` / `RpcClient`: one-request-per-connection TCP RPC with a
  versioned frame, request correlation, bounded method/payload sizes,
  deadlines, cancellation, and typed status responses.

The explicit peer list is a deployment seed list. A node may add peers at
runtime, but both `max_peers` and `max_members` are fixed by configuration.
There is no unbounded gossip state, leader election, consensus, or durable
service registry.

## Discovery state

Each announcement contains:

| Field | Purpose |
| --- | --- |
| magic + version | reject unrelated or incompatible datagrams |
| node id | stable logical identity |
| generation | distinguish a restarted process from an old instance |
| heartbeat sequence | reject duplicate/replayed announcements |
| RPC IPv4 address + port | locate the member's control endpoint |

The receiver timestamps accepted announcements with its own monotonic clock;
clocks are never compared across hosts. A higher generation replaces the
current record. A lower generation is stale and cannot refresh the lease.
A non-increasing sequence in the current generation is a duplicate and also
cannot refresh the lease. Records disappear after `lease_timeout`.

Capacity exhaustion, stale/duplicate packets, parse errors, send failures, and
expired members are all counted in `DiscoveryStats`.

## RPC framing

A request header is 28 bytes:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | `ARRQ` magic |
| 4 | 2 | protocol version |
| 6 | 2 | reserved flags |
| 8 | 8 | request id |
| 16 | 4 | remaining deadline in milliseconds |
| 20 | 4 | method length |
| 24 | 4 | payload length |

A response header is 24 bytes and carries `ARRS`, version, typed
`StatusCode`, the same request id, detail length, and payload length.
Integers use network byte order. Methods are limited to 128 bytes, error
details to 1 KiB, and payloads to the configured bound (with a hard 16 MiB
client ceiling). Invalid bounds or correlation close the connection.

The client uses nonblocking connect/send/receive and polls in short intervals
so a `Deadline` or `std::stop_token` can terminate every I/O phase. The
server is intentionally single-dispatch: this keeps concurrency bounded and
makes overload visible as connection/backlog delay. A production deployment
that needs parallel RPC should add a fixed worker pool and admission metrics,
not detached per-request threads.

## Crash and restart sequence

For a planning-process restart:

1. missed UDP heartbeats cause the old membership lease to expire;
2. `HealthMonitor` independently observes process exit or heartbeat loss;
3. cleanup removes old transport resources;
4. the supervisor starts generation `N + 1`;
5. the new process advertises the same node id with the higher generation;
6. peers replace the old record and reconnect to the advertised RPC endpoint;
7. stale generation `N` packets cannot refresh or overwrite generation
   `N + 1`.

Discovery supplies endpoint membership; `HealthMonitor` owns recovery
policy. Keeping those roles separate avoids embedding process supervision in
the wire protocol.

## Verification

`tests/distributed_test.cpp` uses real UDP and TCP sockets. It verifies:

- generation 1 to 2 replacement and rejection of continuing generation 1
  heartbeats;
- bounded membership and an observable capacity drop;
- lease expiry;
- discovery between a parent and a forked planning process;
- RPC endpoint propagation and a cross-process echo round trip;
- `SIGKILL` followed by membership expiry;
- RPC timeout, pre-request cancellation, and missing-method status.

The test is enabled on Unix and has a 20-second CTest timeout. Its normal run
takes approximately half a second on the recorded WSL2 environment.

## Security and deployment limits

The protocol currently has no authentication, encryption, authorization, or
anti-spoofing beyond generation/sequence checks. It must be restricted to a
trusted network namespace or protected network segment. Do not expose these
ports to an untrusted network. TLS or mutually authenticated transport,
identity provisioning, rate limits, and source allow-lists are required
before hostile-network deployment.

Only numeric IPv4 addresses are accepted. NAT traversal, IPv6, multicast
auto-join, routing across arbitrary subnets, persistence, and split-brain
resolution are intentionally out of scope.
