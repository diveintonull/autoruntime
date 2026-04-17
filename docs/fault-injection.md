# Fault-injection matrix

Date: 2026-08-20

## Reproduction

```bash
projects/autoruntime/scripts/run_test_matrix.sh release
ctest --test-dir projects/autoruntime/build-verify-release \
  -L fault --output-on-failure
```

The captured Release fault run
([log](evidence/fault-matrix-release.log)) passed all 20 separately named
cases. The complete fault label was also repeated five times with
`--repeat until-fail:5`.

## AutoRuntime cases

| CTest suffix | Injected condition | Required observation |
| --- | --- | --- |
| `transport_closed` | publish after shutdown | typed `Closed`; failure count increments |
| `queue_overflow` | hold worker and exceed task capacity | typed `QueueFull`; overflow increments |
| `slow_callback_isolation` | planning blocks 300 ms | separate control group completes within 50 ms |
| `deadline_miss` | 10 ms callback, 1 ms deadline | completed sample records miss |
| `cancelled_task` | cancel before event release | later notify returns `Cancelled` |
| `heartbeat_loss` | advance past heartbeat lease | `FAILED/HeartbeatLost` |
| `no_progress` | heartbeat advances, progress stalls | `FAILED/NoProgress` |
| `backlog_degraded` | backlog exceeds bound | `DEGRADED/BacklogExceeded` |
| `process_exit` | process probe changes to dead | `FAILED/ProcessExited` |
| `stale_generation` | generation 1 update targets generation 2 | `StaleGeneration`; state unchanged |
| `rpc_disconnect` | call a closed TCP endpoint | `TransportError` within deadline |
| `rpc_corrupt_frame` | zeroed 28-byte request header | malformed count increments |
| `duplicate_membership` | duplicate id/generation/sequence | one member; duplicate count increments |
| `service_timeout` | handler exceeds client deadline | typed `Timeout` |
| `message_delay` | test transport delays 40 ms | delivery succeeds after at least 30 ms |
| `message_drop` | test transport drops one publish | no delivery; drop counter increments |
| `slow_consumer` | hold subscriber at depth two | bounded watermark and visible drops |
| `node_restart` | fail generation 3, run hooks | cleanup/start/reconnect; generation 4 |
| `shutdown_during_load` | one active callback, four queued | stop observed, workers join, work rejected |
| `dds_participant_loss` | close real participant, publish | typed `Closed`; failure count increments |

The DDS case is registered only with `AUTORUNTIME_ENABLE_DDS=ON`; the other
19 run in the dependency-light Unix build. The deterministic delay/drop wrapper
is test-only.

## Requested fault-family coverage

- crash/restart: `process_exit`, `node_restart`, and the real SIGKILL test;
- heartbeat/progress: `heartbeat_loss`, `no_progress`;
- slow callback/consumer: `slow_callback_isolation`, `slow_consumer`;
- delay/drop/overflow: `message_delay`, `message_drop`,
  `queue_overflow`;
- disconnect/peer loss: `transport_closed`, `rpc_disconnect`,
  `dds_participant_loss`;
- corruption: `rpc_corrupt_frame`;
- shutdown load: `shutdown_during_load`;
- stale identity: `stale_generation`, `duplicate_membership`;
- deadline: `deadline_miss`, `service_timeout`.

## Real process and recovery tests

- `autoruntime.fastipc_transport` transfers `before-crash` to an exec'd
  planning process at generation 7, sends `SIGKILL`, observes
  `FAILED/ProcessExited`, runs cleanup/start/reconnect, starts generation 8,
  transfers and verifies `after-restart`, then proves heartbeat/progress
  return health to `RUNNING`. It passed ten repeated Release iterations.
- `autoruntime.distributed` discovers a forked process over UDP, performs TCP
  RPC, kills the child, waits for lease expiry, and verifies membership removal.
- `autoruntime.health_monitor` covers state ordering, stale updates, budgets,
  hook exceptions, and deadlines with deterministic probes.

See [recovery.md](recovery.md) for the ASan-discovered shutdown race and its
operation-lifetime fix.

## FastIPC substrate

The separately buildable FastIPC project contributes 12 named shared-memory
faults: missing peer, producer crash, consumer crash, restart, slow consumer,
timeout, malformed header, version mismatch, stale memory, full queue, empty
queue, and rapid restart. AutoRuntime owns policy/adapter behavior; FastIPC owns
layout, liveness, reclaim, and close/unmap invariants.

## Limits

These tests establish typed outcomes and state transitions, not hard real-time
or Byzantine guarantees. Timing assertions use wide margins. The delay/drop
shim does not model reordering or bandwidth. RPC corruption is focused framing
coverage, not fuzzing. Target-hardware soak, physical network impairment, DDS
status storms, and authenticated hostile-peer tests remain production work.
