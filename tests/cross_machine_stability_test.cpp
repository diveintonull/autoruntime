#include "test_support.hpp"

#include "cross_machine_stability.hpp"

#include <autoruntime/status.hpp>

#include <cstddef>
#include <cstdint>

namespace {

using autoruntime::StatusCode;
using autoruntime::stability::NetworkHealthState;
using autoruntime::stability::ProbeObservation;
using autoruntime::stability::StabilityTracker;

ProbeObservation RunningProbe(std::uint64_t generation, double latency_us) {
  ProbeObservation observation;
  observation.discovery_available = true;
  observation.peer_generation = generation;
  observation.rpc_status = StatusCode::Ok;
  observation.dds_recent = true;
  observation.rpc_latency_us = latency_us;
  return observation;
}

int FailureAndRecoveryTransitionsAreExplicit() {
  StabilityTracker tracker(8U);

  const auto started = tracker.ObserveProbe(RunningProbe(1U, 100.0));
  CHECK(started);
  CHECK(started->previous == NetworkHealthState::Starting);
  CHECK(started->current == NetworkHealthState::Running);

  auto delayed = RunningProbe(1U, 0.0);
  delayed.rpc_status = StatusCode::Timeout;
  delayed.rpc_latency_us.reset();
  const auto degraded = tracker.ObserveProbe(delayed);
  CHECK(degraded);
  CHECK(degraded->previous == NetworkHealthState::Running);
  CHECK(degraded->current == NetworkHealthState::Degraded);

  ProbeObservation disconnected;
  disconnected.discovery_available = false;
  disconnected.rpc_status = StatusCode::NotFound;
  disconnected.dds_recent = false;
  const auto failed = tracker.ObserveProbe(disconnected);
  CHECK(failed);
  CHECK(failed->previous == NetworkHealthState::Degraded);
  CHECK(failed->current == NetworkHealthState::Failed);

  auto dds_stalled = RunningProbe(1U, 0.0);
  dds_stalled.dds_recent = false;
  dds_stalled.rpc_latency_us.reset();
  const auto recovering = tracker.ObserveProbe(dds_stalled);
  CHECK(recovering);
  CHECK(recovering->previous == NetworkHealthState::Failed);
  CHECK(recovering->current == NetworkHealthState::Degraded);

  const auto recovered = tracker.ObserveProbe(RunningProbe(1U, 80.0));
  CHECK(recovered);
  CHECK(recovered->previous == NetworkHealthState::Degraded);
  CHECK(recovered->current == NetworkHealthState::Running);

  CHECK(!tracker.ObserveProbe(RunningProbe(2U, 75.0)));
  const auto summary = tracker.Summary();
  CHECK(summary.health_transitions == 5U);
  CHECK(summary.discovery_loss_events == 1U);
  CHECK(summary.discovery_recovery_events == 1U);
  CHECK(summary.recovery_count == 1U);
  CHECK(summary.peer_generation_changes == 1U);
  CHECK(summary.rpc_timeouts == 1U);
  CHECK(summary.discovery_failures == 1U);
  CHECK(summary.final_state == NetworkHealthState::Running);
  return 0;
}

int DdsSequenceAndLatencyStateStayBounded() {
  StabilityTracker tracker(32U);
  tracker.ObserveDds(1U, 1U);
  tracker.ObserveDds(1U, 2U);
  tracker.ObserveDds(1U, 5U);
  tracker.ObserveDds(1U, 5U);
  tracker.ObserveDds(2U, 1U);

  constexpr std::size_t kProbeCount = 1'000'000U;
  for (std::size_t index = 0U; index < kProbeCount; ++index) {
    const auto latency =
        50.0 + static_cast<double>(index % 101U);
    static_cast<void>(
        tracker.ObserveProbe(RunningProbe(2U, latency)));
  }
  tracker.ObserveRpcParseErrors(3U);

  const auto summary = tracker.Summary();
  CHECK(summary.dds_messages == 5U);
  CHECK(summary.message_loss == 2U);
  CHECK(summary.duplicate_messages == 1U);
  CHECK(summary.rpc_successes == kProbeCount);
  CHECK(summary.rpc_parse_errors == 3U);
  CHECK(summary.latency_samples == kProbeCount);
  CHECK(summary.retained_latency_samples == 64U);
  CHECK(summary.baseline_p99_us >= 50.0);
  CHECK(summary.final_p99_us >= 50.0);
  CHECK(summary.final_state == NetworkHealthState::Running);
  return 0;
}

int UnexpectedRpcErrorsAreSeparated() {
  StabilityTracker tracker(4U);
  auto transport_error = RunningProbe(1U, 0.0);
  transport_error.rpc_status = StatusCode::TransportError;
  transport_error.rpc_latency_us.reset();
  static_cast<void>(tracker.ObserveProbe(transport_error));

  auto protocol_error = RunningProbe(1U, 0.0);
  protocol_error.rpc_status = StatusCode::InvalidArgument;
  protocol_error.rpc_latency_us.reset();
  static_cast<void>(tracker.ObserveProbe(protocol_error));

  const auto summary = tracker.Summary();
  CHECK(summary.rpc_transport_errors == 1U);
  CHECK(summary.rpc_unexpected_errors == 1U);
  return 0;
}

int DdsStallObservationsAreCountedWithoutStateChurn() {
  StabilityTracker tracker(4U);
  tracker.ObserveDds(1U, 1U);
  auto stalled = RunningProbe(1U, 42.0);
  stalled.dds_recent = false;

  const auto first = tracker.ObserveProbe(stalled);
  CHECK(first);
  CHECK(first->current == NetworkHealthState::Degraded);
  CHECK(!tracker.ObserveProbe(stalled));

  const auto summary = tracker.Summary();
  CHECK(summary.dds_stall_observations == 2U);
  CHECK(summary.health_transitions == 1U);
  CHECK(summary.final_state == NetworkHealthState::Degraded);
  return 0;
}

}  // namespace

int main() {
  if (const int result = FailureAndRecoveryTransitionsAreExplicit();
      result != 0) {
    return result;
  }
  if (const int result = DdsSequenceAndLatencyStateStayBounded();
      result != 0) {
    return result;
  }
  if (const int result = UnexpectedRpcErrorsAreSeparated(); result != 0) {
    return result;
  }
  return DdsStallObservationsAreCountedWithoutStateChurn();
}
