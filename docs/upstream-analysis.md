# AutoRuntime upstream analysis

## Scope and evidence

AutoRuntime starts from eclipse-ecal/tcp_pubsub at commit
1540876ee8aad623a9b089baaf3f948579b466d9. It is a compact C++ TCP
binary-blob pub/sub library built on standalone Asio. Exact history, license,
and tree hash are in the workspace UPSTREAMS.md.

A clean Release/Ninja baseline with pinned submodules completed all five
upstream CTest tests. This proves the selected transport baseline, not any new
scheduler, health, observability, or DDS requirement.

## Does upstream define a Node?

No. Public objects are Executor, Publisher, Subscriber, and SubscriberSession.
Publishers listen on TCP endpoints and subscribers connect to endpoint lists.
There is no node identity, lifecycle, topic registry, component graph,
service/client abstraction, timer, or runtime coordinator.

## How does communication work?

A publisher accepts TCP sessions and serializes each binary payload into a
packed little-endian TcpHeader plus copied caller bytes in a pooled vector. The
header has only header size, content type, a reserved byte, and payload size. A
one-byte handshake currently negotiates protocol version zero.

Each publisher session uses an Asio strand and allows one write in flight.
Later sends overwrite one next_buffer_to_send_ slot. This is an implicit
latest-value/drop-intermediate policy, not configurable backpressure.

A subscriber resolves an ordered publisher list, connects, handshakes, reads
the variable header and payload, and posts delivery on its strand. On failure it
closes the socket and retries after one second until its budget is exhausted.
It can fail over between configured endpoints, but has no discovery or
membership protocol.

## How does the Executor run?

Executor_Impl owns one shared asio::io_context, a work guard, and N threads
calling io_context::run. It is an I/O completion pool only: no task model,
priority queue, periodic release, deadline, admission, affinity, cancellation
token, or runtime statistics.

Stop resets the work guard and stops the context. The implementation destructor
detaches workers because each captures a shared pointer to the implementation.
That avoids joining self, but gives callers no explicit quiesce, drain, and join
contract.

## On which thread does a callback run?

- Synchronous mode invokes the user callback on the session's Asio strand and
  therefore an executor I/O worker. The next read is scheduled afterward. Slow
  callbacks can occupy the I/O pool and delay unrelated networking.
- Asynchronous mode creates one subscriber callback thread. Incoming messages
  overwrite one last_callback_data_ slot while the callback runs. This isolates
  I/O but collapses bursts silently and has no queue depth, deadline, priority,
  or drop metric.

Replacing or cancelling a callback joins its thread unless called from that
same thread, in which case upstream detaches it.

## How is Transport selected?

It is not selected: TCP is compiled into publisher, subscriber, protocol, and
endpoint APIs. There is no Transport seam and no shared-memory, UDS, DDS, or
mock backend. eCAL samples are external adapters, not a runtime abstraction.

## How does shutdown work?

Publisher cancellation closes/cancels the acceptor, marks it stopped, copies
the session list under a mutex, and cancels sessions. Subscriber cancellation
cancels sessions and stops its callback thread. A session cancels its socket,
retry timer, and resolver. The shared executor stops its I/O context.

There is no ordered runtime shutdown, drain deadline, callback cancellation
propagation, generation retirement, health transition, or guarantee that
detached workers have exited when a public object returns.

## How are failures handled?

Socket/protocol errors are logged. Subscriber sessions close and retry;
publisher sessions are removed after I/O error. Protocol versions above zero
are rejected. This is useful connection-level recovery, but cannot distinguish
process crash, stale instance, no progress, backlog, or deadline overrun.
There is no generation, heartbeat, lease, restart coordinator, or structured
application-visible failure reason.

The receive path trusts remote payload length when reserving and resizing a
buffer. The redesign must enforce configured frame bounds before allocation.

## QoS, metrics, and scheduling

- QoS: no reliability/history/depth/deadline/liveliness model. TCP provides
  ordered reliable bytes while latest-slot overwrite stays implicit.
- Metrics: subscriber count exists; message rates, depths, drops, callback
  latency, task timing, CPU, and end-to-end latency do not.
- Scheduling: no application scheduler. Strands serialize sessions but do not
  implement priority, period, deadline, or isolation groups.
- Tracing: logger callbacks exist; trace identity, sequence, timestamps, and
  structured event schema do not.

## Modification boundary

### Keep

- MIT license, Continental copyright, portable_endian.h notice, and history.
- Known-good CMake/CTest scaffold and selected TCP primitives during extraction.
- Endpoint failover behavior and basic, large-message, multi-peer, and failover
  regression tests.
- Buffer reuse and Asio ownership ideas only where new failure/lifetime tests
  demonstrate correctness.

### Rewrite

- Public API into Node, Publisher, Subscriber, Service, Client, Timer, Executor,
  Transport, and HealthMonitor boundaries.
- Executor into periodic/event/async tasks with priority, period, deadline,
  bounded queues, cancellation, callback groups, and timing metrics.
- Latest-value slots into explicit per-subscription queues and backpressure,
  including drop reasons, watermarks, and slow-callback isolation.
- TCP framing into a bounded versioned envelope carrying sequence, monotonic
  timestamps, trace identity, scheduling metadata, request correlation, and
  error status.
- Recovery into a generation-aware state machine distinguishing disconnect,
  peer replacement, stale generation, and no progress.
- Shutdown into stop-accepting, cancel, drain, close, and joined-worker phases.

### Remove

- eCAL tunnel samples from the runtime product surface.
- Implicit one-slot overwrite as the only queue policy.
- TCP endpoint types from application-facing component APIs.
- Detached worker/callback lifetime as normal shutdown behavior.
- Compatibility/build surfaces that do not serve the focused Linux runtime.

Removal does not erase attribution; imported commits and notices remain in
history.

### Add

- Runtime coordinator and component lifecycle around the unified API.
- Project-local FastIPC adapter for same-host high-rate communication.
- Mature ROS2/DDS client-library adapter for cross-host and ROS integration;
  AutoRuntime will not reimplement DDS.
- Reliable/best-effort, history/depth, deadline, and liveliness policy mapping
  plus a reproducible QoS experiment.
- Heartbeat, generation, state, progress, backlog, deadline overrun, crash
  detection, and reconnect/restart coordination.
- Structured logs, metrics, trace/sequence/timestamps, and Sensor-to-Control
  P50/P95/P99 measurement.
- Limited node discovery, heartbeat membership, and RPC without pretending to
  implement a consensus system.
- Automated scheduler, malformed-message, disconnect, restart, slow-callback,
  shutdown-under-load, and DDS-peer-loss faults with sanitizer builds.
