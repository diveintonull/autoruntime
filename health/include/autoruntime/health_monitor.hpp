#pragma once

#include <autoruntime/deadline.hpp>
#include <autoruntime/status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace autoruntime {

enum class HealthState {
  Starting,
  Running,
  Degraded,
  Failed,
  Recovering,
};

enum class HealthReason {
  None,
  HeartbeatLost,
  ProcessExited,
  NoProgress,
  BacklogExceeded,
  DeadlineMissesExceeded,
  RecoveryFailed,
};

struct HealthPolicy {
  std::chrono::milliseconds heartbeat_timeout{500};
  std::chrono::milliseconds no_progress_timeout{2000};
  std::size_t max_backlog{1024U};
  std::uint64_t max_deadline_misses{0U};
  std::chrono::milliseconds restart_backoff{100};
  std::chrono::milliseconds recovery_timeout{5000};
  std::uint32_t max_restart_attempts{3U};
};

struct ComponentRegistration {
  std::string name;
  std::int64_t process_id{0};
  std::uint64_t generation{1U};
  HealthPolicy policy;
};

struct ComponentHealth {
  std::string name;
  std::int64_t process_id{0};
  std::uint64_t generation{0U};
  HealthState state{HealthState::Starting};
  HealthReason reason{HealthReason::None};
  Deadline::TimePoint last_seen{};
  Deadline::TimePoint last_progress{};
  Deadline::TimePoint last_transition{};
  std::uint64_t heartbeat_sequence{0U};
  std::uint64_t progress_sequence{0U};
  std::size_t backlog{0U};
  std::uint64_t deadline_misses{0U};
  std::uint32_t restart_attempts{0U};
  HealthPolicy policy;
  std::string detail;
};

struct HealthTransition {
  std::string component;
  std::uint64_t generation{0U};
  HealthState previous{HealthState::Starting};
  HealthState current{HealthState::Starting};
  HealthReason reason{HealthReason::None};
  Deadline::TimePoint observed_at{};
  std::string detail;
};

using ProcessProbe = std::function<bool(std::int64_t)>;

struct RecoveryHooks {
  std::function<Status(std::string_view, std::uint64_t)> cleanup;
  std::function<Result<std::int64_t>(std::string_view, std::uint64_t)> start;
  std::function<Status(std::string_view, std::uint64_t)> reconnect;
};

class HealthMonitor {
 public:
  using Clock = Deadline::Clock;
  using TimePoint = Deadline::TimePoint;

  explicit HealthMonitor(ProcessProbe process_probe = {});
  ~HealthMonitor();

  HealthMonitor(const HealthMonitor&) = delete;
  HealthMonitor& operator=(const HealthMonitor&) = delete;
  HealthMonitor(HealthMonitor&&) = delete;
  HealthMonitor& operator=(HealthMonitor&&) = delete;

  [[nodiscard]] Result<ComponentHealth> Register(
      ComponentRegistration registration,
      TimePoint observed_at = Clock::now());
  Status Heartbeat(std::string_view component, std::uint64_t generation,
                   std::uint64_t sequence,
                   TimePoint observed_at = Clock::now());
  Status Progress(std::string_view component, std::uint64_t generation,
                  std::uint64_t sequence,
                  TimePoint observed_at = Clock::now());
  Status ReportBacklog(std::string_view component,
                       std::uint64_t generation, std::size_t depth);
  Status ReportDeadlineMisses(std::string_view component,
                              std::uint64_t generation,
                              std::uint64_t misses);

  [[nodiscard]] std::vector<HealthTransition> Evaluate(
      TimePoint observed_at = Clock::now());
  [[nodiscard]] Result<ComponentHealth> Snapshot(
      std::string_view component) const;
  [[nodiscard]] std::vector<ComponentHealth> SnapshotAll() const;
  [[nodiscard]] std::vector<HealthTransition> Transitions() const;

  [[nodiscard]] Result<ComponentHealth> Recover(
      std::string_view component, const RecoveryHooks& hooks,
      Deadline deadline);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view HealthStateName(HealthState state) noexcept;
[[nodiscard]] std::string_view HealthReasonName(HealthReason reason) noexcept;

}  // namespace autoruntime
