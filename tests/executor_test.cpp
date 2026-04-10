#include "test_support.hpp"
#include <autoruntime/executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  CHECK(stats.samples.front().start_time >= stats.samples.front().release_time);
  CHECK(stats.samples.front().finish_time >= stats.samples.front().start_time);
  CHECK(stats.samples.front().execution_time >= 0ns);
  CHECK(stats.samples.front().release_lateness >= 0ns);
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
}  // namespace

int main() {
  if (PriorityAndTimingMetricsAreObservable() != 0) return 1;
  if (PeriodicTasksHaveDeadlinesAndCancellation() != 0) return 1;
  if (BoundedEventQueueReportsOverflow() != 0) return 1;
  return SlowPlanningCallbackDoesNotBlockCriticalGroup();
}
