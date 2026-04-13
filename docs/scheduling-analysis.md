# Scheduling isolation analysis

Date: 2026-08-20

## Question

Can a 300 ms planning callback delay a 20 ms control loop, and does assigning
the two tasks to separate callback groups prevent that interference?

## Experiment

`autoruntime_scheduling_isolation_experiment` runs the same workload twice:

- a `slow-planning` event callback sleeps for 300 ms;
- a periodic `control-loop` is released every 20 ms with a 15 ms deadline;
- both tasks use one worker in the shared scenario;
- planning and control each use a one-worker callback group in the isolated
  scenario;
- control progress is sampled 220 ms into the 300 ms blockage, then final
  timing samples are collected after the backlog has drained.

Reproduce it with:

```bash
cmake --build projects/autoruntime/build-dds-release \
  --target autoruntime_scheduling_isolation_experiment -j2
projects/autoruntime/build-dds-release/autoruntime_scheduling_isolation_experiment \
  --output projects/autoruntime/docs/evidence/scheduling-isolation-results.json
```

The raw evidence is
[`scheduling-isolation-results.json`](evidence/scheduling-isolation-results.json).

## Result

| Measure | Shared worker group | Isolated worker groups |
| --- | ---: | ---: |
| Control executions during blocked window | 0 | 11 |
| Releases / finished | 21 / 21 | 21 / 21 |
| Deadline misses | 14 | 0 |
| Queue overflows | 0 | 0 |
| Response P50 | 80,162.030 us | 118.924 us |
| Response P95 | 260,159.457 us | 148.408 us |
| Response P99 | 280,159.195 us | 179.106 us |

The Release run used the same unpinned WSL2 host documented in
`e2e-latency-analysis.md`. It is a single run and its microsecond values are
not portable performance guarantees.

## Why priority alone does not help

The slow event has higher queued priority so that it deterministically starts
before the first periodic release. Once a callback is running, AutoRuntime
does not forcibly preempt it. Priority orders jobs waiting in a callback-group
queue; it cannot interrupt arbitrary user code.

In the shared scenario, periodic releases continue while the only worker is
sleeping. They run after planning returns, so 14 responses exceed 15 ms even
though no queue capacity is lost. A deeper queue preserves old work but does
not preserve timeliness.

In the isolated scenario, the control worker is independently runnable. It
executes 11 times during the 220 ms observation window, which matches the
expected cadence after allowing startup phase, and records no deadline miss.

## Runtime policy

The experiment supports these deployment rules:

1. put latency-critical control and safety callbacks in dedicated groups;
2. keep blocking planning, logging, and service handlers out of those groups;
3. treat priority as queue ordering, not operating-system preemption;
4. bound every task/subscription queue and choose whether stale work is useful;
5. monitor queue delay, response time, deadline misses, and high watermarks;
6. make long callbacks cooperate with `std::stop_token`, but do not assume
   cancellation can terminate uncooperative code;
7. use OS affinity and real-time scheduling only after measuring on the target
   Linux kernel and hardware.

## Limits

This test uses `sleep_for` to create a repeatable obstruction, not CPU
contention. It does not measure cache interference, priority inversion in
external locks, page faults, IRQ load, CPU frequency changes, or RT throttling.
Those require target-hardware experiments with pinned workers and kernel
scheduler evidence.
