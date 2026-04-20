#include <autoruntime/executor.hpp>
#include <autoruntime/observability.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef AUTORUNTIME_SOURCE_REVISION
#define AUTORUNTIME_SOURCE_REVISION "unknown"
#endif

using namespace std::chrono_literals;

namespace {

using Clock = autoruntime::Deadline::Clock;

void Require(bool condition, std::string_view detail) {
  if (!condition) {
    throw std::runtime_error(std::string(detail));
  }
}

template <typename T>
T Take(autoruntime::Result<T> result, std::string_view operation) {
  if (!result) {
    throw std::runtime_error(
        std::string(operation) + ": " + result.status().detail());
  }
  return std::move(result).take_value();
}

void RequireStatus(const autoruntime::Status& status,
                   std::string_view operation) {
  if (!status) {
    throw std::runtime_error(
        std::string(operation) + ": " + status.detail());
  }
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream output;
  for (const char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << character;
        break;
    }
  }
  return output.str();
}

double Microseconds(std::chrono::nanoseconds duration) {
  return static_cast<double>(duration.count()) / 1000.0;
}

struct Options {
  std::string output_path;
  std::chrono::milliseconds duration{1000};
  std::chrono::microseconds period{1000};
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      std::cout
          << "usage: autoruntime_realtime_scheduling_benchmark "
             "[--output FILE] [--duration-ms N] [--period-us N]\n";
      std::exit(0);
    }
    if (argument == "--output" && index + 1 < argc) {
      options.output_path = argv[++index];
      continue;
    }
    if (argument == "--duration-ms" && index + 1 < argc) {
      options.duration = std::chrono::milliseconds(
          std::stoll(argv[++index]));
      continue;
    }
    if (argument == "--period-us" && index + 1 < argc) {
      options.period = std::chrono::microseconds(
          std::stoll(argv[++index]));
      continue;
    }
    throw std::invalid_argument("unknown or incomplete argument");
  }
  Require(options.duration > 0ms, "duration must be positive");
  Require(options.period > 0us, "period must be positive");
  return options;
}

#if defined(__linux__)
class StressProcesses {
 public:
  StressProcesses() = default;
  ~StressProcesses() { Stop(); }

  StressProcesses(const StressProcesses&) = delete;
  StressProcesses& operator=(const StressProcesses&) = delete;

  void Start(const std::vector<int>& cpus) {
    for (const int cpu : cpus) {
      const pid_t child = ::fork();
      if (child < 0) {
        Stop();
        throw std::runtime_error("fork CPU stress process failed");
      }
      if (child == 0) {
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(static_cast<std::size_t>(cpu), &cpu_set);
        if (::sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
          ::_exit(120);
        }
        std::uint64_t value =
            static_cast<std::uint64_t>(cpu) +
            static_cast<std::uint64_t>(
                0x9e3779b97f4a7c15ULL);
        for (;;) {
          for (std::size_t iteration = 0U;
               iteration < 1'000'000U; ++iteration) {
            value = value * 2862933555777941757ULL + 3037000493ULL;
          }
          asm volatile("" : "+r"(value));
        }
      }
      children_.push_back(child);
    }
    std::this_thread::sleep_for(100ms);
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return children_.size();
  }

  void Stop() noexcept {
    for (const pid_t child : children_) {
      static_cast<void>(::kill(child, SIGTERM));
    }
    for (const pid_t child : children_) {
      int status = 0;
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
    }
    children_.clear();
  }

 private:
  std::vector<pid_t> children_;
};
#else
class StressProcesses {
 public:
  void Start(const std::vector<int>&) {
    throw std::runtime_error(
        "controlled CPU stress requires Linux process affinity");
  }
  [[nodiscard]] std::size_t size() const noexcept { return 0U; }
  void Stop() noexcept {}
};
#endif

enum class Mode {
  Default,
  Affinity,
  AffinityFifo,
};

struct ScenarioConfig {
  std::string name;
  Mode mode{Mode::Default};
  bool cpu_stress{false};
};

struct ScenarioResult {
  std::string name;
  std::string load;
  int target_cpu{-1};
  std::size_t stress_processes{0U};
  std::uint64_t releases{0U};
  std::uint64_t finished{0U};
  std::uint64_t queue_overflows{0U};
  std::uint64_t skipped_releases{0U};
  std::uint64_t deadline_misses{0U};
  std::uint64_t deadline_events{0U};
  double deadline_miss_rate{0.0};
  autoruntime::TimingDistribution jitter;
  autoruntime::TimingDistribution execution;
  autoruntime::TimingDistribution response;
  bool fifo_requested{false};
  bool fifo_applied{false};
  bool scheduling_fallback{false};
  bool affinity_applied{false};
  double wall_time_ms{0.0};
  double user_cpu_ms{0.0};
  double system_cpu_ms{0.0};
  double cpu_utilization_percent{0.0};
  std::uint64_t voluntary_context_switches{0U};
  std::uint64_t involuntary_context_switches{0U};
  std::uint64_t maximum_resident_bytes{0U};
  std::vector<autoruntime::ThreadSchedulingStatus> threads;
};

ScenarioResult RunScenario(
    const ScenarioConfig& scenario,
    const Options& options,
    const std::vector<int>& available_cpus) {
  Require(!available_cpus.empty(), "no available CPU was detected");
  const int target_cpu = available_cpus.front();

  StressProcesses stress;
  if (scenario.cpu_stress) {
    stress.Start(available_cpus);
  }

  std::atomic<std::uint64_t> deadline_events{0U};
  autoruntime::ExecutorConfig executor_config;
  executor_config.deadline_event_handler =
      [&](const autoruntime::DeadlineEvent&) {
        deadline_events.fetch_add(1U, std::memory_order_relaxed);
      };
  if (scenario.mode != Mode::Default) {
    executor_config.scheduler_thread.cpu_affinity = {target_cpu};
  }
  if (scenario.mode == Mode::AffinityFifo) {
    executor_config.scheduler_thread.policy =
        autoruntime::ThreadSchedulingPolicy::Fifo;
    executor_config.scheduler_thread.priority = 80;
  }

  autoruntime::Executor executor(std::move(executor_config));
  autoruntime::CallbackGroupConfig group_config;
  group_config.name = "control";
  group_config.worker_count = 1U;
  group_config.queue_capacity = 8192U;
  if (scenario.mode != Mode::Default) {
    group_config.thread_scheduling.cpu_affinity = {target_cpu};
  }
  if (scenario.mode == Mode::AffinityFifo) {
    group_config.thread_scheduling.policy =
        autoruntime::ThreadSchedulingPolicy::Fifo;
    group_config.thread_scheduling.priority = 70;
  }
  const auto group = Take(
      executor.CreateCallbackGroup(std::move(group_config)),
      "create control callback group");

  autoruntime::TaskConfig task_config;
  task_config.name = "control-loop";
  task_config.kind = autoruntime::TaskKind::Periodic;
  task_config.priority = 100;
  task_config.period =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          options.period);
  task_config.deadline = task_config.period;
  task_config.queue_capacity = 8192U;
  task_config.callback_group = group;
  task_config.deadline_policy = autoruntime::DeadlinePolicy::Warn;

  std::atomic<std::uint64_t> callbacks{0U};
  const auto task = Take(
      executor.AddTask(
          task_config,
          [&](std::stop_token) {
            callbacks.fetch_add(1U, std::memory_order_relaxed);
          }),
      "add periodic control task");

  RequireStatus(executor.Start(), "start executor");
  const auto scheduling = executor.Scheduling();
  const auto cpu_before = autoruntime::SampleProcessCpu();
  const auto wall_start = Clock::now();
  std::this_thread::sleep_until(wall_start + options.duration);
  RequireStatus(executor.Cancel(task), "cancel control task");
  RequireStatus(
      executor.Stop(autoruntime::Deadline::After(2s)),
      "stop executor");
  const auto wall_finish = Clock::now();
  const auto cpu_after = autoruntime::SampleProcessCpu();
  const auto stats = Take(executor.Stats(task), "read task stats");

  ScenarioResult result;
  result.name = scenario.name;
  result.load = scenario.cpu_stress ? "cpu_stress" : "idle";
  result.target_cpu =
      scenario.mode == Mode::Default ? -1 : target_cpu;
  result.stress_processes = stress.size();
  result.releases = stats.releases;
  result.finished = stats.finished;
  result.queue_overflows = stats.queue_overflows;
  result.skipped_releases = stats.periodic_releases_skipped;
  result.deadline_misses = stats.deadline_misses;
  result.deadline_events =
      deadline_events.load(std::memory_order_relaxed);
  result.deadline_miss_rate = stats.timing.deadline_miss_rate;
  result.jitter = stats.timing.release_jitter;
  result.execution = stats.timing.execution_time;
  result.response = stats.timing.response_time;
  result.fifo_requested =
      scenario.mode == Mode::AffinityFifo;
  result.scheduling_fallback = scheduling.any_fallback;
  result.affinity_applied = true;
  result.fifo_applied = result.fifo_requested;
  result.threads = scheduling.threads;
  for (const auto& thread : scheduling.threads) {
    if (!thread.requested.cpu_affinity.empty() &&
        !thread.affinity_applied) {
      result.affinity_applied = false;
    }
    if (thread.requested.policy ==
            autoruntime::ThreadSchedulingPolicy::Fifo &&
        (!thread.scheduler_applied ||
         thread.effective_policy !=
             autoruntime::ThreadSchedulingPolicy::Fifo)) {
      result.fifo_applied = false;
    }
  }
  const auto elapsed =
      std::chrono::duration<double>(wall_finish - wall_start);
  result.wall_time_ms = elapsed.count() * 1000.0;
  if (cpu_before.available && cpu_after.available) {
    result.user_cpu_ms =
        (cpu_after.user_seconds - cpu_before.user_seconds) * 1000.0;
    result.system_cpu_ms =
        (cpu_after.system_seconds - cpu_before.system_seconds) *
        1000.0;
    const double total_cpu_seconds =
        result.user_cpu_ms / 1000.0 +
        result.system_cpu_ms / 1000.0;
    result.cpu_utilization_percent =
        elapsed.count() > 0.0
            ? total_cpu_seconds / elapsed.count() * 100.0
            : 0.0;
    result.voluntary_context_switches =
        cpu_after.voluntary_context_switches -
        cpu_before.voluntary_context_switches;
    result.involuntary_context_switches =
        cpu_after.involuntary_context_switches -
        cpu_before.involuntary_context_switches;
    result.maximum_resident_bytes =
        cpu_after.maximum_resident_bytes;
  }
  Require(callbacks.load(std::memory_order_relaxed) == stats.finished,
          "callback count and finished count differ");
  stress.Stop();
  return result;
}

void WriteDistribution(
    std::ostream& output,
    const autoruntime::TimingDistribution& distribution) {
  output << "{\"sampled_count\":" << distribution.sampled_count
         << ",\"min_us\":" << Microseconds(distribution.minimum)
         << ",\"p50_us\":" << Microseconds(distribution.p50)
         << ",\"p95_us\":" << Microseconds(distribution.p95)
         << ",\"p99_us\":" << Microseconds(distribution.p99)
         << ",\"p99_9_us\":" << Microseconds(distribution.p99_9)
         << ",\"max_us\":" << Microseconds(distribution.maximum)
         << '}';
}

void WriteThread(
    std::ostream& output,
    const autoruntime::ThreadSchedulingStatus& thread) {
  output << "{\"role\":\"" << JsonEscape(thread.role)
         << "\",\"worker_index\":" << thread.worker_index
         << ",\"native_thread_id\":" << thread.native_thread_id
         << ",\"requested_policy\":\""
         << autoruntime::ThreadSchedulingPolicyName(
                thread.requested.policy)
         << "\",\"requested_priority\":"
         << thread.requested.priority
         << ",\"effective_policy\":\""
         << autoruntime::ThreadSchedulingPolicyName(
                thread.effective_policy)
         << "\",\"effective_priority\":"
         << thread.effective_priority
         << ",\"affinity_applied\":"
         << (thread.affinity_applied ? "true" : "false")
         << ",\"scheduler_applied\":"
         << (thread.scheduler_applied ? "true" : "false")
         << ",\"affinity_fallback\":"
         << (thread.affinity_fallback ? "true" : "false")
         << ",\"scheduler_fallback\":"
         << (thread.scheduler_fallback ? "true" : "false")
         << ",\"affinity_error\":" << thread.affinity_error
         << ",\"scheduler_error\":" << thread.scheduler_error
         << ",\"detail\":\"" << JsonEscape(thread.detail) << "\"}";
}

void WriteScenario(
    std::ostream& output, const ScenarioResult& result,
    bool trailing_comma) {
  output << "    {\n"
         << "      \"name\": \"" << result.name << "\",\n"
         << "      \"load\": \"" << result.load << "\",\n"
         << "      \"target_cpu\": " << result.target_cpu << ",\n"
         << "      \"stress_processes\": "
         << result.stress_processes << ",\n"
         << "      \"releases\": " << result.releases << ",\n"
         << "      \"finished\": " << result.finished << ",\n"
         << "      \"queue_overflows\": "
         << result.queue_overflows << ",\n"
         << "      \"skipped_releases\": "
         << result.skipped_releases << ",\n"
         << "      \"deadline_misses\": "
         << result.deadline_misses << ",\n"
         << "      \"deadline_events\": "
         << result.deadline_events << ",\n"
         << "      \"deadline_miss_rate\": "
         << result.deadline_miss_rate << ",\n"
         << "      \"fifo_requested\": "
         << (result.fifo_requested ? "true" : "false") << ",\n"
         << "      \"fifo_applied\": "
         << (result.fifo_applied ? "true" : "false") << ",\n"
         << "      \"scheduling_fallback\": "
         << (result.scheduling_fallback ? "true" : "false")
         << ",\n"
         << "      \"affinity_applied\": "
         << (result.affinity_applied ? "true" : "false") << ",\n"
         << "      \"wall_time_ms\": " << result.wall_time_ms
         << ",\n"
         << "      \"user_cpu_ms\": " << result.user_cpu_ms
         << ",\n"
         << "      \"system_cpu_ms\": " << result.system_cpu_ms
         << ",\n"
         << "      \"cpu_utilization_percent\": "
         << result.cpu_utilization_percent << ",\n"
         << "      \"voluntary_context_switches\": "
         << result.voluntary_context_switches << ",\n"
         << "      \"involuntary_context_switches\": "
         << result.involuntary_context_switches << ",\n"
         << "      \"maximum_resident_bytes\": "
         << result.maximum_resident_bytes << ",\n"
         << "      \"release_jitter\": ";
  WriteDistribution(output, result.jitter);
  output << ",\n      \"execution_time\": ";
  WriteDistribution(output, result.execution);
  output << ",\n      \"response_time\": ";
  WriteDistribution(output, result.response);
  output << ",\n      \"threads\": [";
  for (std::size_t index = 0U; index < result.threads.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    WriteThread(output, result.threads[index]);
  }
  output << "]\n    }" << (trailing_comma ? ",\n" : "\n");
}

void WriteJson(
    std::ostream& output, const Options& options,
    const std::vector<int>& available_cpus,
    const std::vector<ScenarioResult>& results) {
#if defined(__linux__)
  utsname system{};
  const bool have_uname = ::uname(&system) == 0;
  char hostname[256]{};
  const bool have_hostname =
      ::gethostname(hostname, sizeof(hostname) - 1U) == 0;
#else
  const bool have_uname = false;
  const bool have_hostname = false;
  const char* hostname = "unknown";
#endif
  output << std::fixed << std::setprecision(3);
  output << "{\n"
         << "  \"source_revision\": \""
         << AUTORUNTIME_SOURCE_REVISION << "\",\n"
         << "  \"hostname\": \""
         << (have_hostname ? JsonEscape(hostname) : "unknown")
         << "\",\n"
         << "  \"kernel\": \"";
#if defined(__linux__)
  output << (have_uname ? JsonEscape(system.release) : "unknown");
#else
  output << "unknown";
#endif
  output << "\",\n"
         << "  \"duration_ms\": " << options.duration.count()
         << ",\n"
         << "  \"period_us\": " << options.period.count()
         << ",\n"
         << "  \"clock\": \"std::chrono::steady_clock\",\n"
         << "  \"quantile_method\": \"nearest-rank\",\n"
         << "  \"stress_model\": "
            "\"one SCHED_OTHER busy child per allowed CPU; child CPU "
            "time excluded from RUSAGE_SELF\",\n"
         << "  \"available_cpus\": [";
  for (std::size_t index = 0U;
       index < available_cpus.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << available_cpus[index];
  }
  output << "],\n  \"scenarios\": [\n";
  for (std::size_t index = 0U; index < results.size(); ++index) {
    WriteScenario(output, results[index],
                  index + 1U != results.size());
  }
  output << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    const auto available_cpus = autoruntime::AvailableCpuIds();
    Require(!available_cpus.empty(), "no CPU is available");

    const std::vector<ScenarioConfig> scenarios{
        {"default_idle", Mode::Default, false},
        {"affinity_idle", Mode::Affinity, false},
        {"affinity_fifo_idle", Mode::AffinityFifo, false},
        {"default_cpu_stress", Mode::Default, true},
        {"affinity_cpu_stress", Mode::Affinity, true},
        {"affinity_fifo_cpu_stress", Mode::AffinityFifo, true},
    };
    std::vector<ScenarioResult> results;
    results.reserve(scenarios.size());
    for (const auto& scenario : scenarios) {
      results.push_back(
          RunScenario(scenario, options, available_cpus));
    }

    if (options.output_path.empty()) {
      WriteJson(std::cout, options, available_cpus, results);
    } else {
      std::ofstream output(options.output_path);
      Require(static_cast<bool>(output), "open output file");
      WriteJson(output, options, available_cpus, results);
      Require(static_cast<bool>(output), "write output file");
      std::cout << options.output_path << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "realtime_scheduling_benchmark: "
              << error.what() << '\n';
    return 1;
  }
}
