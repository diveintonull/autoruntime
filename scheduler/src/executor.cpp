#include <autoruntime/executor.hpp>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

struct TaskState {
  mutable std::mutex mutex;
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
  std::chrono::nanoseconds release_lateness{0};
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

struct CallbackGroup {
  CallbackGroupId id{0U};
  CallbackGroupConfig config;
  std::mutex mutex;
  std::condition_variable condition;
  std::priority_queue<Job, std::vector<Job>, HigherPriorityFirst> queue;
  bool stopping{false};
  std::vector<std::thread> workers;
};

}  // namespace

struct Executor::Impl {
  mutable std::mutex mutex;
  std::condition_variable scheduler_condition;
  std::unordered_map<CallbackGroupId, std::shared_ptr<CallbackGroup>> groups;
  std::unordered_map<TaskId, std::shared_ptr<TaskState>> tasks;
  CallbackGroupId next_group_id{1U};
  TaskId next_task_id{1U};
  std::uint64_t next_job_sequence{1U};
  std::uint64_t schedule_version{0U};
  bool running{false};
  bool stopping{false};
  std::thread scheduler;

  [[nodiscard]] std::shared_ptr<TaskState> FindTask(TaskId task_id) const {
    std::lock_guard lock(mutex);
    const auto iterator = tasks.find(task_id);
    return iterator == tasks.end() ? nullptr : iterator->second;
  }

  [[nodiscard]] std::shared_ptr<CallbackGroup> FindGroup(
      CallbackGroupId group_id) const {
    std::lock_guard lock(mutex);
    const auto iterator = groups.find(group_id);
    return iterator == groups.end() ? nullptr : iterator->second;
  }

  Status Enqueue(const std::shared_ptr<TaskState>& task,
                 TimePoint release_time) {
    std::shared_ptr<CallbackGroup> group;
    std::uint64_t sequence = 0U;
    {
      std::lock_guard lock(mutex);
      if (!running || stopping) {
        return Status(StatusCode::Closed, "executor is not accepting work");
      }
      const auto group_iterator = groups.find(task->config.callback_group);
      if (group_iterator == groups.end()) {
        return Status(StatusCode::NotFound, "callback group no longer exists");
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
        return Status(StatusCode::Closed, "callback group is stopping");
      }
      if (group->queue.size() >= group->config.queue_capacity) {
        std::lock_guard task_lock(task->mutex);
        --task->queued;
        ++task->stats.queue_overflows;
        return Status(StatusCode::QueueFull, "callback group queue is full");
      }
      group->queue.push(Job{
          task,
          release_time,
          enqueued_at,
          NonNegativeDuration(enqueued_at, release_time),
          sequence});
    }
    group->condition.notify_one();
    return Status::Ok();
  }

  void WorkerLoop(const std::shared_ptr<CallbackGroup>& group) {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(group->mutex);
        group->condition.wait(lock, [&] {
          return group->stopping || !group->queue.empty();
        });
        if (group->stopping && group->queue.empty()) {
          return;
        }
        job = group->queue.top();
        group->queue.pop();
      }

      bool execute = false;
      std::stop_token stop_token;
      {
        std::lock_guard task_lock(job.task->mutex);
        if (job.task->queued > 0U) {
          --job.task->queued;
        }
        if (!job.task->cancelled && !group->stopping) {
          ++job.task->stats.started;
          execute = true;
          stop_token = job.task->stop_source.get_token();
        }
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
      sample.release_time = job.release_time;
      sample.start_time = start;
      sample.finish_time = finish;
      sample.execution_time = NonNegativeDuration(finish, start);
      sample.release_lateness = job.release_lateness;
      sample.response_time = NonNegativeDuration(finish, job.release_time);
      sample.queue_delay = NonNegativeDuration(start, job.enqueued_at);
      sample.deadline_miss =
          job.task->config.deadline > std::chrono::nanoseconds::zero() &&
          finish > job.release_time + job.task->config.deadline;

      std::lock_guard task_lock(job.task->mutex);
      ++job.task->stats.finished;
      if (callback_failed) {
        ++job.task->stats.callback_failures;
      }
      if (sample.deadline_miss) {
        ++job.task->stats.deadline_misses;
      }
      constexpr std::size_t maximum_samples = 4096U;
      if (job.task->stats.samples.size() == maximum_samples) {
        job.task->stats.samples.erase(job.task->stats.samples.begin());
      }
      job.task->stats.samples.push_back(sample);
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
            task->next_release = now + task->config.period;
          }
        }
      }

      for (const auto& [task, release_time] : due) {
        static_cast<void>(Enqueue(task, release_time));
      }
    }
  }
};

Executor::Executor() : impl_(std::make_unique<Impl>()) {}

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

  auto group = std::make_shared<CallbackGroup>();
  group->config = std::move(config);
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
      (config.kind == TaskKind::Periodic &&
       config.period <= std::chrono::nanoseconds::zero())) {
    return Status(StatusCode::InvalidArgument, "invalid task configuration");
  }

  auto task = std::make_shared<TaskState>();
  task->config = std::move(config);
  task->callback = std::move(callback);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
      return Status(StatusCode::Closed, "executor is stopping");
    }
    if (!impl_->groups.contains(task->config.callback_group)) {
      return Status(StatusCode::NotFound, "callback group does not exist");
    }
    task->id = impl_->next_task_id++;
    task->stats.task_id = task->id;
    task->stats.name = task->config.name;
    task->stats.kind = task->config.kind;
    if (impl_->running && task->config.kind == TaskKind::Periodic) {
      task->next_release = Clock::now() + task->config.period;
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
  std::vector<std::shared_ptr<CallbackGroup>> groups;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->running) {
      return Status(StatusCode::AlreadyExists, "executor is already running");
    }
    if (impl_->stopping) {
      return Status(StatusCode::Closed, "executor cannot be restarted");
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
        group->workers.emplace_back([this, group] {
          impl_->WorkerLoop(group);
        });
      }
    }
    impl_->scheduler = std::thread([this] { impl_->SchedulerLoop(); });
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
  return task->stats;
}

RuntimeStats Executor::Snapshot() const {
  RuntimeStats snapshot;
  snapshot.captured_at = Clock::now();
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
    snapshot.tasks.push_back(task->stats);
  }
  return snapshot;
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
