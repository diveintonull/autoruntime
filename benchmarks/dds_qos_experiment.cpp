#include <autoruntime/dds_transport.hpp>
#include <autoruntime/message.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

struct Scenario {
  std::string name;
  autoruntime::Reliability writer_reliability{
      autoruntime::Reliability::Reliable};
  autoruntime::Reliability reader_reliability{
      autoruntime::Reliability::Reliable};
  std::size_t attempted{0U};
  std::size_t published{0U};
  std::size_t received{0U};
  double send_rate_messages_per_second{0.0};
  std::vector<double> latency_microseconds;
};

[[nodiscard]] const char* Name(autoruntime::Reliability reliability) {
  return reliability == autoruntime::Reliability::Reliable
             ? "Reliable"
             : "BestEffort";
}

void Require(bool condition, std::string_view detail) {
  if (!condition) {
    throw std::runtime_error(std::string(detail));
  }
}

[[nodiscard]] autoruntime::DdsTransportConfig Config(
    std::uint32_t domain_id, std::string participant_name) {
  autoruntime::DdsTransportConfig config;
  config.domain_id = domain_id;
  config.participant_name = std::move(participant_name);
  config.receive_poll_interval = 1ms;
  config.reliability_max_blocking_time = 250ms;
  return config;
}

[[nodiscard]] autoruntime::QosProfile Qos(
    autoruntime::Reliability reliability) {
  autoruntime::QosProfile qos;
  qos.reliability = reliability;
  qos.history = autoruntime::HistoryKind::KeepAll;
  qos.depth = 1024U;
  qos.deadline = 2s;
  qos.liveliness = autoruntime::LivelinessKind::Automatic;
  qos.liveliness_lease = 3s;
  return qos;
}

[[nodiscard]] Scenario RunScenario(
    std::string name, std::uint32_t domain_id,
    autoruntime::Reliability writer_reliability,
    autoruntime::Reliability reader_reliability,
    std::size_t message_count) {
  Scenario result;
  result.name = std::move(name);
  result.writer_reliability = writer_reliability;
  result.reader_reliability = reader_reliability;
  result.attempted = message_count;

  auto reader_transport = autoruntime::DdsTransport::Create(
      Config(domain_id, result.name + "-reader"));
  auto writer_transport = autoruntime::DdsTransport::Create(
      Config(domain_id, result.name + "-writer"));
  Require(static_cast<bool>(reader_transport),
          "failed to create DDS reader participant");
  Require(static_cast<bool>(writer_transport),
          "failed to create DDS writer participant");

  const std::string topic =
      "autoruntime_qos_" + result.name + "_" +
      std::to_string(static_cast<long long>(::getpid()));
  std::atomic<std::size_t> received{0U};
  std::mutex latency_mutex;
  std::vector<double> latencies;
  latencies.reserve(message_count);

  const auto reader_qos = Qos(reader_reliability);
  auto subscription = reader_transport.value()->Subscribe(
      topic, reader_qos,
      [&](autoruntime::Message message) {
        if (message.envelope.sequence == 0U) {
          return;
        }
        const auto now = autoruntime::MonotonicNanoseconds();
        const auto elapsed =
            now >= message.envelope.publish_timestamp_ns
                ? now - message.envelope.publish_timestamp_ns
                : 0U;
        {
          std::lock_guard lock(latency_mutex);
          latencies.push_back(static_cast<double>(elapsed) / 1000.0);
        }
        received.fetch_add(1U, std::memory_order_release);
      });
  Require(static_cast<bool>(subscription),
          "failed to create DDS reader");

  const auto writer_qos = Qos(writer_reliability);
  autoruntime::Message warmup;
  warmup.envelope.trace_id = 1U;
  warmup.envelope.sequence = 0U;
  warmup.envelope.publish_timestamp_ns =
      autoruntime::MonotonicNanoseconds();
  warmup.envelope.source_timestamp_ns =
      warmup.envelope.publish_timestamp_ns;
  warmup.envelope.source_generation = 1U;
  warmup.payload.assign(128U, std::byte{0x2A});
  Require(static_cast<bool>(writer_transport.value()->Publish(
              topic, std::move(warmup), writer_qos)),
          "failed to publish DDS discovery warmup");
  std::this_thread::sleep_for(750ms);

  const std::vector<std::byte> payload(128U, std::byte{0x5A});
  const auto send_start = std::chrono::steady_clock::now();
  for (std::size_t index = 0U; index < message_count; ++index) {
    autoruntime::Message message;
    message.envelope.trace_id = index + 2U;
    message.envelope.span_id = index + 1U;
    message.envelope.sequence = index + 1U;
    message.envelope.publish_timestamp_ns =
        autoruntime::MonotonicNanoseconds();
    message.envelope.source_timestamp_ns =
        message.envelope.publish_timestamp_ns;
    message.envelope.source_generation = 1U;
    message.payload = payload;
    const auto status = writer_transport.value()->Publish(
        topic, std::move(message), writer_qos);
    if (status) {
      ++result.published;
    }
  }
  const auto send_finish = std::chrono::steady_clock::now();
  const auto send_seconds =
      std::chrono::duration<double>(send_finish - send_start).count();
  if (send_seconds > 0.0) {
    result.send_rate_messages_per_second =
        static_cast<double>(result.published) / send_seconds;
  }

  auto last_progress = std::chrono::steady_clock::now();
  std::size_t last_received = received.load(std::memory_order_acquire);
  const auto receive_deadline = last_progress + 5s;
  while (std::chrono::steady_clock::now() < receive_deadline &&
         last_received < result.published) {
    std::this_thread::sleep_for(10ms);
    const auto current = received.load(std::memory_order_acquire);
    if (current != last_received) {
      last_received = current;
      last_progress = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - last_progress > 750ms) {
      break;
    }
  }

  Require(static_cast<bool>(
              reader_transport.value()->Unsubscribe(subscription.value())),
          "failed to stop DDS reader");
  result.received = received.load(std::memory_order_acquire);
  {
    std::lock_guard lock(latency_mutex);
    result.latency_microseconds = std::move(latencies);
  }
  Require(static_cast<bool>(reader_transport.value()->Close()),
          "failed to close DDS reader participant");
  Require(static_cast<bool>(writer_transport.value()->Close()),
          "failed to close DDS writer participant");
  return result;
}

[[nodiscard]] double Percentile(
    std::vector<double> values, double quantile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil(quantile * static_cast<double>(values.size())));
  const auto index =
      std::min(values.size() - 1U, rank == 0U ? 0U : rank - 1U);
  return values[index];
}

[[nodiscard]] double LossPercent(const Scenario& scenario) {
  if (scenario.published == 0U ||
      scenario.received >= scenario.published) {
    return 0.0;
  }
  return 100.0 *
         static_cast<double>(scenario.published - scenario.received) /
         static_cast<double>(scenario.published);
}

void WriteJson(std::ostream& output,
               const std::vector<Scenario>& scenarios) {
  output << std::fixed << std::setprecision(3);
  output << "{\n";
  output << "  \"cyclonedds_version\": \"11.0.1\",\n";
  output << "  \"message_bytes\": 128,\n";
  output << "  \"discovery_warmup_ms\": 750,\n";
  output << "  \"scenarios\": [\n";
  for (std::size_t index = 0U; index < scenarios.size(); ++index) {
    const auto& scenario = scenarios[index];
    output << "    {\n";
    output << "      \"name\": \"" << scenario.name << "\",\n";
    output << "      \"writer_reliability\": \""
           << Name(scenario.writer_reliability) << "\",\n";
    output << "      \"reader_reliability\": \""
           << Name(scenario.reader_reliability) << "\",\n";
    output << "      \"attempted\": " << scenario.attempted << ",\n";
    output << "      \"published\": " << scenario.published << ",\n";
    output << "      \"received\": " << scenario.received << ",\n";
    output << "      \"loss_percent\": " << LossPercent(scenario)
           << ",\n";
    output << "      \"send_rate_messages_per_second\": "
           << scenario.send_rate_messages_per_second << ",\n";
    output << "      \"latency_samples\": "
           << scenario.latency_microseconds.size() << ",\n";
    output << "      \"p50_latency_us\": "
           << Percentile(scenario.latency_microseconds, 0.50) << ",\n";
    output << "      \"p95_latency_us\": "
           << Percentile(scenario.latency_microseconds, 0.95) << ",\n";
    output << "      \"p99_latency_us\": "
           << Percentile(scenario.latency_microseconds, 0.99) << "\n";
    output << "    }" << (index + 1U == scenarios.size() ? "\n" : ",\n");
  }
  output << "  ]\n";
  output << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::size_t message_count = 5000U;
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--messages" && index + 1 < argc) {
        message_count =
            static_cast<std::size_t>(std::stoull(argv[++index]));
      } else if (argument == "--output" && index + 1 < argc) {
        output_path = argv[++index];
      } else {
        throw std::invalid_argument(
            "usage: dds_qos_experiment [--messages N] [--output FILE]");
      }
    }
    Require(message_count > 0U &&
                message_count <=
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max()),
            "message count is out of range");

    const auto domain_base =
        static_cast<std::uint32_t>(40 + (::getpid() % 120));
    std::vector<Scenario> scenarios;
    scenarios.push_back(RunScenario(
        "reliable_matched", domain_base,
        autoruntime::Reliability::Reliable,
        autoruntime::Reliability::Reliable, message_count));
    scenarios.push_back(RunScenario(
        "best_effort_matched", domain_base + 1U,
        autoruntime::Reliability::BestEffort,
        autoruntime::Reliability::BestEffort, message_count));
    scenarios.push_back(RunScenario(
        "best_effort_writer_reliable_reader", domain_base + 2U,
        autoruntime::Reliability::BestEffort,
        autoruntime::Reliability::Reliable,
        std::min<std::size_t>(message_count, 500U)));

    Require(scenarios[0].received > 0U,
            "Reliable matched scenario delivered no samples");
    Require(scenarios[1].received > 0U,
            "BestEffort matched scenario delivered no samples");
    Require(scenarios[2].received == 0U,
            "incompatible BestEffort writer matched a Reliable reader");

    if (output_path.empty()) {
      WriteJson(std::cout, scenarios);
    } else {
      std::ofstream output(output_path);
      Require(static_cast<bool>(output), "failed to open output file");
      WriteJson(output, scenarios);
      Require(static_cast<bool>(output), "failed to write output file");
      std::cout << output_path << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "dds_qos_experiment: " << error.what() << '\n';
    return 1;
  }
}
