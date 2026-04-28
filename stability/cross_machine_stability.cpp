#include "cross_machine_stability.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace autoruntime::stability {
namespace {

double P99(std::vector<double> samples) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil(0.99 * static_cast<double>(samples.size())));
  return samples[std::max<std::size_t>(1U, rank) - 1U];
}

}  // namespace

struct StabilityTracker::Impl {
  explicit Impl(std::size_t requested_window)
      : latency_window(std::max<std::size_t>(1U, requested_window)) {}

  mutable std::mutex mutex;
  const std::size_t latency_window;
  std::vector<double> baseline_latencies;
  std::deque<double> final_latencies;
  std::uint64_t latency_samples{0U};

  NetworkHealthState state{NetworkHealthState::Starting};
  bool failure_episode_active{false};
  bool discovery_initialized{false};
  bool discovery_available{false};
  std::uint64_t peer_generation{0U};

  bool dds_initialized{false};
  std::uint64_t dds_generation{0U};
  std::uint64_t next_dds_sequence{0U};

  StabilitySummary counters;

  void RecordLatency(double latency_us) {
    if (!std::isfinite(latency_us) || latency_us < 0.0) {
      return;
    }
    ++latency_samples;
    if (baseline_latencies.size() < latency_window) {
      baseline_latencies.push_back(latency_us);
    }
    final_latencies.push_back(latency_us);
    if (final_latencies.size() > latency_window) {
      final_latencies.pop_front();
    }
  }

  void ObserveDiscovery(const ProbeObservation& observation) {
    if (!observation.discovery_available) {
      ++counters.discovery_failures;
    }
    if (discovery_initialized &&
        discovery_available != observation.discovery_available) {
      if (observation.discovery_available) {
        ++counters.discovery_recovery_events;
      } else {
        ++counters.discovery_loss_events;
      }
    }
    discovery_initialized = true;
    discovery_available = observation.discovery_available;

    if (!observation.discovery_available ||
        observation.peer_generation == 0U) {
      return;
    }
    if (peer_generation != 0U &&
        peer_generation != observation.peer_generation) {
      ++counters.peer_generation_changes;
    }
    peer_generation = observation.peer_generation;
  }

  void ObserveRpc(const ProbeObservation& observation) {
    if (!observation.discovery_available) {
      return;
    }
    switch (observation.rpc_status) {
      case StatusCode::Ok:
        ++counters.rpc_successes;
        if (observation.rpc_latency_us) {
          RecordLatency(*observation.rpc_latency_us);
        }
        return;
      case StatusCode::Timeout:
        ++counters.rpc_timeouts;
        return;
      case StatusCode::Closed:
      case StatusCode::TransportError:
        ++counters.rpc_transport_errors;
        return;
      case StatusCode::NotFound:
      case StatusCode::InvalidArgument:
      case StatusCode::AlreadyExists:
      case StatusCode::QueueFull:
      case StatusCode::Dropped:
      case StatusCode::Cancelled:
      case StatusCode::StaleGeneration:
      case StatusCode::Unsupported:
      case StatusCode::Internal:
        ++counters.rpc_unexpected_errors;
        return;
    }
  }

  std::pair<NetworkHealthState, NetworkHealthReason> Classify(
      const ProbeObservation& observation) const {
    if (!observation.discovery_available) {
      return {
          NetworkHealthState::Failed,
          NetworkHealthReason::DiscoveryLost};
    }
    if (observation.rpc_status != StatusCode::Ok) {
      return {
          NetworkHealthState::Degraded,
          NetworkHealthReason::RpcUnavailable};
    }
    if (!observation.dds_recent) {
      return {
          NetworkHealthState::Degraded,
          NetworkHealthReason::DdsStalled};
    }
    return {
        NetworkHealthState::Running,
        NetworkHealthReason::Ready};
  }
};

StabilityTracker::StabilityTracker(std::size_t latency_window)
    : impl_(std::make_unique<Impl>(latency_window)) {}

StabilityTracker::~StabilityTracker() = default;
StabilityTracker::StabilityTracker(StabilityTracker&&) noexcept = default;
StabilityTracker& StabilityTracker::operator=(
    StabilityTracker&&) noexcept = default;

std::optional<StabilityTransition> StabilityTracker::ObserveProbe(
    const ProbeObservation& observation) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->counters.observation_count;
  impl_->ObserveDiscovery(observation);
  impl_->ObserveRpc(observation);
  if (impl_->dds_initialized && !observation.dds_recent) {
    ++impl_->counters.dds_stall_observations;
  }

  const auto [next_state, reason] = impl_->Classify(observation);
  if (next_state == NetworkHealthState::Failed) {
    impl_->failure_episode_active = true;
  }
  if (next_state == NetworkHealthState::Running &&
      impl_->failure_episode_active) {
    ++impl_->counters.recovery_count;
    impl_->failure_episode_active = false;
  }
  if (next_state == impl_->state) {
    return std::nullopt;
  }
  StabilityTransition transition{
      impl_->state, next_state, reason,
      impl_->counters.observation_count};
  impl_->state = next_state;
  ++impl_->counters.health_transitions;
  return transition;
}

void StabilityTracker::ObserveDds(std::uint64_t generation,
                                  std::uint64_t sequence) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->counters.dds_messages;
  if (!impl_->dds_initialized || generation > impl_->dds_generation) {
    impl_->dds_initialized = true;
    impl_->dds_generation = generation;
    impl_->next_dds_sequence = sequence + 1U;
    return;
  }
  if (generation < impl_->dds_generation ||
      sequence < impl_->next_dds_sequence) {
    ++impl_->counters.duplicate_messages;
    return;
  }
  if (sequence > impl_->next_dds_sequence) {
    impl_->counters.message_loss +=
        sequence - impl_->next_dds_sequence;
  }
  impl_->next_dds_sequence = sequence + 1U;
}

void StabilityTracker::ObserveRpcParseErrors(
    std::uint64_t cumulative_errors) {
  std::lock_guard lock(impl_->mutex);
  impl_->counters.rpc_parse_errors =
      std::max(impl_->counters.rpc_parse_errors, cumulative_errors);
}

StabilitySummary StabilityTracker::Summary() const {
  std::lock_guard lock(impl_->mutex);
  auto summary = impl_->counters;
  summary.latency_samples = impl_->latency_samples;
  summary.retained_latency_samples =
      impl_->baseline_latencies.size() + impl_->final_latencies.size();
  summary.baseline_p99_us = P99(impl_->baseline_latencies);
  summary.final_p99_us = P99(std::vector<double>(
      impl_->final_latencies.begin(), impl_->final_latencies.end()));
  summary.p99_drift_us =
      summary.final_p99_us - summary.baseline_p99_us;
  summary.final_state = impl_->state;
  return summary;
}

std::string_view NetworkHealthStateName(
    NetworkHealthState state) noexcept {
  switch (state) {
    case NetworkHealthState::Starting:
      return "STARTING";
    case NetworkHealthState::Running:
      return "RUNNING";
    case NetworkHealthState::Degraded:
      return "DEGRADED";
    case NetworkHealthState::Failed:
      return "FAILED";
  }
  return "UNKNOWN";
}

std::string_view NetworkHealthReasonName(
    NetworkHealthReason reason) noexcept {
  switch (reason) {
    case NetworkHealthReason::AwaitingPeer:
      return "awaiting_peer";
    case NetworkHealthReason::Ready:
      return "ready";
    case NetworkHealthReason::DiscoveryLost:
      return "discovery_lost";
    case NetworkHealthReason::RpcUnavailable:
      return "rpc_unavailable";
    case NetworkHealthReason::DdsStalled:
      return "dds_stalled";
  }
  return "unknown";
}

}  // namespace autoruntime::stability
