# DDS QoS analysis

## Scope

AutoRuntime uses Eclipse Cyclone DDS 11.0.1 through its public C API and
generated IDL types. Cyclone is an external dependency: the repository neither
vendors nor modifies its source. `scripts/bootstrap_cyclonedds.sh` downloads
the tagged archive, verifies SHA-256
`c25d46075ad6b5cee564bda9e5f49509e9a6dbb1a8b858e708eb360d335cf973`,
and installs it below the ignored `.deps/` directory.

Cyclone describes itself as an open implementation of DDS and DDSI-RTPS in its
[official repository](https://github.com/eclipse-cyclonedds/cyclonedds).
The pinned [11.0.1 release](https://github.com/eclipse-cyclonedds/cyclonedds/releases/tag/11.0.1)
is used instead of a moving branch. Its documented application workflow is a
CMake install consumed by downstream applications, which is the model used
here.

## AutoRuntime-to-DDS mapping

| AutoRuntime QoS | Cyclone C API | Adapter behavior |
| --- | --- | --- |
| `Reliability::Reliable` | `dds_qset_reliability(..., DDS_RELIABILITY_RELIABLE, max_blocking_time)` | Writer may block for the configured bound when its history is full. |
| `Reliability::BestEffort` | `DDS_RELIABILITY_BEST_EFFORT` | No reliable-history blocking contract is requested. |
| `HistoryKind::KeepLast` | `dds_qset_history(..., DDS_HISTORY_KEEP_LAST, depth)` | The configured depth is passed after range validation. |
| `HistoryKind::KeepAll` | `DDS_HISTORY_KEEP_ALL` | Depth is intentionally ignored; DDS resource limits remain implementation/configuration concerns. |
| positive `deadline` | `dds_qset_deadline` | Applied to both DataWriter and DataReader. |
| automatic/manual liveliness | `dds_qset_liveliness` | Kind and lease are applied to both endpoints; manual writers assert liveliness before writes. |

These are the documented public setters in Cyclone's
[C QoS API](https://cyclonedds.io/docs/cyclonedds/latest/api/qos.html).
The same documentation notes that KeepLast alone uses `depth`, while bounded
KeepAll requires ResourceLimits, and that Reliable's max-blocking time is the
writer's bound when history is full. Deadline, Liveliness, Reliability, and
History all apply to DataWriter and DataReader according to Cyclone's
[QoS overview](https://cyclonedds.io/docs/cyclonedds/latest/about_dds/qos.html).

The IDL message carries AutoRuntime envelope version, trace/span lineage,
sequence, source/publish monotonic timestamps, source generation, priority, and
a bounded 1 MiB octet sequence. Services deliberately return `Unsupported`;
request/reply belongs to AutoRuntime's distributed RPC layer instead of being
silently emulated as a topic pair.

## Questions

The experiment answers two narrow questions:

1. With compatible endpoints on this host, what send-call rate, delivery count,
   and callback-observed latency do matched Reliable and matched BestEffort
   profiles produce?
2. Does a BestEffort writer fail to satisfy a Reliable reader's request, as the
   DDS requested/offered compatibility model predicts?

The second question is important because a successful `dds_write` does not
prove that any reader is compatible. Cyclone exposes explicit offered/requested
incompatible-QoS statuses in its
[entity status API](https://cyclonedds.io/docs/cyclonedds/latest/api/status.html).

## Method

Snapshot date: 2026-08-20.

- Host: WSL2 Linux 6.6.87.2, x86-64.
- CPU: Intel Core Ultra 9 275HX, 24 logical CPUs visible.
- Toolchain: GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1.
- Build: `Release`, Cyclone DDS 11.0.1.
- Topology: two distinct DDS participants in one process and one host; a unique
  domain and topic are used for each scenario.
- Payload: 128 bytes; one unmeasured discovery write, then a 750 ms discovery
  warm-up.
- Compatible scenarios: 5,000 attempted writes, KeepAll history, 2 s deadline,
  automatic liveliness with a 3 s lease.
- Incompatible scenario: 500 BestEffort writes offered to a Reliable reader.
- Send rate: successful synchronous `Publish` calls divided by producer loop
  time. It is not end-to-end throughput.
- Latency: receive-callback monotonic time minus the envelope's pre-write
  monotonic timestamp. Percentiles use nearest-rank over received samples.
- Completion: wait until all published samples arrive or delivery makes no
  progress for 750 ms.

The executable rejects the run unless each compatible scenario delivers at
least one sample and the incompatible scenario delivers none. Raw machine
output is preserved in
[`docs/evidence/dds-qos-results.json`](evidence/dds-qos-results.json).

## Observed result

| Scenario | Published / received | Loss | Send calls/s | P50 | P95 | P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Reliable writer / Reliable reader | 5,000 / 5,000 | 0.000% | 682,864.858 | 683.289 us | 1,074.968 us | 1,118.266 us |
| BestEffort writer / BestEffort reader | 5,000 / 5,000 | 0.000% | 853,467.776 | 985.819 us | 1,122.428 us | 1,135.278 us |
| BestEffort writer / Reliable reader | 500 / 0 | 100.000% | 2,215,094.540 | n/a | n/a | n/a |

For this run, matched BestEffort completed producer calls about 25.0% faster.
Its P50 callback latency was about 44.3% higher, while P95 and P99 were about
4.4% and 1.5% higher. Both matched scenarios delivered every sample. These
measurements do not support a blanket claim that one policy is always faster:
the run is local, short, unpinned, and includes queueing between a bursty writer
and a polling receive thread.

The incompatible pair delivered zero samples even though all 500 write calls
succeeded. That is the strongest behavioral result: endpoint compatibility,
not the writer's return status alone, determines whether data can flow.

## Runtime policy decision

- Use Reliable by default for control commands, state transitions, health
  events, and RPC-related messages where missing a sample changes behavior.
- Use BestEffort for high-rate replaceable sensor streams only when the
  application has explicit queue/drop metrics and tolerates loss.
- Treat History and ResourceLimits as part of the reliability decision.
  Reliable plus an unbounded backlog is not a valid overload strategy.
- Surface incompatible-QoS, deadline-missed, sample-lost, and liveliness status
  changes through HealthMonitor and observability rather than treating silence
  as a generic timeout.

## Limits and follow-up

This is a controlled integration experiment, not a network benchmark. It does
not inject packet loss, cross a physical network, pin CPUs, isolate DDS threads,
set explicit ResourceLimits, or compare multiple DDS implementations. The
zero-loss BestEffort result is therefore not evidence that BestEffort is
lossless. A follow-up multi-host run should use the same executable with network
impairment, record DDS status counters, repeat each case, and report confidence
intervals.

## Reproduce

```bash
cd projects/autoruntime
./scripts/bootstrap_cyclonedds.sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUTORUNTIME_ENABLE_DDS=ON \
  -DAUTORUNTIME_BUILD_BENCHMARKS=ON
cmake --build build-release --target autoruntime_dds_qos_experiment
./build-release/autoruntime_dds_qos_experiment \
  --messages 5000 \
  --output docs/evidence/dds-qos-results.json
```
