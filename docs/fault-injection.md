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
15 independently named CTest cases passed in 0.51 seconds.

## AutoRuntime cases

| CTest case | Injected condition | Required observation |
| --- | --- | --- |
| `transport_closed` | publish after transport shutdown | typed `Closed`, publish-failure counter increments |
| `queue_overflow` | hold a worker and exceed task capacity | typed `QueueFull`, overflow counter increments |
| `slow_callback_isolation` | planning callback sleeps 300 ms | control callback on a separate group completes within 50 ms |
| `deadline_miss` | 10 ms callback with 1 ms deadline | completed sample is marked as a deadline miss |
| `cancelled_task` | cancel before event release | later notify returns typed `Cancelled` |
| `heartbeat_loss` | monotonic time advances beyond heartbeat lease | component enters `FAILED/HeartbeatLost` |
| `no_progress` | heartbeats continue while progress sequence stalls | component enters `FAILED/NoProgress` |
| `backlog_degraded` | reported backlog exceeds policy bound | component enters `DEGRADED/BacklogExceeded` |
| `process_exit` | process probe changes from alive to dead | component enters `FAILED/ProcessExited` |
| `stale_generation` | generation 1 update targets generation 2 record | typed `StaleGeneration`; current state is untouched |
| `rpc_disconnect` | connect to a closed TCP endpoint | typed `TransportError` within the call deadline |
| `rpc_corrupt_frame` | send a zeroed 28-byte request header | server rejects it and increments malformed-request count |
| `duplicate_membership` | two senders claim the same id/generation/sequence | one record remains and duplicate counter increments |
| `service_timeout` | service callback exceeds client deadline | client receives typed `Timeout` |
| `dds_participant_loss` | close the real Cyclone DDS participant then publish | typed `Closed`, publish-failure counter increments |

The DDS case is registered only when `AUTORUNTIME_ENABLE_DDS=ON`; the other
14 cases run in the dependency-light Unix build.

## Process and recovery integration cases

The fault label is supplemented by real process tests:

- `autoruntime.health_monitor` forks a planning process, sends `SIGKILL`,
  observes `FAILED/ProcessExited`, runs cleanup/start/reconnect hooks,
  advances generation 7 to 8, and verifies heartbeat/progress return it to
  `RUNNING`.
- `autoruntime.distributed` discovers a forked planning process over UDP,
  performs a TCP RPC, kills the child, waits for membership lease expiry, and
  verifies the endpoint disappears.
- `autoruntime.fastipc_transport` transfers a versioned envelope across a
  real parent/child shared-memory boundary.

## FastIPC substrate cases

The separately buildable FastIPC project contributes 12 named CTest faults:
peer missing, producer crash, consumer crash, restart, slow consumer, timeout,
malformed header, version mismatch, stale shared memory, full queue, empty
queue, and rapid restart. Its Release/ASan/UBSan/TSan logs live under
`projects/fastipc/tests/results/`.

This separation is intentional: AutoRuntime checks policy and cross-transport
behavior, while FastIPC owns shared-memory layout, peer liveness, and reclaim
faults.

## Limits

Most cases assert deterministic state/status transitions rather than timing
precision. The two timing assertions use large margins (300 ms interference
versus a 50 ms isolated completion window, and explicit deadline expiry).
They validate failure semantics, not worst-case real-time guarantees.

Network corruption currently tests framing and bounds, not fuzzing,
authentication, packet reordering, or hostile traffic. Those remain security
hardening work.
