# AutoRuntime upstream-to-derivative boundary

## Purpose

This file separates retained `tcp_pubsub` history from AutoRuntime's original
and rewritten work. It is an authorship and maintenance map, not a research
novelty claim.

## Provenance and legal boundary

- Primary upstream:
  [eclipse-ecal/tcp_pubsub](https://github.com/eclipse-ecal/tcp_pubsub)
- Pinned upstream commit:
  `1540876ee8aad623a9b089baaf3f948579b466d9`
- Local subtree import commit: `99b2b5d`
- License: MIT; upstream copyright remains in `LICENSE`.
- Additional retained notice:
  `tcp_pubsub/src/portable_endian.h`.

The import preserves upstream source, samples, tests, submodule metadata and
history. No code from the two secondary references was vendored.

## What upstream supplied

The pinned baseline supplies an Asio-based binary TCP publisher/subscriber,
connection handshake/framing, executor thread pool, endpoint failover, a
single next-message slot, CMake/package scaffolding, examples, and tests for
basic, large-message, multi-publisher/subscriber and failover behavior.

It does not supply the AutoRuntime Node API, task model, callback groups,
explicit per-subscription queues, service/client/timer API, transport-neutral
contract, FastIPC/DDS adapters, health/recovery state machine, runtime metrics,
structured tracing, bounded membership, distributed RPC, fault matrix, or the
three experiments.

## Current build boundary

The default `autoruntime` target compiles only the new
`runtime`, `scheduler`, `health`, `observability`, `distributed`, and
`transport` modules. It links the adjacent FastIPC target when enabled.
Cyclone DDS is optional and off by default.

Imported `tcp_pubsub`, its samples, and its tests remain physically present
for provenance and baseline comparison. They enter the target graph only when
`AUTORUNTIME_BUILD_TCP_BASELINE=ON` and its pinned submodules are available.
The default runtime therefore is not a thin wrapper around the imported
library.

## Keep, rewrite, add

### Kept with attribution

| Item | Disposition |
| --- | --- |
| MIT license, Continental copyright, portable_endian notice | retained verbatim |
| Full upstream source/history/tests/samples | retained as the baseline subtree |
| TCP handshake/framing and failover tests | baseline/reference only |
| CMake repository shape and platform workflow artifacts | retained and superseded by root CI |

### Rewritten or replaced

| Surface | Baseline | AutoRuntime |
| --- | --- | --- |
| Public API | TCP publisher/subscriber classes | unified Node/Pub/Sub/Service/Client/Timer/Executor/Transport/HealthMonitor |
| Execution | generic shared Asio thread pool | Periodic/Event/Async tasks, callback groups, priorities, bounds, cancellation, samples |
| Queue semantics | implicit single next-message slot | per-subscription depth, DropNewest/DropOldest, high watermark and drops |
| Callback behavior | socket delivery callback | transport callback -> bounded subscription queue -> executor callback |
| Transport coupling | TCP types at public surface | protocol-neutral interface with typed unsupported capabilities |
| Lifecycle | connection-level ownership | endpoint RAII, cooperative executor stop, transport close/join, generation fencing |
| Build | baseline library and examples | new library by default; imported baseline explicitly opt-in |

### Added by derivative work

| Capability | Evidence |
| --- | --- |
| In-memory runtime services and pub/sub | `runtime/`, `transport/src/in_memory_transport.cpp`, runtime tests |
| FastIPC cross-process adapter and real restart flow | `transport/src/fastipc_transport.cpp`, `tests/fastipc_transport_test.cpp` |
| Cyclone DDS 11.0.1 adapter and QoS mapping | `transport/src/dds_transport.cpp`, DDS tests/experiment |
| Health evaluation and recovery hooks | `health/`, `docs/recovery.md` |
| Metrics, JSON logs, spans, E2E latency and CPU metrics | `observability/`, benchmark evidence |
| Bounded UDP discovery and framed TCP RPC | `distributed/`, distributed tests |
| 20 named runtime fault cases | `tests/fault_injection_test.cpp`, `docs/fault-injection.md` |
| Five-profile sanitizer/build matrix and root CI | scripts, workflow, `docs/testing.md` |

## Quantitative review aids

At implementation checkpoint `4e60e21`:

- new production module files contain 5,758 physical lines across
  `.cpp`, `.hpp`, and `.idl`;
- derivative tests, benchmarks, and the runnable example contain 3,975 physical
  lines;
- the uniformly counted retained upstream C/C++ baseline contains 4,334
  physical lines;
- `git diff --shortstat
  99b2b5d:projects/autoruntime 4e60e21:projects/autoruntime` reports
  56 files changed, 11,002 insertions, and 74 deletions.

These are scope aids, not authorship percentages. Documentation and generated
measurement records appear in the diff, while retained upstream files outside
the diff still belong to the built history.

## Incremental commit evidence

| Commit | Boundary introduced |
| --- | --- |
| `3ce2e98` | upstream architecture audit and modification plan |
| `b3b3b8a` | unified runtime API and executor |
| `254b3ab` | cross-process FastIPC adapter |
| `0c97dfb` | real Cyclone DDS adapter |
| `7810152` | measured DDS QoS experiment |
| `0783e88` | health monitor and recovery policy |
| `f40606c` | metrics, structured logs, traces and CPU data |
| `713d54a` | sensor-planning-control E2E benchmark |
| `290e60f` | bounded discovery and distributed RPC |
| `827226f`, `2f02c65` | explicit runtime and delivery fault matrices |
| `6a97732` | callback-group isolation experiment |
| `da6c4e1` | root CI and five-profile verification |
| `deb6e85` | runnable pipeline example |
| `2bdd95f` | close/unmap lifetime fix plus real post-restart message proof |
| `4e60e21` | dependency-light default build; DDS remains verified opt-in |

## Secondary references

- [gazebosim/gz-transport](https://github.com/gazebosim/gz-transport) informed
  comparison of Node/pub-sub/service seams, discovery behavior, and test
  taxonomy.
- [shawnfeng0/uorb](https://github.com/shawnfeng0/uorb) informed comparison of
  queue depth, callback/poll semantics, and sanitizer organization.

No source from either repository is compiled or copied here.

## Deliberate non-claims

AutoRuntime does not claim ROS 2 compatibility, DDS implementation ownership,
hard real-time scheduling, process-daemon supervision, secure cluster
membership, zero-copy messages, production certification, or parity with
ROS 2, Apollo Cyber RT, iceoryx, eCAL, or a complete DDS stack. Recorded WSL2
experiments are comparative evidence for this implementation only.
