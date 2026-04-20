#pragma once

#include <autoruntime/deadline.hpp>
#include <autoruntime/status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace autoruntime {

using CallbackGroupId = std::uint64_t;
using TaskId = std::uint64_t;

enum class TaskKind {
  Periodic,
  Event,
  Async,
};

enum class ThreadSchedulingPolicy {
  Default,
  Fifo,
};

struct ThreadSchedulingConfig {
  std::vector<int> cpu_affinity;
  ThreadSchedulingPolicy policy{ThreadSchedulingPolicy::Default};
  int priority{0};
};

struct ThreadSchedulingStatus {
  std::string role;
  std::size_t worker_index{0U};
  std::uint64_t native_thread_id{0U};
  ThreadSchedulingConfig requested;
  std::vector<int> effective_cpu_affinity;
  ThreadSchedulingPolicy effective_policy{ThreadSchedulingPolicy::Default};
  int effective_priority{0};
  bool affinity_applied{false};
  bool scheduler_applied{false};
  bool affinity_fallback{false};
  bool scheduler_fallback{false};
  int affinity_error{0};
  int scheduler_error{0};
  std::string detail;
};

struct SchedulingSnapshot {
  std::vector<ThreadSchedulingStatus> threads;
  bool any_fallback{false};
  bool priority_inheritance_requested{false};
  bool priority_inheritance_active{false};
  int priority_inheritance_error{0};
};

enum class DeadlinePolicy {
  Warn,
  Degrade,
  Drop,
};

enum class DeadlineAction {
  None,
  Warned,
  Degraded,
  Dropped,
};

struct CallbackGroupConfig {
  CallbackGroupConfig() = default;
  CallbackGroupConfig(
      std::string configured_name,
      std::size_t configured_worker_count,
      std::size_t configured_queue_capacity,
      ThreadSchedulingConfig configured_scheduling = {})
      : name(std::move(configured_name)),
        worker_count(configured_worker_count),
        queue_capacity(configured_queue_capacity),
        thread_scheduling(std::move(configured_scheduling)) {}

  std::string name;
  std::size_t worker_count{1U};
  std::size_t queue_capacity{256U};
  ThreadSchedulingConfig thread_scheduling;
};

struct TaskConfig {
  std::string name;
  TaskKind kind{TaskKind::Async};
  int priority{0};
  std::chrono::nanoseconds period{0};
  std::chrono::nanoseconds deadline{0};
  std::size_t queue_capacity{1U};
  CallbackGroupId callback_group{0U};
  DeadlinePolicy deadline_policy{DeadlinePolicy::Warn};
};

struct TaskSample {
  TaskId task_id{0U};
  std::uint64_t release_sequence{0U};
  Deadline::TimePoint scheduled_release_time{};
  Deadline::TimePoint actual_start_time{};
  Deadline::TimePoint finish_time{};
  std::chrono::nanoseconds execution_time{0};
  std::chrono::nanoseconds release_jitter{0};
  std::chrono::nanoseconds response_time{0};
  std::chrono::nanoseconds queue_delay{0};
  bool deadline_miss{false};
  bool dropped{false};
  DeadlineAction deadline_action{DeadlineAction::None};
};

struct DeadlineEvent {
  TaskId task_id{0U};
  std::string task_name;
  DeadlinePolicy policy{DeadlinePolicy::Warn};
  DeadlineAction action{DeadlineAction::None};
  std::uint64_t total_deadline_misses{0U};
  TaskSample sample;
};

using DeadlineEventHandler = std::function<void(const DeadlineEvent&)>;

struct ExecutorConfig {
  ThreadSchedulingConfig scheduler_thread;
  bool priority_inheritance{true};
  DeadlineEventHandler deadline_event_handler;
};

struct TimingDistribution {
  std::uint64_t sampled_count{0U};
  std::chrono::nanoseconds minimum{0};
  std::chrono::nanoseconds p50{0};
  std::chrono::nanoseconds p95{0};
  std::chrono::nanoseconds p99{0};
  std::chrono::nanoseconds p99_9{0};
  std::chrono::nanoseconds maximum{0};
};

struct TaskTimingSummary {
  TimingDistribution release_jitter;
  TimingDistribution execution_time;
  TimingDistribution response_time;
  std::uint64_t deadline_observations{0U};
  double deadline_miss_rate{0.0};
};

struct TaskStats {
  TaskId task_id{0U};
  std::string name;
  TaskKind kind{TaskKind::Async};
  std::uint64_t releases{0U};
  std::uint64_t started{0U};
  std::uint64_t finished{0U};
  std::uint64_t queue_overflows{0U};
  std::uint64_t periodic_releases_skipped{0U};
  std::uint64_t deadline_misses{0U};
  std::uint64_t deadline_warnings{0U};
  std::uint64_t deadline_degrades{0U};
  std::uint64_t deadline_drops{0U};
  std::uint64_t deadline_handler_failures{0U};
  std::uint64_t callback_failures{0U};
  bool cancelled{false};
  TaskTimingSummary timing;
  std::vector<TaskSample> samples;
};

struct RuntimeStats {
  Deadline::TimePoint captured_at{};
  bool degraded{false};
  std::vector<TaskStats> tasks;
};

using TaskCallback = std::function<void(std::stop_token)>;

[[nodiscard]] std::vector<int> AvailableCpuIds();
[[nodiscard]] Status ValidateThreadSchedulingConfig(
    const ThreadSchedulingConfig& config);
[[nodiscard]] ThreadSchedulingStatus ApplyCurrentThreadScheduling(
    std::string role, std::size_t worker_index,
    const ThreadSchedulingConfig& config);
[[nodiscard]] std::string_view ThreadSchedulingPolicyName(
    ThreadSchedulingPolicy policy) noexcept;
[[nodiscard]] std::string_view DeadlinePolicyName(
    DeadlinePolicy policy) noexcept;
[[nodiscard]] std::string_view DeadlineActionName(
    DeadlineAction action) noexcept;

class Executor {
 public:
  explicit Executor(ExecutorConfig config = {});
  ~Executor();

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;
  Executor(Executor&&) = delete;
  Executor& operator=(Executor&&) = delete;

  [[nodiscard]] Result<CallbackGroupId> CreateCallbackGroup(
      CallbackGroupConfig config);
  [[nodiscard]] Result<TaskId> AddTask(TaskConfig config,
                                       TaskCallback callback);

  Status Start();
  Status Notify(TaskId task_id);
  Status Cancel(TaskId task_id);
  [[nodiscard]] Result<TaskStats> Stats(TaskId task_id) const;
  [[nodiscard]] RuntimeStats Snapshot() const;
  [[nodiscard]] SchedulingSnapshot Scheduling() const;
  Status AcknowledgeDegraded();
  Status Stop(Deadline deadline = Deadline::Infinite());
  [[nodiscard]] bool running() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime
