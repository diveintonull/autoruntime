#pragma once

#include <autoruntime/status.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace autoruntime::stability {

enum class NetworkHealthState {
  Starting,
  Running,
  Degraded,
  Failed,
};

enum class NetworkHealthReason {
  AwaitingPeer,
  Ready,
  DiscoveryLost,
  RpcUnavailable,
  DdsStalled,
};

struct ProbeObservation {
  bool discovery_available{false};
  std::uint64_t peer_generation{0U};
  StatusCode rpc_status{StatusCode::NotFound};
  bool dds_recent{false};
  std::optional<double> rpc_latency_us;
};

struct StabilityTransition {
  NetworkHealthState previous{NetworkHealthState::Starting};
  NetworkHealthState current{NetworkHealthState::Starting};
  NetworkHealthReason reason{NetworkHealthReason::AwaitingPeer};
  std::uint64_t observation_index{0U};
};

struct StabilitySummary {
  std::uint64_t observation_count{0U};
  std::uint64_t discovery_failures{0U};
  std::uint64_t discovery_loss_events{0U};
  std::uint64_t discovery_recovery_events{0U};
  std::uint64_t peer_generation_changes{0U};
  std::uint64_t rpc_successes{0U};
  std::uint64_t rpc_timeouts{0U};
  std::uint64_t rpc_transport_errors{0U};
  std::uint64_t rpc_unexpected_errors{0U};
  std::uint64_t rpc_parse_errors{0U};
  std::uint64_t dds_stall_observations{0U};
  std::uint64_t dds_messages{0U};
  std::uint64_t message_loss{0U};
  std::uint64_t duplicate_messages{0U};
  std::uint64_t health_transitions{0U};
  std::uint64_t recovery_count{0U};
  std::uint64_t latency_samples{0U};
  std::size_t retained_latency_samples{0U};
  double baseline_p99_us{0.0};
  double final_p99_us{0.0};
  double p99_drift_us{0.0};
  NetworkHealthState final_state{NetworkHealthState::Starting};
};

class StabilityTracker {
 public:
  explicit StabilityTracker(std::size_t latency_window);
  ~StabilityTracker();

  StabilityTracker(const StabilityTracker&) = delete;
  StabilityTracker& operator=(const StabilityTracker&) = delete;
  StabilityTracker(StabilityTracker&&) noexcept;
  StabilityTracker& operator=(StabilityTracker&&) noexcept;

  [[nodiscard]] std::optional<StabilityTransition> ObserveProbe(
      const ProbeObservation& observation);
  void ObserveDds(std::uint64_t generation, std::uint64_t sequence);
  void ObserveRpcParseErrors(std::uint64_t cumulative_errors);

  [[nodiscard]] StabilitySummary Summary() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view NetworkHealthStateName(
    NetworkHealthState state) noexcept;
[[nodiscard]] std::string_view NetworkHealthReasonName(
    NetworkHealthReason reason) noexcept;

}  // namespace autoruntime::stability
