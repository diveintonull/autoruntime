# AutoRuntime

AutoRuntime is a Linux-focused C++20 robotics runtime built as a deep
derivative of
[eclipse-ecal/tcp_pubsub](https://github.com/eclipse-ecal/tcp_pubsub).
It keeps the pinned upstream history and MIT notices while replacing the
application-facing architecture with a transport-neutral runtime, explicit
scheduling policy, health supervision, observability, and bounded distributed
control-plane primitives.

This is an engineering portfolio project, not a production-certified
middleware distribution.

## Implemented capabilities

| Area | Current behavior |
| --- | --- |
| Runtime API | `Node`, `Publisher`, `Subscriber`, `Service`, `Client`, `Timer`, `Executor`, `Transport`, `HealthMonitor` |
| Scheduling | Periodic, Event, and Async tasks; priority queues; callback groups; bounded queues; cooperative cancellation; deadline/queue/execution samples |
| Pub/sub | Per-subscription queues, DropNewest/DropOldest overflow, slow-callback isolation, versioned message envelope |
| Services | Deadline-bounded request/reply on the in-memory transport; bounded service queue |
| Transports | In-memory, FastIPC shared memory, and an optional real Cyclone DDS 11.0.1 adapter |
| Recovery | Heartbeat, progress, backlog, deadline-miss and process-exit evaluation; generation-aware cleanup/start/reconnect hooks |
| Observability | Counters, gauges, bounded histograms, JSON logs, trace spans, E2E latency, CPU/RSS/context-switch snapshots |
| Distributed slice | Explicit-peer UDP discovery with bounded membership and lease expiry; framed deadline/cancellable TCP RPC |
| Verification | Default 26-test build; full 28-test DDS build across Debug/Release/ASan/UBSan/TSan; 20 named runtime fault cases |

## Architecture

```text
Node API
  |-- Publisher / Subscriber / Service / Client / Timer
  |          |
  |          +--> Executor callback groups and bounded queues
  |
  +--> Transport interface
         |-- InMemoryTransport
         |-- FastIpcTransport --> ../fastipc
         +-- DdsTransport -----> Cyclone DDS 11.0.1

HealthMonitor   Metrics / Logs / Traces   Discovery / RPC
     \                 |                     /
      +----------- application policy -------+
```

The runtime core never names a concrete transport type. Imported
`tcp_pubsub` remains available only as an opt-in baseline target and is not
linked into the default AutoRuntime library. See
[architecture.md](docs/architecture.md) and
[UPSTREAM_DIFF.md](UPSTREAM_DIFF.md).

## Dependency-light build

From the repository root:

```bash
cmake -S projects/autoruntime -B projects/autoruntime/build -G Ninja
cmake --build projects/autoruntime/build
ctest --test-dir projects/autoruntime/build --output-on-failure
```

DDS is opt-in, so these commands require CMake, Ninja, a C++20 compiler,
pthreads, and the adjacent FastIPC project only. The verified default build
registered 26 tests and passed 26/26.

Run the sensor -> planning -> control example:

```bash
projects/autoruntime/build/autoruntime_pipeline_demo
```

## Full DDS and sanitizer matrix

The bootstrap script downloads Cyclone DDS 11.0.1, verifies its pinned SHA-256,
and installs it below the ignored `.deps` directory:

```bash
projects/autoruntime/scripts/bootstrap_cyclonedds.sh
projects/autoruntime/scripts/run_test_matrix.sh all
```

The matrix explicitly sets `AUTORUNTIME_ENABLE_DDS=ON` and runs Debug,
Release, ASan, UBSan, and TSan. See
[testing.md](docs/testing.md) for revision-pinned logs and the WSL2 TSan launch
note.

## Experiments and evidence

Release builds expose:

```bash
projects/autoruntime/build-verify-release/autoruntime_pipeline_latency_benchmark
projects/autoruntime/build-verify-release/autoruntime_scheduling_isolation_experiment
projects/autoruntime/build-verify-release/autoruntime_dds_qos_experiment
```

Measured results and raw JSON are in:

- [E2E latency analysis](docs/e2e-latency-analysis.md)
- [callback-group isolation analysis](docs/scheduling-analysis.md)
- [DDS QoS analysis](docs/dds-qos-analysis.md)
- [fault-injection matrix](docs/fault-injection.md)
- [recovery design and crash test](docs/recovery.md)

## Upstream and attribution

- Primary upstream: `eclipse-ecal/tcp_pubsub`
- Pinned commit: `1540876ee8aad623a9b089baaf3f948579b466d9`
- Import commit: `99b2b5d`
- License: MIT, preserved in [LICENSE](LICENSE)
- Additional retained notice: `tcp_pubsub/src/portable_endian.h`

The full upstream source, tests, samples, history, and notices remain in the
tree. AutoRuntime's new modules are separate directories and targets. Exact
Keep/Rewrite/Add boundaries are in [UPSTREAM_DIFF.md](UPSTREAM_DIFF.md).

## Known limitations

- `HealthMonitor` supplies policy and recovery hooks, not a privileged process
  supervisor or deployment daemon.
- FastIPC and DDS adapters currently implement pub/sub only. Runtime services
  work on `InMemoryTransport`; cross-machine service calls use the separate
  distributed RPC slice.
- `Executor::Stop(Deadline)` requests cooperative stop and joins all workers,
  but the deadline argument is not yet enforced. A callback that ignores its
  stop token can delay shutdown indefinitely.
- Priorities order queued jobs inside a callback group; this is not OS
  real-time scheduling, admission control, CPU affinity, or priority
  inheritance.
- Discovery and RPC support numeric IPv4 endpoints and explicit peers. They
  provide no authentication, encryption, consensus, NAT traversal, or
  Byzantine protection.
- Trace and histogram stores are in-process and bounded only where documented;
  there is no OpenTelemetry exporter or durable metrics backend.
- WSL2 benchmark numbers are comparative evidence, not target-hardware or hard
  real-time guarantees.
