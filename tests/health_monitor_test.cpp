#include "test_support.hpp"

#include <autoruntime/health_monitor.hpp>

#include <chrono>
#include <cstdint>
#include <csignal>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unordered_map>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

autoruntime::HealthPolicy Policy() {
  autoruntime::HealthPolicy policy;
  policy.heartbeat_timeout = 100ms;
  policy.no_progress_timeout = 200ms;
  policy.max_backlog = 4U;
  policy.max_deadline_misses = 2U;
  policy.restart_backoff = 1ms;
  policy.recovery_timeout = 1s;
  policy.max_restart_attempts = 3U;
  return policy;
}

int StateMachineAndGenerationAreExplicit() {
  std::unordered_map<std::int64_t, bool> alive{{11, true}, {22, true}};
  autoruntime::HealthMonitor monitor(
      [&](std::int64_t process_id) { return alive[process_id]; });
  const auto base = autoruntime::HealthMonitor::Clock::now();

  auto registered = monitor.Register(
      autoruntime::ComponentRegistration{
          "planning", 11, 1U, Policy()},
      base);
  CHECK(registered);
  CHECK(registered.value().state == autoruntime::HealthState::Starting);

  CHECK(monitor.Heartbeat("planning", 1U, 1U, base + 10ms));
  CHECK(monitor.Progress("planning", 1U, 1U, base + 10ms));
  auto running = monitor.Snapshot("planning");
  CHECK(running);
  CHECK(running.value().state == autoruntime::HealthState::Running);

  CHECK(monitor.ReportBacklog("planning", 1U, 5U));
  auto transitions = monitor.Evaluate(base + 20ms);
  CHECK(!transitions.empty());
  CHECK(monitor.Snapshot("planning").value().state ==
        autoruntime::HealthState::Degraded);
  CHECK(monitor.Snapshot("planning").value().reason ==
        autoruntime::HealthReason::BacklogExceeded);

  CHECK(monitor.ReportBacklog("planning", 1U, 0U));
  CHECK(monitor.ReportDeadlineMisses("planning", 1U, 0U));
  static_cast<void>(monitor.Evaluate(base + 30ms));
  CHECK(monitor.Snapshot("planning").value().state ==
        autoruntime::HealthState::Running);

  CHECK(monitor.ReportDeadlineMisses("planning", 1U, 3U));
  static_cast<void>(monitor.Evaluate(base + 40ms));
  CHECK(monitor.Snapshot("planning").value().state ==
        autoruntime::HealthState::Degraded);
  CHECK(monitor.Snapshot("planning").value().reason ==
        autoruntime::HealthReason::DeadlineMissesExceeded);

  alive[11] = false;
  static_cast<void>(monitor.Evaluate(base + 50ms));
  CHECK(monitor.Snapshot("planning").value().state ==
        autoruntime::HealthState::Failed);
  CHECK(monitor.Snapshot("planning").value().reason ==
        autoruntime::HealthReason::ProcessExited);

  bool cleaned = false;
  bool reconnected = false;
  autoruntime::RecoveryHooks hooks;
  hooks.cleanup = [&](std::string_view name, std::uint64_t generation) {
    cleaned = name == "planning" && generation == 1U;
    return autoruntime::Status::Ok();
  };
  hooks.start = [&](std::string_view name,
                    std::uint64_t generation)
      -> autoruntime::Result<std::int64_t> {
    if (name != "planning" || generation != 2U) {
      return autoruntime::Status(
          autoruntime::StatusCode::Internal, "unexpected restart arguments");
    }
    return std::int64_t{22};
  };
  hooks.reconnect = [&](std::string_view name,
                        std::uint64_t generation) {
    reconnected = name == "planning" && generation == 2U;
    return autoruntime::Status::Ok();
  };

  auto recovered = monitor.Recover(
      "planning", hooks, autoruntime::Deadline::After(1s));
  CHECK(recovered);
  CHECK(cleaned && reconnected);
  CHECK(recovered.value().state == autoruntime::HealthState::Starting);
  CHECK(recovered.value().generation == 2U);
  CHECK(recovered.value().process_id == 22);
  CHECK(recovered.value().restart_attempts == 1U);

  const auto stale =
      monitor.Heartbeat("planning", 1U, 2U,
                        autoruntime::HealthMonitor::Clock::now());
  CHECK(!stale);
  CHECK(stale.code() == autoruntime::StatusCode::StaleGeneration);
  CHECK(monitor.Heartbeat(
      "planning", 2U, 1U, autoruntime::HealthMonitor::Clock::now()));
  CHECK(monitor.Snapshot("planning").value().state ==
        autoruntime::HealthState::Running);
  return 0;
}

int TimeoutAndNoProgressAreDistinct() {
  autoruntime::HealthMonitor monitor(
      [](std::int64_t) { return true; });
  const auto base = autoruntime::HealthMonitor::Clock::now();
  CHECK(monitor.Register(
      autoruntime::ComponentRegistration{"heartbeat", 31, 1U, Policy()},
      base));
  CHECK(monitor.Heartbeat("heartbeat", 1U, 1U, base + 1ms));
  CHECK(monitor.Progress("heartbeat", 1U, 1U, base + 1ms));
  static_cast<void>(monitor.Evaluate(base + 102ms));
  auto heartbeat = monitor.Snapshot("heartbeat");
  CHECK(heartbeat);
  CHECK(heartbeat.value().state == autoruntime::HealthState::Failed);
  CHECK(heartbeat.value().reason ==
        autoruntime::HealthReason::HeartbeatLost);

  auto slow_policy = Policy();
  slow_policy.heartbeat_timeout = 1s;
  CHECK(monitor.Register(
      autoruntime::ComponentRegistration{"stalled", 32, 4U, slow_policy},
      base));
  CHECK(monitor.Heartbeat("stalled", 4U, 1U, base + 1ms));
  CHECK(monitor.Progress("stalled", 4U, 1U, base + 1ms));
  CHECK(monitor.Heartbeat("stalled", 4U, 2U, base + 250ms));
  static_cast<void>(monitor.Evaluate(base + 250ms));
  auto stalled = monitor.Snapshot("stalled");
  CHECK(stalled);
  CHECK(stalled.value().state == autoruntime::HealthState::Failed);
  CHECK(stalled.value().reason ==
        autoruntime::HealthReason::NoProgress);
  return 0;
}

struct ChildGuard {
  pid_t process{-1};

  ~ChildGuard() {
    if (process > 0) {
      static_cast<void>(::kill(process, SIGKILL));
      int status = 0;
      static_cast<void>(::waitpid(process, &status, 0));
    }
  }

  void Reaped() { process = -1; }
};

pid_t SpawnPlanning() {
  const auto child = ::fork();
  if (child == 0) {
    for (;;) {
      ::pause();
    }
  }
  return child;
}

int Kill9PlanningRecoversWithNewGeneration() {
  ChildGuard child{SpawnPlanning()};
  CHECK(child.process > 0);

  autoruntime::HealthMonitor monitor;
  const auto now = autoruntime::HealthMonitor::Clock::now();
  CHECK(monitor.Register(
      autoruntime::ComponentRegistration{
          "planning-process", child.process, 7U, Policy()},
      now));
  CHECK(monitor.Heartbeat("planning-process", 7U, 1U, now));
  CHECK(monitor.Progress("planning-process", 7U, 1U, now));

  CHECK(::kill(child.process, SIGKILL) == 0);
  int status = 0;
  CHECK(::waitpid(child.process, &status, 0) == child.process);
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGKILL);
  child.Reaped();

  static_cast<void>(
      monitor.Evaluate(autoruntime::HealthMonitor::Clock::now()));
  auto failed = monitor.Snapshot("planning-process");
  CHECK(failed);
  CHECK(failed.value().state == autoruntime::HealthState::Failed);
  CHECK(failed.value().reason ==
        autoruntime::HealthReason::ProcessExited);

  bool cleaned = false;
  bool reconnected = false;
  autoruntime::RecoveryHooks hooks;
  hooks.cleanup = [&](std::string_view, std::uint64_t old_generation) {
    cleaned = old_generation == 7U;
    return autoruntime::Status::Ok();
  };
  hooks.start = [&](std::string_view,
                    std::uint64_t new_generation)
      -> autoruntime::Result<std::int64_t> {
    if (new_generation != 8U) {
      return autoruntime::Status(
          autoruntime::StatusCode::Internal, "generation did not advance");
    }
    child.process = SpawnPlanning();
    if (child.process <= 0) {
      return autoruntime::Status(
          autoruntime::StatusCode::Internal, "fork failed");
    }
    return static_cast<std::int64_t>(child.process);
  };
  hooks.reconnect = [&](std::string_view, std::uint64_t generation) {
    reconnected = generation == 8U;
    return autoruntime::Status::Ok();
  };

  auto recovered = monitor.Recover(
      "planning-process", hooks, autoruntime::Deadline::After(2s));
  CHECK(recovered);
  CHECK(cleaned && reconnected);
  CHECK(recovered.value().generation == 8U);
  CHECK(recovered.value().process_id == child.process);
  CHECK(monitor.Heartbeat(
      "planning-process", 8U, 1U,
      autoruntime::HealthMonitor::Clock::now()));
  CHECK(monitor.Progress(
      "planning-process", 8U, 1U,
      autoruntime::HealthMonitor::Clock::now()));
  CHECK(monitor.Snapshot("planning-process").value().state ==
        autoruntime::HealthState::Running);
  return 0;
}

}  // namespace

int main() {
  if (const int result = StateMachineAndGenerationAreExplicit();
      result != 0) {
    return result;
  }
  if (const int result = TimeoutAndNoProgressAreDistinct();
      result != 0) {
    return result;
  }
  return Kill9PlanningRecoversWithNewGeneration();
}
