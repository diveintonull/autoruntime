#include "test_support.hpp"
#include <autoruntime/executor.hpp>
#include <autoruntime/health_monitor.hpp>
#include <autoruntime/realtime_mutex.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

using namespace std::chrono_literals;

namespace {

autoruntime::TaskConfig EventConfig(std::string name,
                                    autoruntime::CallbackGroupId group,
                                    int priority,
                                    std::size_t capacity = 8U) {
  autoruntime::TaskConfig config;
  config.name = std::move(name);
  config.kind = autoruntime::TaskKind::Event;
  config.priority = priority;
  config.deadline = 100ms;
  config.queue_capacity = capacity;
  config.callback_group = group;
  return config;
}

int PriorityAndTimingMetricsAreObservable() {
  autoruntime::Executor executor;
  auto group_result = executor.CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"priority", 1U, 16U});
  CHECK(group_result);
  const auto group = std::move(group_result).take_value();

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_entered = false;
  bool release_gate = false;
  std::mutex order_mutex;
  std::vector<int> order;

  auto gate = executor.AddTask(
      EventConfig("gate", group, 100),
      [&](std::stop_token) {
        std::unique_lock lock(gate_mutex);
        gate_entered = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_gate; });
      });
  auto low = executor.AddTask(
      EventConfig("low", group, 1),
      [&](std::stop_token) {
        std::lock_guard lock(order_mutex);
        order.push_back(1);
      });
  auto high = executor.AddTask(
      EventConfig("high", group, 50),
      [&](std::stop_token) {
        std::lock_guard lock(order_mutex);
        order.push_back(50);
      });
  CHECK(gate && low && high);
  CHECK(executor.Start());
  CHECK(executor.Notify(gate.value()));
  {
    std::unique_lock lock(gate_mutex);
    CHECK(gate_cv.wait_for(lock, 1s, [&] { return gate_entered; }));
  }
  CHECK(executor.Notify(low.value()));
  CHECK(executor.Notify(high.value()));
  {
    std::lock_guard lock(gate_mutex);
    release_gate = true;
  }
  gate_cv.notify_all();
  CHECK(WaitUntil([&] {
    std::lock_guard lock(order_mutex);
    return order.size() == 2U;
  }, 1s));
  {
    std::lock_guard lock(order_mutex);
    CHECK(order == std::vector<int>({50, 1}));
  }

  const auto result = executor.Stats(high.value());
  CHECK(result);
  const auto& stats = result.value();
  CHECK(stats.releases == 1U);
  CHECK(stats.started == 1U);
  CHECK(stats.finished == 1U);
  CHECK(stats.samples.size() == 1U);
  CHECK(stats.samples.front().actual_start_time >=
        stats.samples.front().scheduled_release_time);
  CHECK(stats.samples.front().finish_time >=
        stats.samples.front().actual_start_time);
  CHECK(stats.samples.front().execution_time >= 0ns);
  CHECK(stats.samples.front().release_jitter >= 0ns);
  CHECK(stats.samples.front().response_time >=
        stats.samples.front().execution_time);
  CHECK(stats.samples.front().queue_delay >= 0ns);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int PeriodicTasksHaveDeadlinesAndCancellation() {
  autoruntime::Executor executor;
  auto group = executor.CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"periodic", 1U, 32U});
  CHECK(group);
  autoruntime::TaskConfig config;
  config.name = "periodic-overrun";
  config.kind = autoruntime::TaskKind::Periodic;
  config.priority = 10;
  config.period = 20ms;
  config.deadline = 10ms;
  config.queue_capacity = 2U;
  config.callback_group = group.value();

  std::atomic<std::uint64_t> calls{0U};
  auto task = executor.AddTask(config, [&](std::stop_token stop_token) {
    calls.fetch_add(1U, std::memory_order_relaxed);
    for (int i = 0; i < 15 && !stop_token.stop_requested(); ++i) {
      std::this_thread::sleep_for(1ms);
    }
  });
  CHECK(task);
  CHECK(executor.Start());
  CHECK(WaitUntil(
      [&] { return calls.load(std::memory_order_relaxed) >= 3U; }, 500ms));
  CHECK(executor.Cancel(task.value()));
  const auto stopped_at = calls.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(60ms);
  CHECK(calls.load(std::memory_order_relaxed) <= stopped_at + 1U);
  auto stats = executor.Stats(task.value());
  CHECK(stats);
  CHECK(stats.value().releases >= 3U);
  CHECK(stats.value().deadline_misses >= 1U);
  CHECK(stats.value().cancelled);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int BoundedEventQueueReportsOverflow() {
  autoruntime::Executor executor;
  auto group = executor.CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"bounded", 1U, 8U});
  CHECK(group);
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  auto task = executor.AddTask(
      EventConfig("bounded-event", group.value(), 1, 2U),
      [&](std::stop_token) {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
      });
  CHECK(task);
  CHECK(executor.Start());
  CHECK(executor.Notify(task.value()));
  {
    std::unique_lock lock(mutex);
    CHECK(condition.wait_for(lock, 1s, [&] { return entered; }));
  }
  CHECK(executor.Notify(task.value()));
  CHECK(executor.Notify(task.value()));
  const auto overflow = executor.Notify(task.value());
  CHECK(overflow.code() == autoruntime::StatusCode::QueueFull);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  CHECK(WaitUntil([&] {
    auto stats = executor.Stats(task.value());
    return stats && stats.value().finished >= 3U;
  }, 1s));
  const auto stats = executor.Stats(task.value());
  CHECK(stats);
  CHECK(stats.value().queue_overflows == 1U);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int SlowPlanningCallbackDoesNotBlockCriticalGroup() {
  autoruntime::Executor executor;
  auto planning = executor.CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"planning", 1U, 8U});
  auto critical = executor.CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"critical", 1U, 64U});
  CHECK(planning && critical);
  std::atomic<bool> entered{false};
  auto slow = executor.AddTask(
      EventConfig("slow-planning", planning.value(), 1),
      [&](std::stop_token) {
        entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(300ms);
      });
  CHECK(slow);
  autoruntime::TaskConfig config;
  config.name = "control-loop";
  config.kind = autoruntime::TaskKind::Periodic;
  config.priority = 100;
  config.period = 20ms;
  config.deadline = 15ms;
  config.queue_capacity = 2U;
  config.callback_group = critical.value();
  std::atomic<std::uint64_t> calls{0U};
  auto control = executor.AddTask(config, [&](std::stop_token) {
    calls.fetch_add(1U, std::memory_order_relaxed);
  });
  CHECK(control);
  CHECK(executor.Start());
  CHECK(executor.Notify(slow.value()));
  CHECK(WaitUntil(
      [&] { return entered.load(std::memory_order_acquire); }, 200ms));
  std::this_thread::sleep_for(180ms);
  CHECK(calls.load(std::memory_order_relaxed) >= 7U);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int ThreadAffinityAndFifoFallbackAreObservable() {
  const auto cpus = autoruntime::AvailableCpuIds();
#if defined(__linux__)
  CHECK(!cpus.empty());
#endif
  if (cpus.empty()) {
    return 0;
  }
  const int cpu = cpus.front();

  autoruntime::ThreadSchedulingConfig duplicate_cpu;
  duplicate_cpu.cpu_affinity = {cpu, cpu};
  CHECK(!autoruntime::ValidateThreadSchedulingConfig(duplicate_cpu));

  autoruntime::ThreadSchedulingConfig invalid_default_priority;
  invalid_default_priority.priority = 1;
  CHECK(!autoruntime::ValidateThreadSchedulingConfig(
      invalid_default_priority));

#if defined(__linux__)
  autoruntime::ThreadSchedulingConfig invalid_cpu;
  invalid_cpu.cpu_affinity = {CPU_SETSIZE};
  CHECK(!autoruntime::ValidateThreadSchedulingConfig(invalid_cpu));
#endif

  autoruntime::Executor invalid_group_executor;
  autoruntime::CallbackGroupConfig invalid_group;
  invalid_group.name = "invalid-scheduling";
  invalid_group.thread_scheduling = invalid_default_priority;
  CHECK(!invalid_group_executor.CreateCallbackGroup(
      std::move(invalid_group)));

  autoruntime::ExecutorConfig executor_config;
  executor_config.scheduler_thread.cpu_affinity = {cpu};
  autoruntime::Executor executor(std::move(executor_config));

  autoruntime::CallbackGroupConfig group_config;
  group_config.name = "critical";
  group_config.worker_count = 1U;
  group_config.queue_capacity = 8U;
  group_config.thread_scheduling.cpu_affinity = {cpu};
  group_config.thread_scheduling.policy =
      autoruntime::ThreadSchedulingPolicy::Fifo;
  group_config.thread_scheduling.priority = 1;
  auto group = executor.CreateCallbackGroup(std::move(group_config));
  CHECK(group);

  std::atomic<int> observed_cpu{-1};
  auto task = executor.AddTask(
      EventConfig("affinity-probe", group.value(), 1),
      [&](std::stop_token) {
#if defined(__linux__)
        observed_cpu.store(::sched_getcpu(), std::memory_order_release);
#else
        observed_cpu.store(cpu, std::memory_order_release);
#endif
      });
  CHECK(task);
  CHECK(executor.Start());
  CHECK(executor.Notify(task.value()));
  CHECK(WaitUntil(
      [&] { return observed_cpu.load(std::memory_order_acquire) >= 0; },
      1s));
#if defined(__linux__)
  CHECK(observed_cpu.load(std::memory_order_acquire) == cpu);
#endif

  const auto scheduling = executor.Scheduling();
  CHECK(scheduling.threads.size() == 2U);
  const auto worker = std::find_if(
      scheduling.threads.begin(), scheduling.threads.end(),
      [](const autoruntime::ThreadSchedulingStatus& status) {
        return status.role == "critical";
      });
  CHECK(worker != scheduling.threads.end());
  CHECK(worker->affinity_applied);
  CHECK(std::find(worker->effective_cpu_affinity.begin(),
                  worker->effective_cpu_affinity.end(),
                  cpu) != worker->effective_cpu_affinity.end());
#if defined(__linux__)
  CHECK((worker->scheduler_applied &&
         worker->effective_policy ==
             autoruntime::ThreadSchedulingPolicy::Fifo) ||
        (worker->scheduler_fallback &&
         worker->scheduler_error != 0));
#endif
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int DeadlinePoliciesDropAndDegrade() {
  autoruntime::HealthMonitor health(
      [](std::int64_t) { return true; });
  autoruntime::HealthPolicy health_policy;
  health_policy.heartbeat_timeout = 1s;
  health_policy.no_progress_timeout = 1s;
  health_policy.max_deadline_misses = 0U;
  const auto health_base = autoruntime::HealthMonitor::Clock::now();
  CHECK(health.Register(
      autoruntime::ComponentRegistration{
          "control", 1, 1U, health_policy},
      health_base));
  CHECK(health.Heartbeat("control", 1U, 1U, health_base));
  CHECK(health.Progress("control", 1U, 1U, health_base));

  std::atomic<std::uint64_t> events{0U};
  std::atomic<bool> health_reported{false};
  autoruntime::ExecutorConfig executor_config;
  executor_config.deadline_event_handler =
      [&](const autoruntime::DeadlineEvent& event) {
        events.fetch_add(1U, std::memory_order_relaxed);
        if (event.action == autoruntime::DeadlineAction::Degraded) {
          const auto reported = health.ReportDeadlineMisses(
              "control", 1U, event.total_deadline_misses);
          if (reported) {
            static_cast<void>(
                health.Evaluate(
                    autoruntime::HealthMonitor::Clock::now()));
            health_reported.store(true, std::memory_order_release);
          }
          throw std::runtime_error("test handler failure");
        }
      };
  autoruntime::Executor executor(std::move(executor_config));
  auto group = executor.CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"deadline", 1U, 16U});
  CHECK(group);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool gate_entered = false;
  bool release_gate = false;
  auto gate = executor.AddTask(
      EventConfig("deadline-gate", group.value(), 100),
      [&](std::stop_token) {
        std::unique_lock lock(gate_mutex);
        gate_entered = true;
        gate_condition.notify_all();
        gate_condition.wait(lock, [&] { return release_gate; });
      });
  CHECK(gate);

  auto drop_config = EventConfig("drop-stale", group.value(), 10);
  drop_config.deadline = 5ms;
  drop_config.deadline_policy = autoruntime::DeadlinePolicy::Drop;
  std::atomic<std::uint64_t> drop_calls{0U};
  auto drop = executor.AddTask(
      drop_config,
      [&](std::stop_token) {
        drop_calls.fetch_add(1U, std::memory_order_relaxed);
      });
  CHECK(drop);

  auto degrade_config = EventConfig("degrade", group.value(), 20);
  degrade_config.deadline = 1ms;
  degrade_config.deadline_policy =
      autoruntime::DeadlinePolicy::Degrade;
  auto degrade = executor.AddTask(
      degrade_config,
      [&](std::stop_token) { std::this_thread::sleep_for(5ms); });
  CHECK(degrade);

  CHECK(executor.Start());
  CHECK(executor.Notify(gate.value()));
  {
    std::unique_lock lock(gate_mutex);
    CHECK(gate_condition.wait_for(
        lock, 1s, [&] { return gate_entered; }));
  }
  CHECK(executor.Notify(drop.value()));
  std::this_thread::sleep_for(20ms);
  {
    std::lock_guard lock(gate_mutex);
    release_gate = true;
  }
  gate_condition.notify_all();
  CHECK(WaitUntil(
      [&] {
        const auto stats = executor.Stats(drop.value());
        return stats && stats.value().deadline_drops == 1U;
      },
      1s));
  CHECK(drop_calls.load(std::memory_order_relaxed) == 0U);
  auto drop_stats = executor.Stats(drop.value());
  CHECK(drop_stats);
  CHECK(drop_stats.value().started == 0U);
  CHECK(drop_stats.value().finished == 0U);
  CHECK(drop_stats.value().deadline_misses == 1U);
  CHECK(drop_stats.value().samples.size() == 1U);
  CHECK(drop_stats.value().samples.front().dropped);
  CHECK(drop_stats.value().samples.front().deadline_action ==
        autoruntime::DeadlineAction::Dropped);

  CHECK(executor.Notify(degrade.value()));
  CHECK(WaitUntil(
      [&] {
        const auto stats = executor.Stats(degrade.value());
        return stats && stats.value().finished == 1U;
      },
      1s));
  CHECK(WaitUntil(
      [&] {
        const auto stats = executor.Stats(degrade.value());
        return stats &&
               stats.value().deadline_handler_failures == 1U;
      },
      1s));
  const auto degrade_stats = executor.Stats(degrade.value());
  CHECK(degrade_stats);
  CHECK(degrade_stats.value().deadline_degrades == 1U);
  CHECK(degrade_stats.value().deadline_handler_failures == 1U);
  CHECK(executor.Snapshot().degraded);
  CHECK(health_reported.load(std::memory_order_acquire));
  CHECK(health.Snapshot("control").value().state ==
        autoruntime::HealthState::Degraded);
  CHECK(health.Snapshot("control").value().reason ==
        autoruntime::HealthReason::DeadlineMissesExceeded);
  CHECK(events.load(std::memory_order_relaxed) == 2U);
  CHECK(executor.AcknowledgeDegraded());
  CHECK(!executor.Snapshot().degraded);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int PeriodicReleaseGridAndTailSummaryAreObservable() {
  autoruntime::Executor executor;
  auto group = executor.CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"grid", 1U, 64U});
  CHECK(group);
  autoruntime::TaskConfig config;
  config.name = "absolute-grid";
  config.kind = autoruntime::TaskKind::Periodic;
  config.priority = 1;
  config.period = 5ms;
  config.deadline = 100ms;
  config.queue_capacity = 8U;
  config.callback_group = group.value();

  std::atomic<std::uint64_t> calls{0U};
  auto task = executor.AddTask(
      config,
      [&](std::stop_token) {
        calls.fetch_add(1U, std::memory_order_relaxed);
      });
  CHECK(task);
  CHECK(executor.Start());
  CHECK(WaitUntil(
      [&] { return calls.load(std::memory_order_relaxed) >= 24U; },
      1s));
  CHECK(executor.Cancel(task.value()));
  const auto stats = executor.Stats(task.value());
  CHECK(stats);
  CHECK(stats.value().samples.size() >= 20U);
  for (std::size_t index = 1U;
       index < stats.value().samples.size(); ++index) {
    CHECK(stats.value().samples[index].scheduled_release_time -
              stats.value().samples[index - 1U].scheduled_release_time ==
          5ms);
  }
  for (const auto& sample : stats.value().samples) {
    CHECK(sample.release_jitter ==
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              sample.actual_start_time -
              sample.scheduled_release_time));
  }
  const auto& summary = stats.value().timing.release_jitter;
  CHECK(summary.sampled_count == stats.value().samples.size());
  CHECK(summary.maximum >= summary.p99_9);
  CHECK(summary.p99_9 >= summary.p99);
  CHECK(stats.value().timing.deadline_miss_rate == 0.0);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int PriorityInheritanceMutexReportsProtocol() {
  autoruntime::RealtimeMutex inherited(
      autoruntime::MutexProtocol::PriorityInheritance);
  {
    std::lock_guard lock(inherited);
    CHECK(!inherited.try_lock());
  }
#if defined(__linux__)
  CHECK(inherited.priority_inheritance_active() ||
        inherited.priority_inheritance_error() != 0);
#endif

  autoruntime::RealtimeMutex normal(
      autoruntime::MutexProtocol::Default);
  CHECK(!normal.priority_inheritance_requested());
  CHECK(!normal.priority_inheritance_active());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--case") {
    const std::string_view selected(argv[2]);
    if (selected == "thread_scheduling") {
      return ThreadAffinityAndFifoFallbackAreObservable();
    }
    if (selected == "deadline_policy") {
      return DeadlinePoliciesDropAndDegrade();
    }
    if (selected == "absolute_release") {
      return PeriodicReleaseGridAndTailSummaryAreObservable();
    }
    if (selected == "priority_inheritance") {
      return PriorityInheritanceMutexReportsProtocol();
    }
    return 2;
  }
  if (argc != 1) {
    return 2;
  }
  if (ThreadAffinityAndFifoFallbackAreObservable() != 0) return 1;
  if (DeadlinePoliciesDropAndDegrade() != 0) return 1;
  if (PeriodicReleaseGridAndTailSummaryAreObservable() != 0) return 1;
  if (PriorityInheritanceMutexReportsProtocol() != 0) return 1;
  if (PriorityAndTimingMetricsAreObservable() != 0) return 1;
  if (PeriodicTasksHaveDeadlinesAndCancellation() != 0) return 1;
  if (BoundedEventQueueReportsOverflow() != 0) return 1;
  return SlowPlanningCallbackDoesNotBlockCriticalGroup();
}
