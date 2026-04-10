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
#include <vector>

namespace autoruntime {

using CallbackGroupId = std::uint64_t;
using TaskId = std::uint64_t;

enum class TaskKind {
  Periodic,
  Event,
  Async,
};

struct CallbackGroupConfig {
  std::string name;
  std::size_t worker_count{1U};
  std::size_t queue_capacity{256U};
};

struct TaskConfig {
  std::string name;
  TaskKind kind{TaskKind::Async};
  int priority{0};
  std::chrono::nanoseconds period{0};
  std::chrono::nanoseconds deadline{0};
  std::size_t queue_capacity{1U};
  CallbackGroupId callback_group{0U};
};

struct TaskSample {
  TaskId task_id{0U};
  std::uint64_t release_sequence{0U};
  Deadline::TimePoint release_time{};
  Deadline::TimePoint start_time{};
  Deadline::TimePoint finish_time{};
  std::chrono::nanoseconds execution_time{0};
  std::chrono::nanoseconds release_lateness{0};
  std::chrono::nanoseconds response_time{0};
  std::chrono::nanoseconds queue_delay{0};
  bool deadline_miss{false};
};

struct TaskStats {
  TaskId task_id{0U};
  std::string name;
  TaskKind kind{TaskKind::Async};
  std::uint64_t releases{0U};
  std::uint64_t started{0U};
  std::uint64_t finished{0U};
  std::uint64_t queue_overflows{0U};
  std::uint64_t deadline_misses{0U};
  std::uint64_t callback_failures{0U};
  bool cancelled{false};
  std::vector<TaskSample> samples;
};

struct RuntimeStats {
  Deadline::TimePoint captured_at{};
  std::vector<TaskStats> tasks;
};

using TaskCallback = std::function<void(std::stop_token)>;

class Executor {
 public:
  Executor();
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
  Status Stop(Deadline deadline = Deadline::Infinite());
  [[nodiscard]] bool running() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime
