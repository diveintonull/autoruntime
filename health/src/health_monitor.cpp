#include <autoruntime/health_monitor.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autoruntime {
namespace {

[[nodiscard]] bool DefaultProcessProbe(std::int64_t process_id) {
  if (process_id <= 0) {
    return false;
  }
#if defined(__unix__) || defined(__APPLE__)
  if (::kill(static_cast<pid_t>(process_id), 0) == 0) {
    return true;
  }
  return errno == EPERM;
#else
  static_cast<void>(process_id);
  return true;
#endif
}

[[nodiscard]] bool ValidPolicy(const HealthPolicy& policy) {
  return policy.heartbeat_timeout >
             std::chrono::milliseconds::zero() &&
         policy.no_progress_timeout >
             std::chrono::milliseconds::zero() &&
         policy.restart_backoff >=
             std::chrono::milliseconds::zero() &&
         policy.recovery_timeout >
             std::chrono::milliseconds::zero() &&
         policy.max_restart_attempts != 0U;
}

[[nodiscard]] bool TimedOut(Deadline::TimePoint now,
                            Deadline::TimePoint last,
                            std::chrono::milliseconds timeout) {
  return now > last && now - last > timeout;
}

}  // namespace

struct HealthMonitor::Impl {
  struct ProbeResult {
    std::int64_t process_id{0};
    std::uint64_t generation{0U};
    bool alive{true};
  };

  explicit Impl(ProcessProbe probe)
      : process_probe(probe ? std::move(probe)
                            : ProcessProbe(DefaultProcessProbe)) {}

  mutable std::mutex mutex;
  ProcessProbe process_probe;
  std::unordered_map<std::string, ComponentHealth> components;
  std::vector<HealthTransition> transitions;

  void TransitionLocked(ComponentHealth& component, HealthState state,
                        HealthReason reason, TimePoint observed_at,
                        std::string detail,
                        std::vector<HealthTransition>* emitted = nullptr) {
    if (component.state == state && component.reason == reason &&
        component.detail == detail) {
      return;
    }
    HealthTransition transition;
    transition.component = component.name;
    transition.generation = component.generation;
    transition.previous = component.state;
    transition.current = state;
    transition.reason = reason;
    transition.observed_at = observed_at;
    transition.detail = detail;

    component.state = state;
    component.reason = reason;
    component.last_transition = observed_at;
    component.detail = std::move(detail);
    transitions.push_back(transition);
    if (emitted != nullptr) {
      emitted->push_back(std::move(transition));
    }
  }

  [[nodiscard]] Status CheckGenerationLocked(
      const ComponentHealth& component,
      std::uint64_t generation) const {
    if (generation != component.generation) {
      return Status(
          StatusCode::StaleGeneration,
          generation < component.generation
              ? "component update belongs to a stale generation"
              : "component update belongs to an unregistered generation");
    }
    return Status::Ok();
  }

  [[nodiscard]] Result<ComponentHealth*> FindLocked(
      std::string_view component_name) {
    const auto iterator = components.find(std::string(component_name));
    if (iterator == components.end()) {
      return Status(StatusCode::NotFound,
                    "health component is not registered");
    }
    return &iterator->second;
  }
};

HealthMonitor::HealthMonitor(ProcessProbe process_probe)
    : impl_(std::make_unique<Impl>(std::move(process_probe))) {}

HealthMonitor::~HealthMonitor() = default;

Result<ComponentHealth> HealthMonitor::Register(
    ComponentRegistration registration, TimePoint observed_at) {
  if (registration.name.empty() || registration.process_id < 0 ||
      registration.generation == 0U ||
      !ValidPolicy(registration.policy)) {
    return Status(StatusCode::InvalidArgument,
                  "invalid health component registration");
  }

  ComponentHealth component;
  component.name = std::move(registration.name);
  component.process_id = registration.process_id;
  component.generation = registration.generation;
  component.state = HealthState::Starting;
  component.reason = HealthReason::None;
  component.last_seen = observed_at;
  component.last_progress = observed_at;
  component.last_transition = observed_at;
  component.policy = registration.policy;
  component.detail = "awaiting first heartbeat";

  std::lock_guard lock(impl_->mutex);
  if (impl_->components.contains(component.name)) {
    return Status(StatusCode::AlreadyExists,
                  "health component is already registered");
  }
  auto [iterator, inserted] =
      impl_->components.emplace(component.name, std::move(component));
  if (!inserted) {
    return Status(StatusCode::Internal,
                  "health component insertion failed");
  }
  return iterator->second;
}

Status HealthMonitor::Heartbeat(std::string_view component_name,
                                std::uint64_t generation,
                                std::uint64_t sequence,
                                TimePoint observed_at) {
  std::lock_guard lock(impl_->mutex);
  auto result = impl_->FindLocked(component_name);
  if (!result) {
    return result.status();
  }
  auto& component = *result.value();
  const auto generation_status =
      impl_->CheckGenerationLocked(component, generation);
  if (!generation_status) {
    return generation_status;
  }
  if (sequence < component.heartbeat_sequence) {
    return Status(StatusCode::InvalidArgument,
                  "heartbeat sequence regressed");
  }

  component.heartbeat_sequence = sequence;
  component.last_seen = observed_at;
  if (component.state == HealthState::Starting) {
    impl_->TransitionLocked(component, HealthState::Running,
                            HealthReason::None, observed_at,
                            "first heartbeat observed");
  }
  return Status::Ok();
}

Status HealthMonitor::Progress(std::string_view component_name,
                               std::uint64_t generation,
                               std::uint64_t sequence,
                               TimePoint observed_at) {
  std::lock_guard lock(impl_->mutex);
  auto result = impl_->FindLocked(component_name);
  if (!result) {
    return result.status();
  }
  auto& component = *result.value();
  const auto generation_status =
      impl_->CheckGenerationLocked(component, generation);
  if (!generation_status) {
    return generation_status;
  }
  if (sequence < component.progress_sequence) {
    return Status(StatusCode::InvalidArgument,
                  "progress sequence regressed");
  }
  if (sequence > component.progress_sequence) {
    component.progress_sequence = sequence;
    component.last_progress = observed_at;
  }
  return Status::Ok();
}

Status HealthMonitor::ReportBacklog(std::string_view component_name,
                                    std::uint64_t generation,
                                    std::size_t depth) {
  std::lock_guard lock(impl_->mutex);
  auto result = impl_->FindLocked(component_name);
  if (!result) {
    return result.status();
  }
  auto& component = *result.value();
  const auto generation_status =
      impl_->CheckGenerationLocked(component, generation);
  if (!generation_status) {
    return generation_status;
  }
  component.backlog = depth;
  return Status::Ok();
}

Status HealthMonitor::ReportDeadlineMisses(
    std::string_view component_name, std::uint64_t generation,
    std::uint64_t misses) {
  std::lock_guard lock(impl_->mutex);
  auto result = impl_->FindLocked(component_name);
  if (!result) {
    return result.status();
  }
  auto& component = *result.value();
  const auto generation_status =
      impl_->CheckGenerationLocked(component, generation);
  if (!generation_status) {
    return generation_status;
  }
  component.deadline_misses = misses;
  return Status::Ok();
}

std::vector<HealthTransition> HealthMonitor::Evaluate(
    TimePoint observed_at) {
  struct ProbeRequest {
    std::string name;
    std::int64_t process_id{0};
    std::uint64_t generation{0U};
  };
  std::vector<ProbeRequest> requests;
  {
    std::lock_guard lock(impl_->mutex);
    requests.reserve(impl_->components.size());
    for (const auto& [name, component] : impl_->components) {
      if (component.process_id > 0 &&
          component.state != HealthState::Failed) {
        requests.push_back(
            ProbeRequest{name, component.process_id, component.generation});
      }
    }
  }

  std::unordered_map<std::string, Impl::ProbeResult> probes;
  probes.reserve(requests.size());
  for (const auto& request : requests) {
    bool alive = false;
    try {
      alive = impl_->process_probe(request.process_id);
    } catch (...) {
      alive = false;
    }
    probes.emplace(
        request.name,
        Impl::ProbeResult{
            request.process_id, request.generation, alive});
  }

  std::vector<HealthTransition> emitted;
  std::lock_guard lock(impl_->mutex);
  for (auto& [name, component] : impl_->components) {
    if (component.state == HealthState::Failed) {
      continue;
    }

    const auto probe = probes.find(name);
    if (probe != probes.end() &&
        probe->second.process_id == component.process_id &&
        probe->second.generation == component.generation &&
        !probe->second.alive) {
      impl_->TransitionLocked(
          component, HealthState::Failed,
          HealthReason::ProcessExited, observed_at,
          "process probe reports that the component exited", &emitted);
      continue;
    }

    if (component.state == HealthState::Recovering) {
      if (TimedOut(observed_at, component.last_transition,
                   component.policy.recovery_timeout)) {
        impl_->TransitionLocked(
            component, HealthState::Failed,
            HealthReason::RecoveryFailed, observed_at,
            "recovery did not complete before its timeout", &emitted);
      }
      continue;
    }

    if (TimedOut(observed_at, component.last_seen,
                 component.policy.heartbeat_timeout)) {
      impl_->TransitionLocked(
          component, HealthState::Failed,
          HealthReason::HeartbeatLost, observed_at,
          "heartbeat timeout expired", &emitted);
      continue;
    }
    if (TimedOut(observed_at, component.last_progress,
                 component.policy.no_progress_timeout)) {
      impl_->TransitionLocked(
          component, HealthState::Failed,
          HealthReason::NoProgress, observed_at,
          "progress sequence did not advance", &emitted);
      continue;
    }

    if (component.backlog > component.policy.max_backlog) {
      impl_->TransitionLocked(
          component, HealthState::Degraded,
          HealthReason::BacklogExceeded, observed_at,
          "reported backlog exceeds policy", &emitted);
      continue;
    }
    if (component.deadline_misses >
        component.policy.max_deadline_misses) {
      impl_->TransitionLocked(
          component, HealthState::Degraded,
          HealthReason::DeadlineMissesExceeded, observed_at,
          "reported deadline misses exceed policy", &emitted);
      continue;
    }
    if (component.state == HealthState::Degraded) {
      impl_->TransitionLocked(component, HealthState::Running,
                              HealthReason::None, observed_at,
                              "degradation signals cleared", &emitted);
    }
  }
  return emitted;
}

Result<ComponentHealth> HealthMonitor::Snapshot(
    std::string_view component_name) const {
  std::lock_guard lock(impl_->mutex);
  const auto iterator =
      impl_->components.find(std::string(component_name));
  if (iterator == impl_->components.end()) {
    return Status(StatusCode::NotFound,
                  "health component is not registered");
  }
  return iterator->second;
}

std::vector<ComponentHealth> HealthMonitor::SnapshotAll() const {
  std::vector<ComponentHealth> snapshot;
  {
    std::lock_guard lock(impl_->mutex);
    snapshot.reserve(impl_->components.size());
    for (const auto& [name, component] : impl_->components) {
      static_cast<void>(name);
      snapshot.push_back(component);
    }
  }
  std::sort(snapshot.begin(), snapshot.end(),
            [](const ComponentHealth& left,
               const ComponentHealth& right) {
              return left.name < right.name;
            });
  return snapshot;
}

std::vector<HealthTransition> HealthMonitor::Transitions() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->transitions;
}

Result<ComponentHealth> HealthMonitor::Recover(
    std::string_view component_name, const RecoveryHooks& hooks,
    Deadline deadline) {
  if (!hooks.cleanup || !hooks.start || !hooks.reconnect) {
    return Status(StatusCode::InvalidArgument,
                  "recovery requires cleanup, start, and reconnect hooks");
  }

  std::string name(component_name);
  std::uint64_t old_generation = 0U;
  std::uint64_t new_generation = 0U;
  std::chrono::milliseconds backoff{0};
  std::chrono::milliseconds recovery_timeout{0};
  {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->FindLocked(name);
    if (!result) {
      return result.status();
    }
    auto& component = *result.value();
    if (component.state != HealthState::Failed) {
      return Status(StatusCode::InvalidArgument,
                    "only a failed component can be recovered");
    }
    if (component.restart_attempts >=
        component.policy.max_restart_attempts) {
      return Status(StatusCode::Closed,
                    "component exhausted its restart budget");
    }

    old_generation = component.generation;
    new_generation = old_generation + 1U;
    if (new_generation == 0U) {
      return Status(StatusCode::Internal,
                    "component generation overflowed");
    }
    backoff = component.policy.restart_backoff;
    recovery_timeout = component.policy.recovery_timeout;
    ++component.restart_attempts;
    impl_->TransitionLocked(
        component, HealthState::Recovering, component.reason,
        Clock::now(), "recovery hooks are running");
  }

  auto fail = [&](Status failure) -> Result<ComponentHealth> {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->FindLocked(name);
    if (result && result.value()->generation == old_generation) {
      impl_->TransitionLocked(
          *result.value(), HealthState::Failed,
          HealthReason::RecoveryFailed, Clock::now(), failure.detail());
    }
    return failure;
  };

  const auto recovery_started = Clock::now();
  Status cleanup_status =
      Status(StatusCode::Internal, "cleanup hook did not run");
  try {
    cleanup_status = hooks.cleanup(name, old_generation);
  } catch (const std::exception& error) {
    cleanup_status = Status(
        StatusCode::Internal,
        std::string("cleanup hook threw: ") + error.what());
  } catch (...) {
    cleanup_status =
        Status(StatusCode::Internal, "cleanup hook threw");
  }
  if (!cleanup_status) {
    return fail(cleanup_status);
  }

  if (deadline.expired() ||
      (!deadline.infinite() &&
       deadline.time_point() - Clock::now() < backoff)) {
    return fail(Status(StatusCode::Timeout,
                       "recovery deadline expires during backoff"));
  }
  std::this_thread::sleep_for(backoff);
  if (deadline.expired() ||
      Clock::now() - recovery_started > recovery_timeout) {
    return fail(Status(StatusCode::Timeout,
                       "recovery timed out before restart"));
  }

  Result<std::int64_t> started =
      Status(StatusCode::Internal, "start hook did not run");
  try {
    started = hooks.start(name, new_generation);
  } catch (const std::exception& error) {
    started = Status(
        StatusCode::Internal,
        std::string("start hook threw: ") + error.what());
  } catch (...) {
    started = Status(StatusCode::Internal, "start hook threw");
  }
  if (!started) {
    return fail(started.status());
  }
  if (started.value() <= 0) {
    return fail(Status(StatusCode::InvalidArgument,
                       "start hook returned an invalid process id"));
  }
  if (deadline.expired() ||
      Clock::now() - recovery_started > recovery_timeout) {
    try {
      static_cast<void>(hooks.cleanup(name, new_generation));
    } catch (...) {
    }
    return fail(Status(StatusCode::Timeout,
                       "recovery timed out after restart"));
  }

  Status reconnect_status =
      Status(StatusCode::Internal, "reconnect hook did not run");
  try {
    reconnect_status = hooks.reconnect(name, new_generation);
  } catch (const std::exception& error) {
    reconnect_status = Status(
        StatusCode::Internal,
        std::string("reconnect hook threw: ") + error.what());
  } catch (...) {
    reconnect_status =
        Status(StatusCode::Internal, "reconnect hook threw");
  }
  if (!reconnect_status) {
    try {
      static_cast<void>(hooks.cleanup(name, new_generation));
    } catch (...) {
    }
    return fail(reconnect_status);
  }

  std::lock_guard lock(impl_->mutex);
  auto result = impl_->FindLocked(name);
  if (!result) {
    return result.status();
  }
  auto& component = *result.value();
  if (component.generation != old_generation ||
      component.state != HealthState::Recovering) {
    return Status(StatusCode::StaleGeneration,
                  "component changed during recovery");
  }

  const auto now = Clock::now();
  component.process_id = started.value();
  component.generation = new_generation;
  component.last_seen = now;
  component.last_progress = now;
  component.heartbeat_sequence = 0U;
  component.progress_sequence = 0U;
  component.backlog = 0U;
  component.deadline_misses = 0U;
  impl_->TransitionLocked(component, HealthState::Starting,
                          HealthReason::None, now,
                          "restart complete; awaiting heartbeat");
  return component;
}

std::string_view HealthStateName(HealthState state) noexcept {
  switch (state) {
    case HealthState::Starting:
      return "STARTING";
    case HealthState::Running:
      return "RUNNING";
    case HealthState::Degraded:
      return "DEGRADED";
    case HealthState::Failed:
      return "FAILED";
    case HealthState::Recovering:
      return "RECOVERING";
  }
  return "UNKNOWN";
}

std::string_view HealthReasonName(HealthReason reason) noexcept {
  switch (reason) {
    case HealthReason::None:
      return "none";
    case HealthReason::HeartbeatLost:
      return "heartbeat_lost";
    case HealthReason::ProcessExited:
      return "process_exited";
    case HealthReason::NoProgress:
      return "no_progress";
    case HealthReason::BacklogExceeded:
      return "backlog_exceeded";
    case HealthReason::DeadlineMissesExceeded:
      return "deadline_misses_exceeded";
    case HealthReason::RecoveryFailed:
      return "recovery_failed";
  }
  return "unknown";
}

}  // namespace autoruntime
