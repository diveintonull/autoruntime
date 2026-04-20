#include <autoruntime/executor.hpp>

#include <autoruntime/realtime_mutex.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <queue>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace autoruntime {
namespace {

using Clock = Deadline::Clock;
using TimePoint = Deadline::TimePoint;

[[nodiscard]] std::chrono::nanoseconds NonNegativeDuration(
    TimePoint end, TimePoint begin) {
  if (end <= begin) {
    return std::chrono::nanoseconds::zero();
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin);
}

[[nodiscard]] TimingDistribution SummarizeDurations(
    std::vector<std::chrono::nanoseconds> values) {
  TimingDistribution result;
  if (values.empty()) {
    return result;
  }
  std::sort(values.begin(), values.end());
  result.sampled_count =
      static_cast<std::uint64_t>(values.size());
  result.minimum = values.front();
  result.maximum = values.back();
  const auto percentile =
      [&](double quantile) -> std::chrono::nanoseconds {
    const auto rank = static_cast<std::size_t>(
        std::ceil(quantile * static_cast<double>(values.size())));
    const auto index = std::min(
        values.size() - 1U, rank == 0U ? 0U : rank - 1U);
    return values[index];
  };
  result.p50 = percentile(0.50);
  result.p95 = percentile(0.95);
  result.p99 = percentile(0.99);
  result.p99_9 = percentile(0.999);
  return result;
}

void FinalizeTimingSummary(TaskStats& stats) {
  std::vector<std::chrono::nanoseconds> release_jitter;
  std::vector<std::chrono::nanoseconds> execution_time;
  std::vector<std::chrono::nanoseconds> response_time;
  release_jitter.reserve(stats.samples.size());
  execution_time.reserve(stats.samples.size());
  response_time.reserve(stats.samples.size());
  for (const auto& sample : stats.samples) {
    release_jitter.push_back(sample.release_jitter);
    response_time.push_back(sample.response_time);
    if (!sample.dropped) {
      execution_time.push_back(sample.execution_time);
    }
  }
  stats.timing.release_jitter =
      SummarizeDurations(std::move(release_jitter));
  stats.timing.execution_time =
      SummarizeDurations(std::move(execution_time));
  stats.timing.response_time =
      SummarizeDurations(std::move(response_time));
  stats.timing.deadline_observations =
      stats.finished + stats.deadline_drops;
  if (stats.timing.deadline_observations == 0U) {
    stats.timing.deadline_miss_rate = 0.0;
  } else {
    stats.timing.deadline_miss_rate =
        static_cast<double>(stats.deadline_misses) /
        static_cast<double>(stats.timing.deadline_observations);
  }
}

void AppendSample(TaskStats& stats, TaskSample sample) {
  constexpr std::size_t maximum_samples = 4096U;
  if (stats.samples.size() == maximum_samples) {
    stats.samples.erase(stats.samples.begin());
  }
  stats.samples.push_back(std::move(sample));
}

[[nodiscard]] MutexProtocol ProtocolFor(bool enabled) noexcept {
  return enabled ? MutexProtocol::PriorityInheritance
                 : MutexProtocol::Default;
}

struct TaskState {
  explicit TaskState(bool priority_inheritance)
      : mutex(ProtocolFor(priority_inheritance)) {}

  mutable RealtimeMutex mutex;
  TaskId id{0U};
  TaskConfig config;
  TaskCallback callback;
  std::stop_source stop_source;
  bool cancelled{false};
  std::size_t queued{0U};
  TimePoint next_release{};
  TaskStats stats;
};

struct Job {
  std::shared_ptr<TaskState> task;
  TimePoint release_time{};
  TimePoint enqueued_at{};
  std::uint64_t sequence{0U};
};

struct HigherPriorityFirst {
  bool operator()(const Job& left, const Job& right) const noexcept {
    if (left.task->config.priority != right.task->config.priority) {
      return left.task->config.priority < right.task->config.priority;
    }
    if (left.release_time != right.release_time) {
      return left.release_time > right.release_time;
    }
    return left.sequence > right.sequence;
  }
};

struct ThreadStartup {
  mutable std::mutex mutex;
  std::condition_variable condition;
  bool ready{false};
  ThreadSchedulingStatus status;
};

void PublishThreadStartup(
    const std::shared_ptr<ThreadStartup>& startup,
    ThreadSchedulingStatus status) {
  {
    std::lock_guard lock(startup->mutex);
    startup->status = std::move(status);
    startup->ready = true;
  }
  startup->condition.notify_all();
}

[[nodiscard]] ThreadSchedulingStatus WaitForThreadStartup(
    const std::shared_ptr<ThreadStartup>& startup) {
  std::unique_lock lock(startup->mutex);
  startup->condition.wait(lock, [&] { return startup->ready; });
  return startup->status;
}

struct CallbackGroup {
  CallbackGroup(CallbackGroupConfig configured,
                bool priority_inheritance)
      : config(std::move(configured)),
        mutex(ProtocolFor(priority_inheritance)) {}

  CallbackGroupId id{0U};
  CallbackGroupConfig config;
  RealtimeMutex mutex;
  std::condition_variable_any condition;
  std::priority_queue<Job, std::vector<Job>, HigherPriorityFirst> queue;
  bool stopping{false};
  std::vector<std::thread> workers;
  std::vector<std::shared_ptr<ThreadStartup>> startups;
};

[[nodiscard]] std::string ErrorDetail(
    std::string_view operation, int error) {
  std::ostringstream output;
  output << operation << " failed: " << std::strerror(error)
         << " (" << error << ')';
  return output.str();
}

void AppendDetail(std::string& detail, std::string addition) {
  if (!detail.empty()) {
    detail += "; ";
  }
  detail += std::move(addition);
}

#if defined(__linux__)
[[nodiscard]] ThreadSchedulingPolicy PolicyFromNative(
    int policy) noexcept {
  return policy == SCHED_FIFO ? ThreadSchedulingPolicy::Fifo
                              : ThreadSchedulingPolicy::Default;
}

void ReadEffectiveScheduling(ThreadSchedulingStatus& status) {
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  const int affinity_error = ::pthread_getaffinity_np(
      ::pthread_self(), sizeof(cpu_set), &cpu_set);
  if (affinity_error == 0) {
    for (std::size_t cpu = 0U;
         cpu < static_cast<std::size_t>(CPU_SETSIZE); ++cpu) {
      if (CPU_ISSET(cpu, &cpu_set)) {
        status.effective_cpu_affinity.push_back(
            static_cast<int>(cpu));
      }
    }
  } else {
    AppendDetail(status.detail,
                 ErrorDetail("pthread_getaffinity_np",
                             affinity_error));
  }

  int policy = SCHED_OTHER;
  sched_param parameters{};
  const int scheduler_error = ::pthread_getschedparam(
      ::pthread_self(), &policy, &parameters);
  if (scheduler_error == 0) {
    status.effective_policy = PolicyFromNative(policy);
    status.effective_priority = parameters.sched_priority;
  } else {
    AppendDetail(status.detail,
                 ErrorDetail("pthread_getschedparam",
                             scheduler_error));
  }
}
#endif

}  // namespace

std::vector<int> AvailableCpuIds() {
  std::vector<int> cpus;
#if defined(__linux__)
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  if (::sched_getaffinity(0, sizeof(cpu_set), &cpu_set) == 0) {
    for (std::size_t cpu = 0U;
         cpu < static_cast<std::size_t>(CPU_SETSIZE); ++cpu) {
      if (CPU_ISSET(cpu, &cpu_set)) {
        cpus.push_back(static_cast<int>(cpu));
      }
    }
  }
#else
  const auto count = std::thread::hardware_concurrency();
  cpus.reserve(count);
  for (unsigned int cpu = 0U; cpu < count; ++cpu) {
    cpus.push_back(static_cast<int>(cpu));
  }
#endif
  return cpus;
}

Status ValidateThreadSchedulingConfig(
    const ThreadSchedulingConfig& config) {
  std::vector<int> cpus = config.cpu_affinity;
  std::sort(cpus.begin(), cpus.end());
  if (std::adjacent_find(cpus.begin(), cpus.end()) != cpus.end()) {
    return Status(StatusCode::InvalidArgument,
                  "CPU affinity contains a duplicate CPU");
  }
  for (const int cpu : cpus) {
#if defined(__linux__)
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
#else
    if (cpu < 0) {
#endif
      return Status(StatusCode::InvalidArgument,
                    "CPU affinity contains an invalid CPU");
    }
  }
  const auto available = AvailableCpuIds();
  if (!available.empty()) {
    for (const int cpu : cpus) {
      if (std::find(available.begin(), available.end(), cpu) ==
          available.end()) {
        return Status(
            StatusCode::InvalidArgument,
            "CPU affinity is outside the current process cpuset");
      }
    }
  }

  if (config.policy == ThreadSchedulingPolicy::Default) {
    if (config.priority != 0) {
      return Status(
          StatusCode::InvalidArgument,
          "default scheduling policy requires priority 0");
    }
    return Status::Ok();
  }

#if defined(__linux__)
  const int minimum = ::sched_get_priority_min(SCHED_FIFO);
  const int maximum = ::sched_get_priority_max(SCHED_FIFO);
  if (minimum == -1 || maximum == -1) {
    return Status(StatusCode::Unsupported,
                  "SCHED_FIFO priority range is unavailable");
  }
#else
  constexpr int minimum = 1;
  constexpr int maximum = 99;
#endif
  if (config.priority < minimum || config.priority > maximum) {
    return Status(StatusCode::InvalidArgument,
                  "SCHED_FIFO priority is outside the platform range");
  }
  return Status::Ok();
}

ThreadSchedulingStatus ApplyCurrentThreadScheduling(
    std::string role, std::size_t worker_index,
    const ThreadSchedulingConfig& config) {
  ThreadSchedulingStatus status;
  status.role = std::move(role);
  status.worker_index = worker_index;
  status.requested = config;
#if defined(__linux__)
  status.native_thread_id =
      static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif

  const auto validation = ValidateThreadSchedulingConfig(config);
  if (!validation) {
    status.affinity_error = EINVAL;
    status.scheduler_error = EINVAL;
    status.affinity_fallback = !config.cpu_affinity.empty();
    status.scheduler_fallback =
        config.policy == ThreadSchedulingPolicy::Fifo;
    status.detail = validation.detail();
#if defined(__linux__)
    ReadEffectiveScheduling(status);
#endif
    return status;
  }

#if defined(__linux__)
  if (config.cpu_affinity.empty()) {
    status.affinity_applied = true;
  } else {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (const int cpu : config.cpu_affinity) {
      CPU_SET(static_cast<std::size_t>(cpu), &cpu_set);
    }
    status.affinity_error = ::pthread_setaffinity_np(
        ::pthread_self(), sizeof(cpu_set), &cpu_set);
    status.affinity_applied = status.affinity_error == 0;
    status.affinity_fallback = !status.affinity_applied;
    if (status.affinity_error != 0) {
      AppendDetail(status.detail,
                   ErrorDetail("pthread_setaffinity_np",
                               status.affinity_error));
    }
  }

  const int native_policy =
      config.policy == ThreadSchedulingPolicy::Fifo
          ? SCHED_FIFO
          : SCHED_OTHER;
  sched_param parameters{};
  parameters.sched_priority = config.priority;
  status.scheduler_error = ::pthread_setschedparam(
      ::pthread_self(), native_policy, &parameters);
  status.scheduler_applied = status.scheduler_error == 0;
  status.scheduler_fallback = !status.scheduler_applied;
  if (status.scheduler_error != 0) {
    AppendDetail(status.detail,
                 ErrorDetail("pthread_setschedparam",
                             status.scheduler_error));
  }
  ReadEffectiveScheduling(status);
#else
  status.effective_cpu_affinity = AvailableCpuIds();
  status.effective_policy = ThreadSchedulingPolicy::Default;
  status.effective_priority = 0;
  status.affinity_applied = config.cpu_affinity.empty();
  status.affinity_fallback = !config.cpu_affinity.empty();
  status.scheduler_applied =
      config.policy == ThreadSchedulingPolicy::Default;
  status.scheduler_fallback =
      config.policy == ThreadSchedulingPolicy::Fifo;
  if (status.affinity_fallback) {
    status.affinity_error = ENOTSUP;
    AppendDetail(status.detail,
                 "thread affinity is unsupported on this platform");
  }
  if (status.scheduler_fallback) {
    status.scheduler_error = ENOTSUP;
    AppendDetail(status.detail,
                 "SCHED_FIFO is unsupported on this platform");
  }
#endif
  return status;
}

namespace {

void ConfigureThreadStartup(
    const std::shared_ptr<ThreadStartup>& startup,
    std::string role, std::size_t worker_index,
    const ThreadSchedulingConfig& config) noexcept {
  try {
    PublishThreadStartup(
        startup,
        ApplyCurrentThreadScheduling(
            std::move(role), worker_index, config));
  } catch (const std::exception& error) {
    ThreadSchedulingStatus status;
    status.role = std::move(role);
    status.worker_index = worker_index;
    status.requested = config;
    status.affinity_fallback = !config.cpu_affinity.empty();
    status.scheduler_fallback =
        config.policy == ThreadSchedulingPolicy::Fifo;
    status.affinity_error =
        status.affinity_fallback ? EFAULT : 0;
    status.scheduler_error =
        status.scheduler_fallback ? EFAULT : 0;
    status.detail =
        std::string("thread configuration threw: ") + error.what();
    PublishThreadStartup(startup, std::move(status));
  } catch (...) {
    ThreadSchedulingStatus status;
    status.role = std::move(role);
    status.worker_index = worker_index;
    status.requested = config;
    status.affinity_fallback = !config.cpu_affinity.empty();
    status.scheduler_fallback =
        config.policy == ThreadSchedulingPolicy::Fifo;
    status.affinity_error =
        status.affinity_fallback ? EFAULT : 0;
    status.scheduler_error =
        status.scheduler_fallback ? EFAULT : 0;
    status.detail = "thread configuration threw";
    PublishThreadStartup(startup, std::move(status));
  }
}

}  // namespace

std::string_view ThreadSchedulingPolicyName(
    ThreadSchedulingPolicy policy) noexcept {
  switch (policy) {
    case ThreadSchedulingPolicy::Default:
      return "default";
    case ThreadSchedulingPolicy::Fifo:
      return "fifo";
  }
  return "unknown";
}

std::string_view DeadlinePolicyName(DeadlinePolicy policy) noexcept {
  switch (policy) {
    case DeadlinePolicy::Warn:
      return "warn";
    case DeadlinePolicy::Degrade:
      return "degrade";
    case DeadlinePolicy::Drop:
      return "drop";
  }
  return "unknown";
}

std::string_view DeadlineActionName(DeadlineAction action) noexcept {
  switch (action) {
    case DeadlineAction::None:
      return "none";
    case DeadlineAction::Warned:
      return "warned";
    case DeadlineAction::Degraded:
      return "degraded";
    case DeadlineAction::Dropped:
      return "dropped";
  }
  return "unknown";
}

struct Executor::Impl {
  explicit Impl(ExecutorConfig configured)
      : config(std::move(configured)),
        mutex(ProtocolFor(config.priority_inheritance)),
        scheduler_startup(std::make_shared<ThreadStartup>()) {}

  ExecutorConfig config;
  mutable RealtimeMutex mutex;
  std::condition_variable_any scheduler_condition;
  std::unordered_map<CallbackGroupId, std::shared_ptr<CallbackGroup>> groups;
  std::unordered_map<TaskId, std::shared_ptr<TaskState>> tasks;
  CallbackGroupId next_group_id{1U};
  TaskId next_task_id{1U};
  std::uint64_t next_job_sequence{1U};
  std::uint64_t schedule_version{0U};
  bool running{false};
  bool stopping{false};
  std::atomic<bool> degraded{false};
  std::thread scheduler;
  std::shared_ptr<ThreadStartup> scheduler_startup;

  [[nodiscard]] std::shared_ptr<TaskState> FindTask(TaskId task_id) const {
    std::lock_guard lock(mutex);
    const auto iterator = tasks.find(task_id);
    return iterator == tasks.end() ? nullptr : iterator->second;
  }

  Status Enqueue(const std::shared_ptr<TaskState>& task,
                 TimePoint release_time) {
    std::shared_ptr<CallbackGroup> group;
    std::uint64_t sequence = 0U;
    {
      std::lock_guard lock(mutex);
      if (!running || stopping) {
        return Status(StatusCode::Closed,
                      "executor is not accepting work");
      }
      const auto group_iterator =
          groups.find(task->config.callback_group);
      if (group_iterator == groups.end()) {
        return Status(StatusCode::NotFound,
                      "callback group no longer exists");
      }
      group = group_iterator->second;
      sequence = next_job_sequence++;
    }

    const auto enqueued_at = Clock::now();
    {
      std::lock_guard task_lock(task->mutex);
      if (task->cancelled) {
        return Status(StatusCode::Cancelled, "task is cancelled");
      }
      ++task->stats.releases;
      if (task->queued >= task->config.queue_capacity) {
        ++task->stats.queue_overflows;
        return Status(StatusCode::QueueFull, "task queue is full");
      }
      ++task->queued;
    }

    {
      std::lock_guard group_lock(group->mutex);
      if (group->stopping) {
        std::lock_guard task_lock(task->mutex);
        --task->queued;
        return Status(StatusCode::Closed,
                      "callback group is stopping");
      }
      if (group->queue.size() >= group->config.queue_capacity) {
        std::lock_guard task_lock(task->mutex);
        --task->queued;
        ++task->stats.queue_overflows;
        return Status(StatusCode::QueueFull,
                      "callback group queue is full");
      }
      group->queue.push(
          Job{task, release_time, enqueued_at, sequence});
    }
    group->condition.notify_one();
    return Status::Ok();
  }

  void InvokeDeadlineHandler(
      const std::shared_ptr<TaskState>& task,
      const DeadlineEvent& event) {
    if (!config.deadline_event_handler) {
      return;
    }
    try {
      config.deadline_event_handler(event);
    } catch (...) {
      std::lock_guard task_lock(task->mutex);
      ++task->stats.deadline_handler_failures;
    }
  }

  void WorkerLoop(const std::shared_ptr<CallbackGroup>& group) {
    for (;;) {
      Job job;
      bool group_stopping = false;
      {
        std::unique_lock lock(group->mutex);
        group->condition.wait(lock, [&] {
          return group->stopping || !group->queue.empty();
        });
        if (group->stopping && group->queue.empty()) {
          return;
        }
        group_stopping = group->stopping;
        job = group->queue.top();
        group->queue.pop();
      }

      const auto dispatch_time = Clock::now();
      bool execute = false;
      bool dropped = false;
      std::stop_token stop_token;
      DeadlineEvent drop_event;
      {
        std::lock_guard task_lock(job.task->mutex);
        if (job.task->queued > 0U) {
          --job.task->queued;
        }
        if (!job.task->cancelled && !group_stopping) {
          const bool expired_before_start =
              job.task->config.deadline >
                  std::chrono::nanoseconds::zero() &&
              dispatch_time >
                  job.release_time + job.task->config.deadline;
          if (expired_before_start &&
              job.task->config.deadline_policy ==
                  DeadlinePolicy::Drop) {
            TaskSample sample;
            sample.task_id = job.task->id;
            sample.release_sequence = job.sequence;
            sample.scheduled_release_time = job.release_time;
            sample.actual_start_time = dispatch_time;
            sample.finish_time = dispatch_time;
            sample.release_jitter =
                NonNegativeDuration(dispatch_time, job.release_time);
            sample.response_time = sample.release_jitter;
            sample.queue_delay =
                NonNegativeDuration(dispatch_time, job.enqueued_at);
            sample.deadline_miss = true;
            sample.dropped = true;
            sample.deadline_action = DeadlineAction::Dropped;
            ++job.task->stats.deadline_misses;
            ++job.task->stats.deadline_drops;
            AppendSample(job.task->stats, sample);
            drop_event.task_id = job.task->id;
            drop_event.task_name = job.task->config.name;
            drop_event.policy = job.task->config.deadline_policy;
            drop_event.action = DeadlineAction::Dropped;
            drop_event.total_deadline_misses =
                job.task->stats.deadline_misses;
            drop_event.sample = sample;
            dropped = true;
          } else {
            ++job.task->stats.started;
            execute = true;
            stop_token = job.task->stop_source.get_token();
          }
        }
      }
      if (dropped) {
        InvokeDeadlineHandler(job.task, drop_event);
        continue;
      }
      if (!execute) {
        continue;
      }

      const auto start = Clock::now();
      bool callback_failed = false;
      try {
        job.task->callback(stop_token);
      } catch (const std::exception&) {
        callback_failed = true;
      } catch (...) {
        callback_failed = true;
      }
      const auto finish = Clock::now();

      TaskSample sample;
      sample.task_id = job.task->id;
      sample.release_sequence = job.sequence;
      sample.scheduled_release_time = job.release_time;
      sample.actual_start_time = start;
      sample.finish_time = finish;
      sample.execution_time = NonNegativeDuration(finish, start);
      sample.release_jitter =
          NonNegativeDuration(start, job.release_time);
      sample.response_time =
          NonNegativeDuration(finish, job.release_time);
      sample.queue_delay =
          NonNegativeDuration(start, job.enqueued_at);
      sample.deadline_miss =
          job.task->config.deadline >
              std::chrono::nanoseconds::zero() &&
          finish > job.release_time + job.task->config.deadline;

      bool emit_deadline_event = false;
      DeadlineEvent event;
      {
        std::lock_guard task_lock(job.task->mutex);
        ++job.task->stats.finished;
        if (callback_failed) {
          ++job.task->stats.callback_failures;
        }
        if (sample.deadline_miss) {
          ++job.task->stats.deadline_misses;
          switch (job.task->config.deadline_policy) {
            case DeadlinePolicy::Warn:
              sample.deadline_action = DeadlineAction::Warned;
              ++job.task->stats.deadline_warnings;
              break;
            case DeadlinePolicy::Degrade:
              sample.deadline_action = DeadlineAction::Degraded;
              ++job.task->stats.deadline_degrades;
              degraded.store(true, std::memory_order_release);
              break;
            case DeadlinePolicy::Drop:
              sample.deadline_action = DeadlineAction::Warned;
              ++job.task->stats.deadline_warnings;
              break;
          }
          event.task_id = job.task->id;
          event.task_name = job.task->config.name;
          event.policy = job.task->config.deadline_policy;
          event.action = sample.deadline_action;
          event.total_deadline_misses =
              job.task->stats.deadline_misses;
          event.sample = sample;
          emit_deadline_event = true;
        }
        AppendSample(job.task->stats, sample);
      }
      if (emit_deadline_event) {
        InvokeDeadlineHandler(job.task, event);
      }
    }
  }

  void SchedulerLoop() {
    for (;;) {
      std::vector<std::pair<std::shared_ptr<TaskState>, TimePoint>> due;
      {
        std::unique_lock lock(mutex);
        if (stopping) {
          return;
        }

        TimePoint earliest = TimePoint::max();
        for (const auto& [unused_id, task] : tasks) {
          static_cast<void>(unused_id);
          std::lock_guard task_lock(task->mutex);
          if (!task->cancelled &&
              task->config.kind == TaskKind::Periodic) {
            earliest = std::min(earliest, task->next_release);
          }
        }

        const auto observed_version = schedule_version;
        if (earliest == TimePoint::max()) {
          scheduler_condition.wait(lock, [&] {
            return stopping || schedule_version != observed_version;
          });
          continue;
        }
        if (Clock::now() < earliest) {
          scheduler_condition.wait_until(lock, earliest, [&] {
            return stopping || schedule_version != observed_version;
          });
          continue;
        }

        const auto now = Clock::now();
        for (const auto& [unused_id, task] : tasks) {
          static_cast<void>(unused_id);
          std::lock_guard task_lock(task->mutex);
          if (task->cancelled ||
              task->config.kind != TaskKind::Periodic) {
            continue;
          }
          std::size_t catch_up = 0U;
          constexpr std::size_t maximum_catch_up = 1024U;
          while (task->next_release <= now &&
                 catch_up < maximum_catch_up) {
            due.emplace_back(task, task->next_release);
            task->next_release += task->config.period;
            ++catch_up;
          }
          if (catch_up == maximum_catch_up &&
              task->next_release <= now) {
            const auto behind = now - task->next_release;
            const auto skipped =
                static_cast<std::uint64_t>(
                    behind / task->config.period) +
                1U;
            task->stats.periodic_releases_skipped += skipped;
            task->next_release +=
                task->config.period *
                static_cast<std::int64_t>(skipped);
          }
        }
      }

      for (const auto& [task, release_time] : due) {
        static_cast<void>(Enqueue(task, release_time));
      }
    }
  }
};

Executor::Executor(ExecutorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Executor::~Executor() {
  if (impl_) {
    static_cast<void>(Stop(Deadline::Infinite()));
  }
}

Result<CallbackGroupId> Executor::CreateCallbackGroup(
    CallbackGroupConfig config) {
  if (config.name.empty() || config.worker_count == 0U ||
      config.queue_capacity == 0U) {
    return Status(StatusCode::InvalidArgument,
                  "callback group requires a name, workers, and capacity");
  }
  const auto scheduling_status =
      ValidateThreadSchedulingConfig(config.thread_scheduling);
  if (!scheduling_status) {
    return scheduling_status;
  }

  auto group = std::make_shared<CallbackGroup>(
      std::move(config), impl_->config.priority_inheritance);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running || impl_->stopping) {
      return Status(StatusCode::AlreadyExists,
                    "callback groups must be created before Start");
    }
    group->id = impl_->next_group_id++;
    impl_->groups.emplace(group->id, group);
  }
  return group->id;
}

Result<TaskId> Executor::AddTask(TaskConfig config,
                                 TaskCallback callback) {
  if (config.name.empty() || !callback ||
      config.queue_capacity == 0U ||
      config.deadline < std::chrono::nanoseconds::zero() ||
      (config.kind == TaskKind::Periodic &&
       config.period <= std::chrono::nanoseconds::zero())) {
    return Status(StatusCode::InvalidArgument,
                  "invalid task configuration");
  }

  auto task = std::make_shared<TaskState>(
      impl_->config.priority_inheritance);
  task->config = std::move(config);
  task->callback = std::move(callback);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
      return Status(StatusCode::Closed, "executor is stopping");
    }
    if (!impl_->groups.contains(task->config.callback_group)) {
      return Status(StatusCode::NotFound,
                    "callback group does not exist");
    }
    task->id = impl_->next_task_id++;
    task->stats.task_id = task->id;
    task->stats.name = task->config.name;
    task->stats.kind = task->config.kind;
    if (impl_->running &&
        task->config.kind == TaskKind::Periodic) {
      task->next_release =
          Clock::now() + task->config.period;
    }
    impl_->tasks.emplace(task->id, task);
    ++impl_->schedule_version;
  }
  impl_->scheduler_condition.notify_all();

  if (task->config.kind == TaskKind::Async && running()) {
    const auto status = impl_->Enqueue(task, Clock::now());
    if (!status) {
      return status;
    }
  }
  return task->id;
}

Status Executor::Start() {
  const auto scheduler_config_status =
      ValidateThreadSchedulingConfig(
          impl_->config.scheduler_thread);
  if (!scheduler_config_status) {
    return scheduler_config_status;
  }

  std::vector<std::shared_ptr<CallbackGroup>> groups;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) {
      return Status(StatusCode::AlreadyExists,
                    "executor is already running");
    }
    if (impl_->stopping) {
      return Status(StatusCode::Closed,
                    "executor cannot be restarted");
    }
    if (impl_->groups.empty()) {
      return Status(StatusCode::InvalidArgument,
                    "executor requires at least one callback group");
    }
    impl_->running = true;
    const auto now = Clock::now();
    for (const auto& [unused_id, task] : impl_->tasks) {
      static_cast<void>(unused_id);
      std::lock_guard task_lock(task->mutex);
      if (task->config.kind == TaskKind::Periodic) {
        task->next_release = now + task->config.period;
      }
    }
    for (const auto& [unused_id, group] : impl_->groups) {
      static_cast<void>(unused_id);
      groups.push_back(group);
    }
  }

  try {
    for (const auto& group : groups) {
      for (std::size_t index = 0U;
           index < group->config.worker_count; ++index) {
        auto startup = std::make_shared<ThreadStartup>();
        group->startups.push_back(startup);
        group->workers.emplace_back(
            [this, group, startup, index] {
              ConfigureThreadStartup(
                  startup, group->config.name, index,
                  group->config.thread_scheduling);
              impl_->WorkerLoop(group);
            });
      }
    }
    impl_->scheduler = std::thread([this] {
      ConfigureThreadStartup(
          impl_->scheduler_startup, "scheduler", 0U,
          impl_->config.scheduler_thread);
      impl_->SchedulerLoop();
    });

    for (const auto& group : groups) {
      for (const auto& startup : group->startups) {
        static_cast<void>(WaitForThreadStartup(startup));
      }
    }
    static_cast<void>(
        WaitForThreadStartup(impl_->scheduler_startup));
  } catch (const std::system_error& error) {
    static_cast<void>(Stop(Deadline::Infinite()));
    return Status(StatusCode::Internal,
                  std::string("failed to start executor thread: ") +
                      error.what());
  }
  return Status::Ok();
}

Status Executor::Notify(TaskId task_id) {
  auto task = impl_->FindTask(task_id);
  if (!task) {
    return Status(StatusCode::NotFound, "task does not exist");
  }
  if (task->config.kind == TaskKind::Periodic) {
    return Status(StatusCode::InvalidArgument,
                  "periodic tasks cannot be manually notified");
  }
  return impl_->Enqueue(task, Clock::now());
}

Status Executor::Cancel(TaskId task_id) {
  auto task = impl_->FindTask(task_id);
  if (!task) {
    return Status(StatusCode::NotFound, "task does not exist");
  }
  {
    std::lock_guard lock(task->mutex);
    if (task->cancelled) {
      return Status::Ok();
    }
    task->cancelled = true;
    task->stats.cancelled = true;
    task->stop_source.request_stop();
  }
  {
    std::lock_guard lock(impl_->mutex);
    ++impl_->schedule_version;
  }
  impl_->scheduler_condition.notify_all();
  return Status::Ok();
}

Result<TaskStats> Executor::Stats(TaskId task_id) const {
  auto task = impl_->FindTask(task_id);
  if (!task) {
    return Status(StatusCode::NotFound, "task does not exist");
  }
  std::lock_guard lock(task->mutex);
  auto stats = task->stats;
  FinalizeTimingSummary(stats);
  return stats;
}

RuntimeStats Executor::Snapshot() const {
  RuntimeStats snapshot;
  snapshot.captured_at = Clock::now();
  snapshot.degraded =
      impl_->degraded.load(std::memory_order_acquire);
  std::vector<std::shared_ptr<TaskState>> tasks;
  {
    std::lock_guard lock(impl_->mutex);
    tasks.reserve(impl_->tasks.size());
    for (const auto& [unused_id, task] : impl_->tasks) {
      static_cast<void>(unused_id);
      tasks.push_back(task);
    }
  }
  snapshot.tasks.reserve(tasks.size());
  for (const auto& task : tasks) {
    std::lock_guard lock(task->mutex);
    auto stats = task->stats;
    FinalizeTimingSummary(stats);
    snapshot.tasks.push_back(std::move(stats));
  }
  return snapshot;
}

SchedulingSnapshot Executor::Scheduling() const {
  SchedulingSnapshot snapshot;
  snapshot.priority_inheritance_requested =
      impl_->mutex.priority_inheritance_requested();
  snapshot.priority_inheritance_active =
      impl_->mutex.priority_inheritance_active();
  snapshot.priority_inheritance_error =
      impl_->mutex.priority_inheritance_error();

  auto append = [&](const std::shared_ptr<ThreadStartup>& startup) {
    std::lock_guard lock(startup->mutex);
    if (!startup->ready) {
      return;
    }
    snapshot.any_fallback =
        snapshot.any_fallback ||
        startup->status.affinity_fallback ||
        startup->status.scheduler_fallback;
    snapshot.threads.push_back(startup->status);
  };
  append(impl_->scheduler_startup);

  std::vector<std::shared_ptr<CallbackGroup>> groups;
  {
    std::lock_guard lock(impl_->mutex);
    groups.reserve(impl_->groups.size());
    for (const auto& [unused_id, group] : impl_->groups) {
      static_cast<void>(unused_id);
      groups.push_back(group);
    }
  }
  std::sort(groups.begin(), groups.end(),
            [](const auto& left, const auto& right) {
              return left->id < right->id;
            });
  for (const auto& group : groups) {
    for (const auto& startup : group->startups) {
      append(startup);
    }
  }
  return snapshot;
}

Status Executor::AcknowledgeDegraded() {
  impl_->degraded.store(false, std::memory_order_release);
  return Status::Ok();
}

Status Executor::Stop(Deadline deadline) {
  static_cast<void>(deadline);
  std::vector<std::shared_ptr<TaskState>> tasks;
  std::vector<std::shared_ptr<CallbackGroup>> groups;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running && !impl_->stopping) {
      return Status::Ok();
    }
    impl_->running = false;
    impl_->stopping = true;
    ++impl_->schedule_version;
    for (const auto& [unused_id, task] : impl_->tasks) {
      static_cast<void>(unused_id);
      tasks.push_back(task);
    }
    for (const auto& [unused_id, group] : impl_->groups) {
      static_cast<void>(unused_id);
      groups.push_back(group);
    }
  }

  for (const auto& task : tasks) {
    task->stop_source.request_stop();
  }
  impl_->scheduler_condition.notify_all();
  for (const auto& group : groups) {
    {
      std::lock_guard lock(group->mutex);
      group->stopping = true;
    }
    group->condition.notify_all();
  }

  if (impl_->scheduler.joinable()) {
    impl_->scheduler.join();
  }
  for (const auto& group : groups) {
    for (auto& worker : group->workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    group->workers.clear();
  }
  return Status::Ok();
}

bool Executor::running() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

}  // namespace autoruntime
