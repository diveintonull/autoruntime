#include <autoruntime/executor.hpp>
#include <autoruntime/in_memory_transport.hpp>
#include <autoruntime/node.hpp>
#include <autoruntime/observability.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

void Require(bool condition, std::string_view detail) {
  if (!condition) {
    throw std::runtime_error(std::string(detail));
  }
}

void RequireStatus(const autoruntime::Status& status,
                   std::string_view operation) {
  if (!status) {
    throw std::runtime_error(std::string(operation) + ": " +
                             status.detail());
  }
}

template <typename T>
T Take(autoruntime::Result<T> result, std::string_view operation) {
  if (!result) {
    throw std::runtime_error(std::string(operation) + ": " +
                             result.status().detail());
  }
  return std::move(result).take_value();
}

autoruntime::SubscriptionOptions SubscriptionOptions(
    autoruntime::CallbackGroupId group, std::size_t capacity) {
  autoruntime::SubscriptionOptions options;
  options.callback_group = group;
  options.queue_capacity = capacity;
  options.overflow_policy = autoruntime::OverflowPolicy::DropNewest;
  options.qos.reliability = autoruntime::Reliability::Reliable;
  options.qos.history = autoruntime::HistoryKind::KeepLast;
  options.qos.depth = capacity;
  options.qos.deadline = 100ms;
  return options;
}

void AddExecutorMetrics(autoruntime::MetricsRegistry& metrics,
                        const autoruntime::RuntimeStats& snapshot) {
  std::uint64_t releases = 0U;
  std::uint64_t started = 0U;
  std::uint64_t finished = 0U;
  std::uint64_t overflows = 0U;
  std::uint64_t deadline_misses = 0U;
  std::uint64_t failures = 0U;
  for (const auto& task : snapshot.tasks) {
    releases += task.releases;
    started += task.started;
    finished += task.finished;
    overflows += task.queue_overflows;
    deadline_misses += task.deadline_misses;
    failures += task.callback_failures;
    for (const auto& sample : task.samples) {
      static_cast<void>(metrics.Observe(
          "executor.callback_execution_us",
          static_cast<double>(sample.execution_time.count()) / 1000.0));
      static_cast<void>(metrics.Observe(
          "executor.queue_delay_us",
          static_cast<double>(sample.queue_delay.count()) / 1000.0));
      static_cast<void>(metrics.Observe(
          "executor.response_time_us",
          static_cast<double>(sample.response_time.count()) / 1000.0));
      static_cast<void>(metrics.Observe(
          "executor.release_lateness_us",
          static_cast<double>(sample.release_lateness.count()) / 1000.0));
    }
  }
  static_cast<void>(metrics.IncrementCounter(
      "executor.releases", static_cast<double>(releases)));
  static_cast<void>(metrics.IncrementCounter(
      "executor.started", static_cast<double>(started)));
  static_cast<void>(metrics.IncrementCounter(
      "executor.finished", static_cast<double>(finished)));
  static_cast<void>(metrics.IncrementCounter(
      "executor.queue_overflows", static_cast<double>(overflows)));
  static_cast<void>(metrics.IncrementCounter(
      "executor.deadline_misses", static_cast<double>(deadline_misses)));
  static_cast<void>(metrics.IncrementCounter(
      "executor.callback_failures", static_cast<double>(failures)));
}

void WriteDistribution(std::ostream& output,
                       const autoruntime::DistributionSummary& summary) {
  output << "{\"count\":" << summary.count
         << ",\"sampled_count\":" << summary.sampled_count
         << ",\"min_us\":" << summary.minimum
         << ",\"max_us\":" << summary.maximum
         << ",\"mean_us\":" << summary.mean
         << ",\"p50_us\":" << summary.p50
         << ",\"p95_us\":" << summary.p95
         << ",\"p99_us\":" << summary.p99 << '}';
}

struct BenchmarkResult {
  std::size_t message_count{0U};
  std::size_t payload_bytes{0U};
  std::uint64_t completed{0U};
  std::uint64_t trace_failures{0U};
  std::uint64_t publish_failures{0U};
  std::uint64_t trace_spans{0U};
  double duration_seconds{0.0};
  double throughput_messages_per_second{0.0};
  autoruntime::DistributionSummary e2e;
  autoruntime::ProcessCpuMetrics cpu;
  std::string metrics_json;
  std::vector<std::string> logs;
};

BenchmarkResult Run(std::size_t message_count,
                    std::size_t payload_bytes) {
  BenchmarkResult result;
  result.message_count = message_count;
  result.payload_bytes = payload_bytes;

  auto executor = std::make_shared<autoruntime::Executor>();
  auto transport = std::make_shared<autoruntime::InMemoryTransport>();
  const auto planning_group = Take(
      executor->CreateCallbackGroup(
          autoruntime::CallbackGroupConfig{"planning", 1U, 64U}),
      "create planning callback group");
  const auto control_group = Take(
      executor->CreateCallbackGroup(
          autoruntime::CallbackGroupConfig{"control", 1U, 64U}),
      "create control callback group");

  autoruntime::Node sensor({"sensor", 1U}, executor, transport);
  autoruntime::Node planning({"planning", 1U}, executor, transport);
  autoruntime::Node control({"control", 1U}, executor, transport);
  autoruntime::MetricsRegistry metrics(
      std::max<std::size_t>(message_count * 8U, 8192U));
  autoruntime::TraceRecorder traces;
  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  std::atomic<std::uint64_t> completed{0U};
  std::atomic<std::uint64_t> checksum{0U};
  std::atomic<std::uint64_t> trace_failures{0U};
  std::atomic<std::uint64_t> publish_failures{0U};

  std::mutex log_mutex;
  autoruntime::StructuredLogger logger([&](std::string line) {
    std::lock_guard lock(log_mutex);
    result.logs.push_back(std::move(line));
  });
  RequireStatus(logger.Log({autoruntime::LogLevel::Info,
                            "pipeline_benchmark",
                            "started",
                            1U,
                            0U,
                            0U,
                            autoruntime::StatusCode::Ok,
                            "sensor-planning-control run started",
                            {{"messages", std::to_string(message_count)},
                             {"payload_bytes",
                              std::to_string(payload_bytes)}}}),
                "write start log");

  auto planning_publisher = Take(
      planning.CreatePublisher("planning/output"),
      "create planning publisher");

  auto control_subscriber = Take(
      control.CreateSubscriber(
          "planning/output",
          SubscriptionOptions(control_group, message_count + 1U),
          [&](const autoruntime::Message& message) {
            const auto callback_start = autoruntime::MonotonicNanoseconds();
            std::uint64_t local_checksum = 0U;
            for (const auto value : message.payload) {
              local_checksum += std::to_integer<unsigned int>(value);
            }
            checksum.fetch_add(local_checksum, std::memory_order_relaxed);
            const auto finish = autoruntime::MonotonicNanoseconds();
            const auto trace_status = traces.Record(
                {message.envelope.trace_id,
                 message.envelope.span_id,
                 message.envelope.parent_span_id,
                 message.envelope.source_generation,
                 "control",
                 "receive_and_apply",
                 std::min(message.envelope.publish_timestamp_ns,
                          callback_start),
                 finish,
                 autoruntime::StatusCode::Ok});
            if (!trace_status) {
              trace_failures.fetch_add(1U, std::memory_order_relaxed);
            }
            const auto origin = message.envelope.source_timestamp_ns;
            const auto e2e_ns = finish >= origin ? finish - origin : 0U;
            static_cast<void>(metrics.Observe(
                "pipeline.e2e_latency_us",
                static_cast<double>(e2e_ns) / 1000.0));
            completed.fetch_add(1U, std::memory_order_release);
            completion_cv.notify_all();
          }),
      "create control subscriber");

  auto planning_subscriber = Take(
      planning.CreateSubscriber(
          "sensor/input",
          SubscriptionOptions(planning_group, message_count + 1U),
          [&](const autoruntime::Message& message) {
            const auto start = autoruntime::MonotonicNanoseconds();
            const auto sensor_status = traces.Record(
                {message.envelope.trace_id,
                 message.envelope.span_id,
                 message.envelope.parent_span_id,
                 message.envelope.source_generation,
                 "sensor",
                 "publish_to_planning",
                 message.envelope.source_timestamp_ns,
                 start,
                 autoruntime::StatusCode::Ok});
            if (!sensor_status) {
              trace_failures.fetch_add(1U, std::memory_order_relaxed);
            }

            const auto planning_span_id = autoruntime::NextSpanId();
            const auto publish_status = planning_publisher.Publish(
                message.payload,
                {message.envelope.trace_id, planning_span_id,
                 message.envelope.source_timestamp_ns});
            const auto finish = autoruntime::MonotonicNanoseconds();
            const auto planning_status = traces.Record(
                {message.envelope.trace_id,
                 planning_span_id,
                 message.envelope.span_id,
                 message.envelope.source_generation,
                 "planning",
                 "process_and_publish",
                 start,
                 finish,
                 publish_status.code()});
            if (!planning_status) {
              trace_failures.fetch_add(1U, std::memory_order_relaxed);
            }
            if (!publish_status) {
              publish_failures.fetch_add(1U, std::memory_order_relaxed);
            }
          }),
      "create planning subscriber");

  auto sensor_publisher = Take(sensor.CreatePublisher("sensor/input"),
                               "create sensor publisher");
  RequireStatus(executor->Start(), "start executor");
  const std::vector<std::byte> payload(payload_bytes, std::byte{0x5A});
  const auto started_at = std::chrono::steady_clock::now();
  for (std::size_t index = 0U; index < message_count; ++index) {
    static_cast<void>(index);
    const auto publish_status = sensor_publisher.Publish(payload);
    if (!publish_status) {
      publish_failures.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  {
    std::unique_lock lock(completion_mutex);
    const auto completed_all = completion_cv.wait_for(
        lock, 30s, [&] {
          return completed.load(std::memory_order_acquire) >= message_count;
        });
    Require(completed_all, "pipeline did not complete before timeout");
  }
  const auto finished_at = std::chrono::steady_clock::now();
  result.duration_seconds =
      std::chrono::duration<double>(finished_at - started_at).count();
  result.completed = completed.load(std::memory_order_acquire);
  result.trace_failures =
      trace_failures.load(std::memory_order_acquire);
  result.publish_failures =
      publish_failures.load(std::memory_order_acquire);
  result.throughput_messages_per_second =
      result.duration_seconds > 0.0
          ? static_cast<double>(result.completed) / result.duration_seconds
          : 0.0;

  const auto planning_stats = planning_subscriber.Stats();
  const auto control_stats = control_subscriber.Stats();
  const auto transport_stats = transport->Stats();
  const auto executor_stats = executor->Snapshot();
  RequireStatus(executor->Stop(autoruntime::Deadline::After(2s)),
                "stop executor");

  const auto spans = traces.Snapshot();
  result.trace_spans = static_cast<std::uint64_t>(spans.size());
  result.e2e = traces.EndToEndLatencyMicroseconds();
  result.cpu = autoruntime::SampleProcessCpu();

  static_cast<void>(metrics.SetGauge("node.sensor.state_running", 1.0));
  static_cast<void>(metrics.SetGauge("node.planning.state_running", 1.0));
  static_cast<void>(metrics.SetGauge("node.control.state_running", 1.0));
  static_cast<void>(metrics.SetGauge(
      "pipeline.message_rate_per_second",
      result.throughput_messages_per_second));
  static_cast<void>(metrics.SetGauge(
      "planning.queue_high_watermark",
      static_cast<double>(planning_stats.queue_high_watermark)));
  static_cast<void>(metrics.SetGauge(
      "control.queue_high_watermark",
      static_cast<double>(control_stats.queue_high_watermark)));
  static_cast<void>(metrics.IncrementCounter(
      "planning.dropped_messages",
      static_cast<double>(planning_stats.dropped_messages)));
  static_cast<void>(metrics.IncrementCounter(
      "control.dropped_messages",
      static_cast<double>(control_stats.dropped_messages)));
  static_cast<void>(metrics.IncrementCounter(
      "transport.published_messages",
      static_cast<double>(transport_stats.published_messages)));
  static_cast<void>(metrics.IncrementCounter(
      "transport.delivered_messages",
      static_cast<double>(transport_stats.delivered_messages)));
  static_cast<void>(metrics.IncrementCounter(
      "transport.dropped_messages",
      static_cast<double>(transport_stats.dropped_messages)));
  static_cast<void>(metrics.IncrementCounter(
      "pipeline.publish_failures",
      static_cast<double>(result.publish_failures)));
  static_cast<void>(metrics.IncrementCounter(
      "pipeline.trace_failures",
      static_cast<double>(result.trace_failures)));
  static_cast<void>(metrics.SetGauge(
      "process.maximum_resident_bytes",
      static_cast<double>(result.cpu.maximum_resident_bytes)));
  static_cast<void>(metrics.SetGauge(
      "process.user_cpu_seconds", result.cpu.user_seconds));
  static_cast<void>(metrics.SetGauge(
      "process.system_cpu_seconds", result.cpu.system_seconds));
  static_cast<void>(metrics.SetGauge(
      "pipeline.payload_checksum",
      static_cast<double>(checksum.load(std::memory_order_relaxed))));
  AddExecutorMetrics(metrics, executor_stats);

  Require(result.completed == message_count,
          "not every sensor message reached control");
  Require(result.publish_failures == 0U, "pipeline publish failed");
  Require(result.trace_failures == 0U, "pipeline trace recording failed");
  Require(result.e2e.count == message_count,
          "trace count does not match message count");
  Require(result.trace_spans == message_count * 3U,
          "expected three spans per message");

  RequireStatus(logger.Log({autoruntime::LogLevel::Info,
                            "pipeline_benchmark",
                            "completed",
                            1U,
                            0U,
                            0U,
                            autoruntime::StatusCode::Ok,
                            "all messages reached control",
                            {{"completed", std::to_string(result.completed)},
                             {"p99_us", std::to_string(result.e2e.p99)}}}),
                "write completion log");
  result.metrics_json = metrics.RenderJson();
  return result;
}

void WriteJson(std::ostream& output, const BenchmarkResult& result) {
  output << std::fixed << std::setprecision(3);
  output << "{\n";
  output << "  \"benchmark\": \"sensor_planning_control\",\n";
  output << "  \"transport\": \"InMemoryTransport\",\n";
  output << "  \"message_count\": " << result.message_count << ",\n";
  output << "  \"payload_bytes\": " << result.payload_bytes << ",\n";
  output << "  \"completed\": " << result.completed << ",\n";
  output << "  \"duration_seconds\": " << result.duration_seconds << ",\n";
  output << "  \"throughput_messages_per_second\": "
         << result.throughput_messages_per_second << ",\n";
  output << "  \"trace_spans\": " << result.trace_spans << ",\n";
  output << "  \"trace_failures\": " << result.trace_failures << ",\n";
  output << "  \"publish_failures\": " << result.publish_failures << ",\n";
  output << "  \"e2e_latency\": ";
  WriteDistribution(output, result.e2e);
  output << ",\n";
  output << "  \"process\": {\"available\":"
         << (result.cpu.available ? "true" : "false")
         << ",\"user_seconds\":" << result.cpu.user_seconds
         << ",\"system_seconds\":" << result.cpu.system_seconds
         << ",\"maximum_resident_bytes\":"
         << result.cpu.maximum_resident_bytes
         << ",\"voluntary_context_switches\":"
         << result.cpu.voluntary_context_switches
         << ",\"involuntary_context_switches\":"
         << result.cpu.involuntary_context_switches << "},\n";
  output << "  \"metrics_snapshot\": " << result.metrics_json << ",\n";
  output << "  \"structured_logs\": [\n";
  for (std::size_t index = 0U; index < result.logs.size(); ++index) {
    output << "    " << result.logs[index]
           << (index + 1U == result.logs.size() ? "\n" : ",\n");
  }
  output << "  ]\n";
  output << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::size_t message_count = 5000U;
    std::size_t payload_bytes = 256U;
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--messages" && index + 1 < argc) {
        message_count =
            static_cast<std::size_t>(std::stoull(argv[++index]));
      } else if (argument == "--payload-bytes" && index + 1 < argc) {
        payload_bytes =
            static_cast<std::size_t>(std::stoull(argv[++index]));
      } else if (argument == "--output" && index + 1 < argc) {
        output_path = argv[++index];
      } else {
        throw std::invalid_argument(
            "usage: pipeline_latency_benchmark [--messages N] "
            "[--payload-bytes N] [--output FILE]");
      }
    }
    Require(message_count > 0U && message_count <= 100000U,
            "message count must be in [1, 100000]");
    Require(payload_bytes > 0U && payload_bytes <= 1024U * 1024U,
            "payload bytes must be in [1, 1048576]");
    Require(message_count <=
                std::numeric_limits<std::size_t>::max() / 8U,
            "message count overflows metrics capacity");

    const auto result = Run(message_count, payload_bytes);
    if (output_path.empty()) {
      WriteJson(std::cout, result);
    } else {
      std::ofstream output(output_path);
      Require(static_cast<bool>(output), "failed to open output file");
      WriteJson(output, result);
      Require(static_cast<bool>(output), "failed to write output file");
      std::cout << output_path << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pipeline_latency_benchmark: " << error.what() << '\n';
    return 1;
  }
}
