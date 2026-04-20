#include <autoruntime/executor.hpp>
#include <autoruntime/realtime_mutex.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

#ifndef AUTORUNTIME_SOURCE_REVISION
#define AUTORUNTIME_SOURCE_REVISION "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;

void Require(bool condition, std::string_view detail) {
  if (!condition) {
    throw std::runtime_error(std::string(detail));
  }
}

void BusyFor(std::chrono::milliseconds duration) {
  const auto finish = Clock::now() + duration;
  std::uint64_t value = 0x6a09e667f3bcc909ULL;
  while (Clock::now() < finish) {
    for (std::size_t index = 0U; index < 10'000U; ++index) {
      value = value * 2862933555777941757ULL + 3037000493ULL;
    }
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : "+r"(value));
#endif
  }
}

struct Gate {
  std::mutex mutex;
  std::condition_variable condition;
  std::size_t configured{0U};
  bool low_locked{false};
  bool begin{false};
};

struct Trial {
  bool priority_inheritance_requested{false};
  bool priority_inheritance_active{false};
  int priority_inheritance_error{0};
  bool fifo_applied{false};
  bool affinity_applied{false};
  bool measurement_valid{false};
  double high_blocking_us{0.0};
  double low_critical_us{0.0};
  double medium_busy_us{0.0};
  std::vector<autoruntime::ThreadSchedulingStatus> threads;
};

void PublishConfigured(
    Gate& gate, autoruntime::ThreadSchedulingStatus status,
    std::vector<autoruntime::ThreadSchedulingStatus>& statuses,
    std::mutex& statuses_mutex) {
  {
    std::lock_guard lock(statuses_mutex);
    statuses.push_back(std::move(status));
  }
  {
    std::lock_guard lock(gate.mutex);
    ++gate.configured;
  }
  gate.condition.notify_all();
}

void WaitForBegin(Gate& gate) {
  std::unique_lock lock(gate.mutex);
  gate.condition.wait(lock, [&] { return gate.begin; });
}

Trial RunTrial(bool priority_inheritance, int cpu) {
  autoruntime::RealtimeMutex shared_mutex(
      priority_inheritance
          ? autoruntime::MutexProtocol::PriorityInheritance
          : autoruntime::MutexProtocol::Default);
  Gate gate;
  std::mutex statuses_mutex;
  std::vector<autoruntime::ThreadSchedulingStatus> statuses;
  statuses.reserve(3U);
  Clock::duration high_blocking{};
  Clock::duration low_critical{};
  Clock::duration medium_busy{};

  auto scheduling = [&](int priority) {
    autoruntime::ThreadSchedulingConfig config;
    config.cpu_affinity = {cpu};
    config.policy = autoruntime::ThreadSchedulingPolicy::Fifo;
    config.priority = priority;
    return config;
  };

  std::thread low([&] {
    PublishConfigured(
        gate,
        autoruntime::ApplyCurrentThreadScheduling(
            "low", 0U, scheduling(10)),
        statuses, statuses_mutex);
    shared_mutex.lock();
    {
      std::unique_lock lock(gate.mutex);
      gate.low_locked = true;
      gate.condition.notify_all();
      gate.condition.wait(lock, [&] { return gate.begin; });
    }
    const auto start = Clock::now();
    BusyFor(20ms);
    const auto finish = Clock::now();
    low_critical = finish - start;
    shared_mutex.unlock();
  });

  std::thread medium([&] {
    PublishConfigured(
        gate,
        autoruntime::ApplyCurrentThreadScheduling(
            "medium", 0U, scheduling(20)),
        statuses, statuses_mutex);
    WaitForBegin(gate);
    const auto start = Clock::now();
    BusyFor(100ms);
    const auto finish = Clock::now();
    medium_busy = finish - start;
  });

  std::thread high([&] {
    PublishConfigured(
        gate,
        autoruntime::ApplyCurrentThreadScheduling(
            "high", 0U, scheduling(30)),
        statuses, statuses_mutex);
    WaitForBegin(gate);
    const auto start = Clock::now();
    std::lock_guard lock(shared_mutex);
    const auto finish = Clock::now();
    high_blocking = finish - start;
  });

  {
    std::unique_lock lock(gate.mutex);
    Require(gate.condition.wait_for(
                lock, 2s,
                [&] {
                  return gate.configured == 3U &&
                         gate.low_locked;
                }),
            "threads did not reach the inversion gate");
    gate.begin = true;
  }
  gate.condition.notify_all();

  low.join();
  medium.join();
  high.join();

  Trial result;
  result.priority_inheritance_requested = priority_inheritance;
  result.priority_inheritance_active =
      shared_mutex.priority_inheritance_active();
  result.priority_inheritance_error =
      shared_mutex.priority_inheritance_error();
  result.fifo_applied = true;
  result.affinity_applied = true;
  {
    std::lock_guard lock(statuses_mutex);
    result.threads = std::move(statuses);
  }
  std::sort(
      result.threads.begin(), result.threads.end(),
      [](const auto& left, const auto& right) {
        return left.requested.priority < right.requested.priority;
      });
  for (const auto& status : result.threads) {
    result.fifo_applied =
        result.fifo_applied &&
        status.scheduler_applied &&
        status.effective_policy ==
            autoruntime::ThreadSchedulingPolicy::Fifo;
    result.affinity_applied =
        result.affinity_applied && status.affinity_applied;
  }
  result.measurement_valid =
      result.fifo_applied && result.affinity_applied &&
      (!priority_inheritance ||
       result.priority_inheritance_active);
  result.high_blocking_us =
      std::chrono::duration<double, std::micro>(
          high_blocking)
          .count();
  result.low_critical_us =
      std::chrono::duration<double, std::micro>(
          low_critical)
          .count();
  result.medium_busy_us =
      std::chrono::duration<double, std::micro>(
          medium_busy)
          .count();
  return result;
}

std::string JsonEscape(std::string_view value) {
  std::string output;
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      output.push_back('\\');
    }
    if (character == '\n') {
      output += "\\n";
    } else {
      output.push_back(character);
    }
  }
  return output;
}

void WriteThread(
    std::ostream& output,
    const autoruntime::ThreadSchedulingStatus& status) {
  output << "{\"role\":\"" << status.role
         << "\",\"requested_priority\":"
         << status.requested.priority
         << ",\"effective_policy\":\""
         << autoruntime::ThreadSchedulingPolicyName(
                status.effective_policy)
         << "\",\"effective_priority\":"
         << status.effective_priority
         << ",\"affinity_applied\":"
         << (status.affinity_applied ? "true" : "false")
         << ",\"scheduler_applied\":"
         << (status.scheduler_applied ? "true" : "false")
         << ",\"scheduler_error\":"
         << status.scheduler_error
         << ",\"detail\":\""
         << JsonEscape(status.detail) << "\"}";
}

void WriteTrial(
    std::ostream& output, const Trial& trial,
    bool trailing_comma) {
  output << "    {\n"
         << "      \"priority_inheritance_requested\": "
         << (trial.priority_inheritance_requested
                 ? "true"
                 : "false")
         << ",\n"
         << "      \"priority_inheritance_active\": "
         << (trial.priority_inheritance_active ? "true" : "false")
         << ",\n"
         << "      \"priority_inheritance_error\": "
         << trial.priority_inheritance_error << ",\n"
         << "      \"fifo_applied\": "
         << (trial.fifo_applied ? "true" : "false") << ",\n"
         << "      \"affinity_applied\": "
         << (trial.affinity_applied ? "true" : "false") << ",\n"
         << "      \"measurement_valid\": "
         << (trial.measurement_valid ? "true" : "false") << ",\n"
         << "      \"high_blocking_us\": "
         << trial.high_blocking_us << ",\n"
         << "      \"low_critical_us\": "
         << trial.low_critical_us << ",\n"
         << "      \"medium_busy_us\": "
         << trial.medium_busy_us << ",\n"
         << "      \"threads\": [";
  for (std::size_t index = 0U;
       index < trial.threads.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    WriteThread(output, trial.threads[index]);
  }
  output << "]\n    }" << (trailing_comma ? ",\n" : "\n");
}

void WriteJson(
    std::ostream& output, int cpu,
    const Trial& without_inheritance,
    const Trial& with_inheritance) {
  const bool comparison_valid =
      without_inheritance.measurement_valid &&
      with_inheritance.measurement_valid;
  output << std::fixed << std::setprecision(3);
  output << "{\n"
         << "  \"source_revision\": \""
         << AUTORUNTIME_SOURCE_REVISION << "\",\n"
         << "  \"target_cpu\": " << cpu << ",\n"
         << "  \"low_priority\": 10,\n"
         << "  \"medium_priority\": 20,\n"
         << "  \"high_priority\": 30,\n"
         << "  \"low_critical_ms\": 20,\n"
         << "  \"medium_busy_ms\": 100,\n"
         << "  \"comparison_valid\": "
         << (comparison_valid ? "true" : "false") << ",\n"
         << "  \"invalid_reason\": \""
         << (comparison_valid
                 ? ""
                 : "SCHED_FIFO and affinity must apply to all three "
                   "threads; ordinary-user fallback is not a PI result")
         << "\",\n"
         << "  \"trials\": [\n";
  WriteTrial(output, without_inheritance, true);
  WriteTrial(output, with_inheritance, false);
  output << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string output_path;
    if (argc == 3 && std::string_view(argv[1]) == "--output") {
      output_path = argv[2];
    } else if (argc != 1) {
      throw std::invalid_argument(
          "usage: priority_inversion_experiment [--output FILE]");
    }

    const auto cpus = autoruntime::AvailableCpuIds();
    Require(!cpus.empty(), "no CPU is available");
    const int cpu = cpus.front();
    const auto without_inheritance = RunTrial(false, cpu);
    const auto with_inheritance = RunTrial(true, cpu);

    if (output_path.empty()) {
      WriteJson(
          std::cout, cpu, without_inheritance,
          with_inheritance);
    } else {
      std::ofstream output(output_path);
      Require(static_cast<bool>(output), "open output file");
      WriteJson(
          output, cpu, without_inheritance,
          with_inheritance);
      Require(static_cast<bool>(output), "write output file");
      std::cout << output_path << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "priority_inversion_experiment: "
              << error.what() << '\n';
    return 1;
  }
}
