# Health and recovery design

## State model

```text
STARTING --heartbeat/progress--> RUNNING
RUNNING  --backlog/deadline----> DEGRADED
DEGRADED --signals clear------> RUNNING
*        --exit/lease/stall----> FAILED
FAILED   --Recover()----------> RECOVERING
RECOVERING --hooks succeed----> STARTING (generation + 1)
RECOVERING --hook/timeout-----> FAILED
```

`HealthState` and `HealthReason` are separate. A component can therefore be
`FAILED/ProcessExited`, `FAILED/HeartbeatLost`, or
`DEGRADED/BacklogExceeded` without encoding reason in free-form text.

## Detection

`Evaluate` first snapshots probe requests under the monitor mutex, executes
the possibly blocking process probes without that mutex, and then applies
results only when PID and generation still match. Evaluation order is:

1. process exit;
2. recovery timeout;
3. heartbeat timeout;
4. no-progress timeout;
5. backlog bound;
6. deadline-miss bound;
7. degraded-to-running recovery.

Heartbeat and progress sequences must increase. Every update carries the
registered generation; old or unregistered future generations return
`StaleGeneration`.

## Recovery transaction

`Recover` accepts only a `Failed` component with remaining restart budget.
It reserves the next generation and changes state to `Recovering` under the
monitor lock. It then invokes three application-owned hooks outside the lock:

1. `cleanup(name, old_generation)`
2. wait the configured restart backoff
3. `start(name, new_generation) -> pid`
4. `reconnect(name, new_generation)`

Failures are caught and converted to typed status. A failed reconnect triggers
best-effort cleanup of the replacement generation. Success commits only if the
component still has the old generation and remains `Recovering`; otherwise it
returns `StaleGeneration`. The component enters `Starting` with cleared
heartbeat/progress/backlog/deadline counters.

The monitor does not fork processes itself. Hook ownership keeps deployment
policy outside the reusable state machine.

## Real SIGKILL and data-flow test

`tests/fastipc_transport_test.cpp` performs a complete same-host recovery:

1. the parent creates a FastIPC publisher as camera generation 9;
2. an exec'd child creates the planning subscriber at generation 7;
3. `before-crash` crosses shared memory and its envelope is checked;
4. the parent sends `SIGKILL` and reaps the child;
5. `HealthMonitor::Evaluate` observes
   `FAILED/ProcessExited`;
6. recovery hooks start a new exec'd child at generation 8;
7. reconnect publishes `after-restart`;
8. the replacement proves receiver generation 8, source generation 9,
   sequence 2, payload, and a nonzero trace id;
9. heartbeat and progress for generation 8 return health to `RUNNING`;
10. the replacement and publisher close cleanly.

The Release test was repeated ten times; the captured result is
[evidence/recovery-repeat-10.log](evidence/recovery-repeat-10.log).

## Bug found by the test

The first stronger version restored the message flow but the replacement
occasionally crashed during shutdown. ASan reported a read after
`munmap`: the FastIPC receiver thread was still unwinding a blocked
`Receive` while another thread's `Close` released the mapping.

Commit `2bdd95f` fixed the substrate with an in-process operation lease:

1. closure rejects new leases;
2. both futex epochs are incremented and woken;
3. active operations observe `Closed` and release leases;
4. `Close` waits for the active count to reach zero;
5. role metadata, mapping, and fd are released last.

FastIPC now has a direct
`LocalCloseWaitsForBlockedOperationBeforeUnmapping` regression, and the
AutoRuntime crash/restart test exercises the full adapter lifecycle. All five
sanitizer/build profiles pass with this fix.

## Ownership and concurrency

- `HealthMonitor` owns only state, policy, transition history, and the probe
  callable.
- Application/deployment code owns process handles and hook side effects.
- Hooks never run while the monitor mutex is held.
- A generation is the logical incarnation fence; a PID is only a liveness
  observation.
- FastIPC independently fences substrate roles with PID, process start ticks,
  channel generation, and role token.
- Recovery is synchronous in this slice. A production supervisor would run it
  on a dedicated control-plane executor.

## Limits

- No process daemon, cgroup/systemd integration, exponential backoff, rolling
  restart, dependency graph, or persisted restart budget.
- A hook can block past the caller deadline because C++ cannot preempt an
  arbitrary synchronous function. Deadlines are checked between hook stages.
- Process probing uses `kill(pid, 0)` by default and does not by itself fence
  PID reuse; generation and the FastIPC substrate's start-tick checks carry
  that responsibility in the verified path.
- Recovery establishes `Starting`, not immediate health. Fresh heartbeat and
  progress are required.
- No multi-node leader election or exactly-once restart semantics.
