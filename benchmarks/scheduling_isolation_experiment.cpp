#include <autoruntime/executor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

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

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

double Percentile(std::vector<double> values, double quantile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil(quantile * static_cast<double>(values.size())));
  return values[std::min(
      values.size() - 1U, rank == 0U ? 0U : rank - 1U)];
}

struct Scenario {
  std::string name;
  std::uint64_t control_calls_during_block{0U};
  std::uint64_t control_calls_final{0U};
  std::uint64_t releases{0U};
  std::uint64_t finished{0U};
  std::uint64_t deadline_misses{0U};
  std::uint64_t queue_overflows{0U};
  double response_p50_us{0.0};
  double response_p95_us{0.0};
  double response_p99_us{0.0};
};

Scenario RunScenario(bool isolated) {
  autoruntime::Executor executor;
  const auto planning_group = Take(
      executor.CreateCallbackGroup({"planning", 1U, 64U}),
      "create planning group");
  auto control_group = planning_group;
  if (isolated) {
    control_group = Take(
        executor.CreateCallbackGroup({"control", 1U, 64U}),
        "create control group");
  }

  std::atomic<bool> slow_entered{false};
  std::atomic<bool> slow_finished{false};
  std::atomic<std::uint64_t> control_calls{0U};

  autoruntime::TaskConfig slow_config;
  slow_config.name = "slow-planning";
  slow_config.kind = autoruntime::TaskKind::Event;
  slow_config.priority = 200;
  slow_config.deadline = 400ms;
  slow_config.queue_capacity = 1U;
  slow_config.callback_group = planning_group;
  const auto slow_task = Take(
      executor.AddTask(
          slow_config,
          [&](std::stop_token) {
            slow_entered.store(true, std::memory_order_release);
            std::this_thread::sleep_for(300ms);
            slow_finished.store(true, std::memory_order_release);
          }),
      "add slow planning task");

  autoruntime::TaskConfig control_config;
  control_config.name = "control-loop";
  control_config.kind = autoruntime::TaskKind::Periodic;
  control_config.priority = 100;
  control_config.period = 20ms;
  control_config.deadline = 15ms;
  control_config.queue_capacity = 32U;
  control_config.callback_group = control_group;
  const auto control_task = Take(
      executor.AddTask(
          control_config,
          [&](std::stop_token) {
            control_calls.fetch_add(1U, std::memory_order_relaxed);
          }),
      "add control task");

  Require(static_cast<bool>(executor.Start()), "start executor");
  Require(static_cast<bool>(executor.Notify(slow_task)),
          "release slow task");
  Require(WaitUntil(
              [&] {
                return slow_entered.load(std::memory_order_acquire);
              },
              500ms),
          "slow task did not start");

  std::this_thread::sleep_for(220ms);
  Scenario result;
  result.name = isolated ? "isolated_groups" : "shared_group";
  result.control_calls_during_block =
      control_calls.load(std::memory_order_acquire);

  Require(WaitUntil(
              [&] {
                return slow_finished.load(std::memory_order_acquire);
              },
              500ms),
          "slow task did not finish");
  std::this_thread::sleep_for(120ms);

  const auto stats = Take(
      executor.Stats(control_task), "read control stats");
  result.control_calls_final =
      control_calls.load(std::memory_order_acquire);
  result.releases = stats.releases;
  result.finished = stats.finished;
  result.deadline_misses = stats.deadline_misses;
  result.queue_overflows = stats.queue_overflows;
  std::vector<double> response_microseconds;
  response_microseconds.reserve(stats.samples.size());
  for (const auto& sample : stats.samples) {
    response_microseconds.push_back(
        static_cast<double>(sample.response_time.count()) / 1000.0);
  }
  result.response_p50_us = Percentile(response_microseconds, 0.50);
  result.response_p95_us = Percentile(response_microseconds, 0.95);
  result.response_p99_us = Percentile(response_microseconds, 0.99);

  Require(static_cast<bool>(
              executor.Stop(autoruntime::Deadline::After(1s))),
          "stop executor");
  return result;
}

void WriteScenario(std::ostream& output, const Scenario& scenario,
                   bool trailing_comma) {
  output << "    {\n";
  output << "      \"name\": \"" << scenario.name << "\",\n";
  output << "      \"control_calls_during_300ms_block\": "
         << scenario.control_calls_during_block << ",\n";
  output << "      \"control_calls_final\": "
         << scenario.control_calls_final << ",\n";
  output << "      \"releases\": " << scenario.releases << ",\n";
  output << "      \"finished\": " << scenario.finished << ",\n";
  output << "      \"deadline_misses\": "
         << scenario.deadline_misses << ",\n";
  output << "      \"queue_overflows\": "
         << scenario.queue_overflows << ",\n";
  output << "      \"response_p50_us\": "
         << scenario.response_p50_us << ",\n";
  output << "      \"response_p95_us\": "
         << scenario.response_p95_us << ",\n";
  output << "      \"response_p99_us\": "
         << scenario.response_p99_us << "\n";
  output << "    }" << (trailing_comma ? ",\n" : "\n");
}

void WriteJson(std::ostream& output, const Scenario& shared,
               const Scenario& isolated) {
  output << std::fixed << std::setprecision(3);
  output << "{\n";
  output << "  \"slow_callback_ms\": 300,\n";
  output << "  \"observation_during_block_ms\": 220,\n";
  output << "  \"control_period_ms\": 20,\n";
  output << "  \"control_deadline_ms\": 15,\n";
  output << "  \"scenarios\": [\n";
  WriteScenario(output, shared, true);
  WriteScenario(output, isolated, false);
  output << "  ]\n";
  output << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string output_path;
    if (argc == 3 && std::string_view(argv[1]) == "--output") {
      output_path = argv[2];
    } else if (argc != 1) {
      throw std::invalid_argument(
          "usage: scheduling_isolation_experiment [--output FILE]");
    }

    const auto shared = RunScenario(false);
    const auto isolated = RunScenario(true);
    Require(shared.control_calls_during_block == 0U,
            "shared group unexpectedly ran control during blockage");
    Require(isolated.control_calls_during_block >= 8U,
            "isolated control did not maintain its expected cadence");
    Require(shared.deadline_misses > 0U,
            "shared group did not expose deadline misses");
    Require(isolated.queue_overflows == 0U,
            "isolated control overflowed");

    if (output_path.empty()) {
      WriteJson(std::cout, shared, isolated);
    } else {
      std::ofstream output(output_path);
      Require(static_cast<bool>(output), "open output file");
      WriteJson(output, shared, isolated);
      Require(static_cast<bool>(output), "write output file");
      std::cout << output_path << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "scheduling_isolation_experiment: "
              << error.what() << '\n';
    return 1;
  }
}
