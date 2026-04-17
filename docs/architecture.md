# AutoRuntime architecture

## Design intent

AutoRuntime separates application semantics, scheduling, transport, recovery,
observability, and the small distributed control plane. The goal is to make
ownership and failure behavior inspectable through narrow public seams rather
than hide them behind a monolithic middleware singleton.

## Module map

| Module | Public seam | Main implementation | Responsibility |
| --- | --- | --- | --- |
| Runtime | `runtime/include/autoruntime/node.hpp` | `runtime/src/node.cpp` | Node-scoped endpoints, subscription/service queues, timers, envelopes |
| Scheduler | `scheduler/include/autoruntime/executor.hpp` | `scheduler/src/executor.cpp` | task release, priority dispatch, callback groups, cancellation, samples |
| Transport | `transport/include/autoruntime/transport.hpp` | `transport/src/*` | protocol-neutral pub/sub and request/reply contract |
| Health | `health/include/autoruntime/health_monitor.hpp` | `health/src/health_monitor.cpp` | state evaluation, generations, restart budget and hooks |
| Observability | `observability/include/autoruntime/observability.hpp` | `observability/src/observability.cpp` | metrics, JSON logs, spans, process usage |
| Distributed | `distributed/include/autoruntime/distributed.hpp` | `distributed/src/*` | bounded discovery and framed RPC |

## Data path

1. A `Publisher` builds a versioned `MessageEnvelope` with trace/span ids,
   sequence, monotonic timestamps, source generation, and priority.
2. The selected `Transport` publishes the message. It owns wire/shared-memory
   encoding, concrete QoS mapping, and transport counters.
3. A transport subscription callback calls `Subscriber::Impl::Accept`.
4. The subscriber applies its own bounded queue and explicit DropNewest or
   DropOldest policy.
5. At most one Event task is scheduled for that subscription. `ProcessOne`
   removes one message, invokes the application callback, then reschedules
   itself while backlog remains.
6. Executor samples capture release, queue, execution, response, and deadline
   timing independently of transport metrics.

This two-stage transport/subscription boundary deliberately isolates a slow
application callback from a transport receive thread.

## Transport capability matrix

| Capability | In-memory | FastIPC | Cyclone DDS |
| --- | --- | --- | --- |
| Pub/sub | yes | yes, configured SPSC endpoints | yes |
| Service/client | yes | unsupported | unsupported |
| Cross-process | no | same Linux host | DDS domain |
| Queue/QoS mapping | runtime + local bus | reliable -> bounded timeout; best effort -> drop | reliability/history/deadline/liveliness |
| Concrete lifetime | shared bus state | receiver `jthread` plus FastIPC channel | participant/readers/writers plus receiver `jthread` |

The common interface exposes service methods so a transport can return typed
`Unsupported`; it does not pretend every adapter has identical capabilities.

## Scheduler model

A scheduler thread releases periodic jobs. Event jobs are released by
`Notify`; Async jobs release once when added to a running executor. Each
callback group owns a bounded priority queue and a configured worker set.
Priority is descending, then release time, then insertion sequence.

Task and group capacity are checked separately. Overflow returns
`QueueFull` and increments task counters. Cancellation sets a stop source,
prevents future dispatch, and gives running cooperative callbacks a
`stop_token`. Finished callbacks retain at most 4096 samples per task.

## Ownership and lifetime

- The caller owns the shared `Executor` and concrete `Transport`.
- `Node` keeps shared ownership of both; endpoint handles keep shared PImpls.
- Subscription and service transport callbacks capture weak endpoint state so
  callback deregistration does not create a reference cycle.
- Endpoint destructors call idempotent `Close`/`Cancel`.
- Executor callback groups and tasks must be configured before `Start` where
  documented; the executor is one-shot after stopping.
- Concrete transports mark closed, stop receiver threads, close their substrate,
  and join before destruction.

## Health and recovery

`HealthMonitor` stores component state separately from the execution and
transport objects. It evaluates process liveness, heartbeat freshness, progress,
backlog, and deadline misses. Updates carry a generation; stale generations
cannot revive a replacement process. Recovery transitions a failed component
through `Recovering`, invokes application-owned cleanup/start/reconnect hooks
outside the monitor lock, advances the generation, and returns to `Starting`
until heartbeat/progress establish `Running`.

See [recovery.md](recovery.md) for the verified SIGKILL flow.

## Observability

Scheduler samples, subscription stats, transport stats, health transitions,
metrics, structured logs, trace spans, and process resource usage remain
separate data products. The benchmark composes three spans per trace to derive
sensor-to-control latency; it does not infer latency from wall-clock log
timestamps.

## Distributed control plane

Discovery sends versioned UDP announcements to an explicit bounded peer list.
Records are keyed by node id and fenced by generation plus heartbeat sequence;
leases remove silent members. RPC uses a size-bounded versioned TCP frame,
nonblocking I/O, monotonic deadlines, cancellation polling, and typed response
status.

The distributed slice is intentionally not a general cluster manager.

## Shutdown order

A safe application shutdown is:

1. stop new publication and timers;
2. close subscriptions/services so their transport callbacks are removed;
3. request executor stop and let cooperative callbacks return;
4. close concrete transports and join receiver threads;
5. stop discovery/RPC and persist final metrics if desired.

`Executor::Stop` currently joins without enforcing its supplied deadline, so
applications must keep callbacks cooperative.

## Invariants

- A Node generation is nonzero and accompanies every published envelope.
- Subscription queues and callback-group queues are bounded.
- Transport callbacks never own application endpoint lifetime strongly.
- A stale health generation cannot mutate current state.
- Discovery membership and RPC frames have explicit size bounds.
- Imported `tcp_pubsub` is excluded from the default AutoRuntime target.

## Non-goals

This implementation does not provide hard real-time scheduling, zero-copy DDS
loans, a process-manager daemon, secure discovery, distributed consensus,
schema evolution beyond the current envelope, or ROS 2 API compatibility.
