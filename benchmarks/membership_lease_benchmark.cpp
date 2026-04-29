#include <autoruntime/distributed.hpp>
#include <autoruntime/observability.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef AUTORUNTIME_SOURCE_REVISION
#define AUTORUNTIME_SOURCE_REVISION "unknown"
#endif

#ifndef AUTORUNTIME_COMPILER
#define AUTORUNTIME_COMPILER "unknown"
#endif

#ifndef AUTORUNTIME_BUILD_TYPE
#define AUTORUNTIME_BUILD_TYPE "unknown"
#endif

using namespace std::chrono_literals;

namespace {

using Clock = autoruntime::Deadline::Clock;

constexpr auto kHeartbeatPeriod = 10ms;
constexpr auto kLeaseTimeout = 60ms;
constexpr auto kGenerationFenceTimeout = 500ms;
constexpr auto kOperationTimeout = 2s;

void Require(bool condition, std::string_view detail) {
  if (!condition) {
    throw std::runtime_error(std::string(detail));
  }
}

void RequireStatus(const autoruntime::Status& status,
                   std::string_view operation) {
  if (!status) {
    throw std::runtime_error(
        std::string(operation) + ": " + status.detail());
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
bool WaitUntil(Predicate&& predicate,
               std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

struct Options {
  std::size_t iterations{30U};
  std::string output_path;
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      std::cout
          << "usage: autoruntime_membership_lease_benchmark "
             "[--iterations N] [--output FILE]\n";
      std::exit(0);
    }
    if (argument == "--iterations" && index + 1 < argc) {
      const auto parsed = std::stoull(argv[++index]);
      Require(parsed > 0U &&
                  parsed <=
                      static_cast<unsigned long long>(
                          std::numeric_limits<std::size_t>::max()) &&
                  parsed <= 10'000U,
              "iterations must be in [1, 10000]");
      options.iterations = static_cast<std::size_t>(parsed);
      continue;
    }
    if (argument == "--output" && index + 1 < argc) {
      options.output_path = argv[++index];
      continue;
    }
    throw std::invalid_argument("unknown or incomplete argument");
  }
  return options;
}

autoruntime::DiscoveryConfig DiscoveryConfig(
    std::string node_id, std::uint64_t generation) {
  autoruntime::DiscoveryConfig config;
  config.node_id = std::move(node_id);
  config.generation = generation;
  config.bind_endpoint = {"127.0.0.1", 0U};
  config.advertised_rpc_endpoint = {"127.0.0.1", 1U};
  config.heartbeat_period = kHeartbeatPeriod;
  config.lease_timeout = kLeaseTimeout;
  config.generation_fence_timeout = kGenerationFenceTimeout;
  config.max_members = 4U;
  config.max_peers = 4U;
  config.max_generation_fences = 4U;
  return config;
}

bool IsGeneration(const autoruntime::DiscoveryService& service,
                  std::uint64_t generation) {
  const auto member = service.Find("benchmark-member");
  return member && member.value().generation == generation;
}

double Microseconds(Clock::duration duration) {
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(
                 duration)
                 .count()) /
         1000.0;
}

autoruntime::DistributionSummary DistributionFor(
    const autoruntime::MetricsRegistry& metrics,
    std::string_view name) {
  const auto snapshot = metrics.Snapshot();
  const auto iterator = std::find_if(
      snapshot.begin(), snapshot.end(),
      [&](const autoruntime::MetricSample& sample) {
        return sample.name == name &&
               sample.kind == autoruntime::MetricKind::Histogram;
      });
  Require(iterator != snapshot.end(), "missing histogram summary");
  return iterator->distribution;
}

void AppendDistribution(
    std::ostringstream& output, std::string_view name,
    const autoruntime::DistributionSummary& distribution) {
  output << '"' << name << '"' << ':' << '{'
         << '"' << "count" << '"' << ':'
         << distribution.count
         << ",\"min\":" << distribution.minimum
         << ",\"mean\":" << distribution.mean
         << ",\"p50\":" << distribution.p50
         << ",\"p95\":" << distribution.p95
         << ",\"p99\":" << distribution.p99
         << ",\"p99_9\":" << distribution.p99_9
         << ",\"max\":" << distribution.maximum << '}';
}

std::uint64_t CounterDelta(std::uint64_t after,
                           std::uint64_t before) {
  return after >= before ? after - before : 0U;
}

std::string Run(const Options& options) {
  auto supervisor_config =
      DiscoveryConfig("benchmark-supervisor", 1U);
  auto supervisor = Take(
      autoruntime::DiscoveryService::Create(supervisor_config),
      "create supervisor");
  RequireStatus(supervisor->Start(), "start supervisor");

  std::uint64_t generation = 2U;
  auto active_config =
      DiscoveryConfig("benchmark-member", generation);
  auto active = Take(
      autoruntime::DiscoveryService::Create(active_config),
      "create initial member");
  RequireStatus(
      active->AddPeer(supervisor->LocalEndpoint()),
      "add supervisor peer");
  RequireStatus(active->Start(), "start initial member");
  Require(WaitUntil(
              [&] { return IsGeneration(*supervisor, generation); },
              kOperationTimeout),
          "initial member was not discovered");

  autoruntime::MetricsRegistry metrics(options.iterations);
  const auto cpu_before = autoruntime::SampleProcessCpu();
  const auto wall_started = Clock::now();

  for (std::size_t iteration = 0U;
       iteration < options.iterations; ++iteration) {
    const auto stale_before =
        supervisor->Stats().stale_announcements;
    auto stale_config =
        DiscoveryConfig("benchmark-member", generation - 1U);
    auto stale = Take(
        autoruntime::DiscoveryService::Create(stale_config),
        "create stale member");
    RequireStatus(
        stale->AddPeer(supervisor->LocalEndpoint()),
        "add stale peer");
    RequireStatus(stale->Start(), "start stale member");
    Require(WaitUntil(
                [&] {
                  return supervisor->Stats().stale_announcements >
                         stale_before;
                },
                kOperationTimeout),
            "stale heartbeat was not observed");

    RequireStatus(active->Stop(), "stop active member");
    const auto loss_started = Clock::now();
    Require(WaitUntil(
                [&] {
                  return !supervisor->Find("benchmark-member");
                },
                kOperationTimeout),
            "member lease did not expire");
    RequireStatus(
        metrics.Observe(
            "lease_detection_us",
            Microseconds(Clock::now() - loss_started)),
        "observe lease detection");

    std::this_thread::sleep_for(kHeartbeatPeriod * 2);
    Require(!supervisor->Find("benchmark-member"),
            "stale generation resurrected after lease expiry");

    ++generation;
    auto restarted_config =
        DiscoveryConfig("benchmark-member", generation);
    auto restarted = Take(
        autoruntime::DiscoveryService::Create(restarted_config),
        "create restarted member");
    RequireStatus(
        restarted->AddPeer(supervisor->LocalEndpoint()),
        "add restarted peer");
    const auto recovery_started = Clock::now();
    RequireStatus(restarted->Start(), "start restarted member");
    Require(WaitUntil(
                [&] { return IsGeneration(*supervisor, generation); },
                kOperationTimeout),
            "restarted member was not discovered");
    RequireStatus(
        metrics.Observe(
            "restart_discovery_us",
            Microseconds(Clock::now() - recovery_started)),
        "observe restart discovery");

    RequireStatus(stale->Stop(), "stop stale member");
    active = std::move(restarted);
  }

  const auto wall_finished = Clock::now();
  const auto cpu_after = autoruntime::SampleProcessCpu();
  const auto stats = supervisor->Stats();
  Require(stats.expired_members >= options.iterations,
          "not every stopped member expired");
  Require(stats.generation_fences_created >= options.iterations,
          "not every expiry created a generation fence");
  Require(stats.generation_fence_rejections >= options.iterations,
          "stale generations were not fenced in every iteration");

  RequireStatus(active->Stop(), "stop final member");
  RequireStatus(supervisor->Stop(), "stop supervisor");

  const auto lease = DistributionFor(metrics, "lease_detection_us");
  const auto recovery =
      DistributionFor(metrics, "restart_discovery_us");
  const double wall_seconds =
      std::chrono::duration<double>(wall_finished - wall_started).count();
  const double user_seconds =
      cpu_after.user_seconds - cpu_before.user_seconds;
  const double system_seconds =
      cpu_after.system_seconds - cpu_before.system_seconds;
  const double cpu_percent =
      wall_seconds > 0.0
          ? ((user_seconds + system_seconds) / wall_seconds) * 100.0
          : 0.0;

  std::ostringstream output;
  output << std::setprecision(15)
         << "{\"schema_version\":1,\"status\":\"passed\""
         << ",\"source_revision\":\""
         << AUTORUNTIME_SOURCE_REVISION << "\""
         << ",\"compiler\":\"" << AUTORUNTIME_COMPILER << "\""
         << ",\"build_type\":\""
         << AUTORUNTIME_BUILD_TYPE << "\""
         << ",\"topology\":\"single_host_ipv4_loopback\""
         << ",\"clock\":\"steady_clock\""
         << ",\"iterations\":" << options.iterations
         << ",\"heartbeat_period_ms\":"
         << kHeartbeatPeriod.count()
         << ",\"lease_timeout_ms\":" << kLeaseTimeout.count()
         << ",\"generation_fence_timeout_ms\":"
         << kGenerationFenceTimeout.count()
         << ",\"final_generation\":" << generation
         << ",\"expired_members\":" << stats.expired_members
         << ",\"generation_fences_created\":"
         << stats.generation_fences_created
         << ",\"generation_fence_rejections\":"
         << stats.generation_fence_rejections << ',';
  AppendDistribution(output, "lease_detection_us", lease);
  output << ',';
  AppendDistribution(output, "restart_discovery_us", recovery);
  output << ",\"process\":{\"cpu_metrics_available\":"
         << ((cpu_before.available && cpu_after.available)
                 ? "true" : "false")
         << ",\"wall_seconds\":" << wall_seconds
         << ",\"user_cpu_seconds\":" << user_seconds
         << ",\"system_cpu_seconds\":" << system_seconds
         << ",\"cpu_utilization_percent\":" << cpu_percent
         << ",\"voluntary_context_switches\":"
         << CounterDelta(cpu_after.voluntary_context_switches,
                         cpu_before.voluntary_context_switches)
         << ",\"involuntary_context_switches\":"
         << CounterDelta(cpu_after.involuntary_context_switches,
                         cpu_before.involuntary_context_switches)
         << ",\"maximum_resident_bytes\":"
         << cpu_after.maximum_resident_bytes << "}}";
  return output.str();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    const auto result = Run(options);
    std::cout << result << '\n';
    if (!options.output_path.empty()) {
      std::ofstream output(options.output_path,
                           std::ios::binary | std::ios::trunc);
      Require(output.is_open(), "failed to open output file");
      output << result << '\n';
      Require(output.good(), "failed to write output file");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "membership lease benchmark failed: "
              << error.what() << '\n';
    return 1;
  }
}
