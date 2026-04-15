# Fault-injection matrix

Date: 2026-08-20

## Reproduction

```bash
cmake --build projects/autoruntime/build-dds-release -j2
ctest --test-dir projects/autoruntime/build-dds-release \
  -L fault --output-on-failure
```

The captured Release run is
[`fault-matrix-release.log`](evidence/fault-matrix-release.log): all
20 independently named CTest cases passed, with zero failures. The complete
matrix was also repeated five times with `--repeat until-fail:5`.

## AutoRuntime cases

| CTest suffix | Injected condition | Required observation |
| --- | --- | --- |
| `transport_closed` | publish after transport shutdown | typed `Closed`; publish-failure count increments |
| `queue_overflow` | hold a worker and exceed task capacity | typed `QueueFull`; overflow count increments |
| `slow_callback_isolation` | planning callback blocks for 300 ms | control callback in another group completes within 50 ms |
| `deadline_miss` | 10 ms callback with a 1 ms deadline | completed sample records a deadline miss |
| `cancelled_task` | cancel before an event release | later notify returns typed `Cancelled` |
| `heartbeat_loss` | advance monotonic time past the heartbeat lease | `FAILED/HeartbeatLost` |
| `no_progress` | heartbeat advances while progress stalls | `FAILED/NoProgress` |
| `backlog_degraded` | report backlog above the policy bound | `DEGRADED/BacklogExceeded` |
| `process_exit` | process probe changes from alive to dead | `FAILED/ProcessExited` |
| `stale_generation` | generation 1 update targets generation 2 | typed `StaleGeneration`; current state is unchanged |
| `rpc_disconnect` | connect to a closed TCP endpoint | typed `TransportError` within the call deadline |
| `rpc_corrupt_frame` | send a zeroed 28-byte request header | server increments malformed-request count |
| `duplicate_membership` | two announcements claim one id/generation/sequence | one record remains; duplicate count increments |
| `service_timeout` | service handler exceeds client deadline | client receives typed `Timeout` |
| `message_delay` | test transport delays publish by 40 ms | delivery succeeds and measured delay is at least 30 ms |
| `message_drop` | test transport silently drops one publish | no delivery; published and dropped counts are one |
| `slow_consumer` | hold a subscriber while publishing into depth two | bounded high watermark of two and observable drops |
| `node_restart` | fail generation 3, run recovery hooks | cleanup/start/reconnect run; generation 4 resumes |
| `shutdown_during_load` | stop with one callback active and four queued releases | stop token is observed; workers join; new work is rejected |
| `dds_participant_loss` | close a real Cyclone DDS participant, then publish | typed `Closed`; publish-failure count increments |

The DDS case exists only with `AUTORUNTIME_ENABLE_DDS=ON`. The other
19 cases run in the dependency-light Unix build. The delay/drop adapter is
test-only; it wraps the public `Transport` seam and is not a claimed
production network emulator.

## Specification coverage

The explicit cases map the requested fault families as follows:

- node crash and restart: `process_exit`, `node_restart`,
  plus the real `kill -9` integration below;
- heartbeat loss: `heartbeat_loss`;
- slow callback: `slow_callback_isolation`;
- message delay and drop: `message_delay`,
  `message_drop`;
- slow consumer and queue overflow: `slow_consumer`,
  `queue_overflow`;
- transport disconnect: `transport_closed` and
  `rpc_disconnect`;
- DDS peer/participant loss: `dds_participant_loss`;
- malformed message: `rpc_corrupt_frame`;
- shutdown during load: `shutdown_during_load`.

## Process and recovery integration

The named fault cases are supplemented by real process tests:

- `autoruntime.health_monitor` forks a planning process, sends
  `SIGKILL`, observes `FAILED/ProcessExited`, runs
  cleanup/start/reconnect hooks, advances generation 7 to 8, and verifies
  heartbeat and progress return it to `RUNNING`.
- `autoruntime.distributed` discovers a forked planning process over
  UDP, performs a TCP RPC, kills the child, waits for membership lease expiry,
  and verifies the endpoint disappears.
- `autoruntime.fastipc_transport` transfers a versioned envelope
  across a real parent/child shared-memory boundary.

## FastIPC substrate

The separately buildable FastIPC project contributes 12 named shared-memory
faults: peer missing, producer crash, consumer crash, restart, slow consumer,
timeout, malformed header, version mismatch, stale shared memory, full queue,
empty queue, and rapid restart. AutoRuntime checks runtime policy and adapter
behavior; FastIPC owns layout, liveness, and reclaim invariants.

## Limits

These tests establish typed outcomes and state transitions, not hard real-time
or Byzantine-fault guarantees. Timing assertions have wide margins. The
message-delay/drop shim is deterministic and process-local; it does not model
packet reordering, bandwidth limits, DDS status storms, or a lossy physical
network. RPC corruption is a focused framing test, not a fuzzer. Long target
hardware soak, network impairment, and authenticated hostile-peer testing
remain production work.
