#include <rclcpp/rclcpp.hpp>
#include <rmw/rmw.h>
#include <std_msgs/msg/byte_multi_array.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cerrno>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef RCLCPP_BASELINE_SOURCE_REVISION
#define RCLCPP_BASELINE_SOURCE_REVISION "unknown"
#endif

#ifndef RCLCPP_BASELINE_COMPILER
#define RCLCPP_BASELINE_COMPILER "unknown"
#endif

#ifndef RCLCPP_BASELINE_BUILD_TYPE
#define RCLCPP_BASELINE_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Message = std_msgs::msg::ByteMultiArray;
using namespace std::chrono_literals;

constexpr std::uint32_t kSchemaVersion = 1U;
constexpr std::size_t kMaximumPayloadBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMebibyte = 1024U * 1024U;
constexpr std::uint64_t kWarmupSequenceMask = 1ULL << 63U;
constexpr std::size_t kQueueDepth = 64U;
constexpr auto kControlTimeout = 10s;
constexpr auto kDrainTimeout = 5s;

struct Options {
  std::vector<std::size_t> payload_sizes{
      64U, 1024U, 64U * 1024U, 1024U * 1024U};
  std::uint32_t frequency_hz{100U};
  std::chrono::milliseconds duration{3000};
  std::size_t warmup_messages{20U};
  std::size_t trials{3U};
  std::string run_id;
  std::string output_path;
  bool self_test{false};
};

struct CpuSnapshot {
  bool available{false};
  double user_seconds{0.0};
  double system_seconds{0.0};
  std::uint64_t maximum_resident_bytes{0U};
  std::uint64_t voluntary_context_switches{0U};
  std::uint64_t involuntary_context_switches{0U};
};

struct ChildCounters {
  std::atomic<std::uint64_t> requests_received{0U};
  std::atomic<std::uint64_t> payload_mismatches{0U};
  std::atomic<std::uint64_t> response_publish_failures{0U};
};

struct ChildReport {
  std::uint64_t requests_received{0U};
  std::uint64_t payload_mismatches{0U};
  std::uint64_t response_publish_failures{0U};
  double user_cpu_seconds{0.0};
  double system_cpu_seconds{0.0};
  std::uint64_t voluntary_context_switches{0U};
  std::uint64_t involuntary_context_switches{0U};
  std::uint64_t maximum_resident_bytes{0U};
};

struct StateSnapshot {
  std::uint64_t completed_round_trips{0U};
  std::uint64_t response_payload_mismatches{0U};
  std::uint64_t unexpected_responses{0U};
  std::vector<double> latencies_us;
};

struct CaseResult {
  std::string run_id;
  std::uint64_t case_id{0U};
  std::size_t trial{0U};
  std::size_t payload_bytes{0U};
  std::uint32_t frequency_hz{0U};
  std::uint64_t configured_duration_ms{0U};
  std::uint64_t scheduled_requests{0U};
  std::uint64_t published_requests{0U};
  std::uint64_t request_publish_failures{0U};
  std::uint64_t completed_round_trips{0U};
  std::uint64_t lost_responses{0U};
  std::uint64_t responder_requests{0U};
  std::uint64_t responder_payload_mismatches{0U};
  std::uint64_t responder_publish_failures{0U};
  std::uint64_t response_payload_mismatches{0U};
  std::uint64_t unexpected_responses{0U};
  std::uint64_t logical_payload_bytes{0U};
  double send_window_ms{0.0};
  double drain_ms{0.0};
  double completed_round_trips_per_second{0.0};
  double logical_payload_mib_per_second{0.0};
  double p50_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
  double p99_9_us{0.0};
  double max_us{0.0};
  double user_cpu_ms{0.0};
  double system_cpu_ms{0.0};
  double cpu_utilization_percent{0.0};
  std::uint64_t voluntary_context_switches{0U};
  std::uint64_t involuntary_context_switches{0U};
  std::uint64_t initiator_peak_rss_kib{0U};
  std::uint64_t responder_peak_rss_kib{0U};

  [[nodiscard]] bool ok() const noexcept {
    return scheduled_requests == published_requests &&
           request_publish_failures == 0U &&
           completed_round_trips == published_requests &&
           lost_responses == 0U &&
           responder_requests == published_requests &&
           responder_payload_mismatches == 0U &&
           responder_publish_failures == 0U &&
           response_payload_mismatches == 0U &&
           unexpected_responses == 0U;
  }
};

[[nodiscard]] std::uint64_t ParseUnsigned(
    std::string_view text, std::string_view option) {
  std::uint64_t value = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument(
        std::string(option) + " requires an unsigned integer");
  }
  return value;
}

[[nodiscard]] std::string_view OptionValue(
    int& index, int argc, char** argv, std::string_view argument,
    std::string_view option) {
  const std::string prefix = std::string(option) + "=";
  if (argument.starts_with(prefix)) {
    const auto value = argument.substr(prefix.size());
    if (value.empty()) {
      throw std::invalid_argument(
          std::string(option) + " requires a value");
    }
    return value;
  }
  if (argument == option && index + 1 < argc) {
    return std::string_view(argv[++index]);
  }
  throw std::invalid_argument(
      "unknown or incomplete argument: " + std::string(argument));
}

[[nodiscard]] Options ParseOptions(int argc, char** argv) {
  Options options;
  bool explicit_payload = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      std::cout
          << "usage: rclcpp_comparative_benchmark "
             "[--payload BYTES] [--frequency HZ] "
             "[--duration-ms MS] [--warmup N] [--trials N] "
             "[--run-id ID] [--output FILE] [--self-test]\n";
      std::exit(0);
    }
    if (argument == "--self-test") {
      options.self_test = true;
      continue;
    }
    if (argument == "--payload" ||
        argument.starts_with("--payload=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--payload"),
          "--payload");
      if (value < sizeof(std::uint64_t) ||
          value > kMaximumPayloadBytes) {
        throw std::invalid_argument(
            "--payload must be between 8 and 4194304 bytes");
      }
      if (!explicit_payload) {
        options.payload_sizes.clear();
        explicit_payload = true;
      }
      options.payload_sizes.push_back(
          static_cast<std::size_t>(value));
      continue;
    }
    if (argument == "--frequency" ||
        argument.starts_with("--frequency=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--frequency"),
          "--frequency");
      if (value == 0U || value > 10'000U) {
        throw std::invalid_argument(
            "--frequency must be between 1 and 10000");
      }
      options.frequency_hz = static_cast<std::uint32_t>(value);
      continue;
    }
    if (argument == "--duration-ms" ||
        argument.starts_with("--duration-ms=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--duration-ms"),
          "--duration-ms");
      if (value == 0U || value > 600'000U) {
        throw std::invalid_argument(
            "--duration-ms must be between 1 and 600000");
      }
      options.duration =
          std::chrono::milliseconds(static_cast<std::int64_t>(value));
      continue;
    }
    if (argument == "--warmup" ||
        argument.starts_with("--warmup=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--warmup"),
          "--warmup");
      if (value > 10'000U) {
        throw std::invalid_argument("--warmup must not exceed 10000");
      }
      options.warmup_messages = static_cast<std::size_t>(value);
      continue;
    }
    if (argument == "--trials" ||
        argument.starts_with("--trials=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--trials"),
          "--trials");
      if (value == 0U || value > 100U) {
        throw std::invalid_argument(
            "--trials must be between 1 and 100");
      }
      options.trials = static_cast<std::size_t>(value);
      continue;
    }
    if (argument == "--run-id" ||
        argument.starts_with("--run-id=")) {
      options.run_id = std::string(
          OptionValue(index, argc, argv, argument, "--run-id"));
      continue;
    }
    if (argument == "--output" ||
        argument.starts_with("--output=")) {
      options.output_path = std::string(
          OptionValue(index, argc, argv, argument, "--output"));
      continue;
    }
    throw std::invalid_argument(
        "unknown argument: " + std::string(argument));
  }
  if (options.self_test) {
    if (!explicit_payload) {
      options.payload_sizes = {64U};
    }
    options.frequency_hz = 50U;
    options.duration = 200ms;
    options.warmup_messages = 3U;
    options.trials = 1U;
  }
  return options;
}

[[nodiscard]] CpuSnapshot SampleCpu() noexcept {
  CpuSnapshot result;
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    return result;
  }
  result.available = true;
  result.user_seconds =
      static_cast<double>(usage.ru_utime.tv_sec) +
      static_cast<double>(usage.ru_utime.tv_usec) / 1'000'000.0;
  result.system_seconds =
      static_cast<double>(usage.ru_stime.tv_sec) +
      static_cast<double>(usage.ru_stime.tv_usec) / 1'000'000.0;
  result.maximum_resident_bytes =
      static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
  result.voluntary_context_switches =
      static_cast<std::uint64_t>(usage.ru_nvcsw);
  result.involuntary_context_switches =
      static_cast<std::uint64_t>(usage.ru_nivcsw);
  return result;
}

[[nodiscard]] double Delta(double after, double before) noexcept {
  return std::max(0.0, after - before);
}

[[nodiscard]] std::uint64_t Delta(
    std::uint64_t after, std::uint64_t before) noexcept {
  return after >= before ? after - before : 0U;
}

[[nodiscard]] std::string UtcNow() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream stream;
  stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

[[nodiscard]] std::string DefaultRunId() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream stream;
  stream << "comparative-" << std::put_time(&utc, "%Y%m%dT%H%M%SZ")
         << '-' << ::getpid();
  return stream.str();
}

[[nodiscard]] std::string JsonEscape(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char value : text) {
    switch (value) {
      case '\\':
        result.append("\\\\");
        break;
      case '"':
        result.append("\\\"");
        break;
      case '\n':
        result.append("\\n");
        break;
      case '\r':
        result.append("\\r");
        break;
      case '\t':
        result.append("\\t");
        break;
      default:
        result.push_back(value);
        break;
    }
  }
  return result;
}

[[nodiscard]] std::string EnvironmentJson(const Options& options) {
  char hostname[256]{};
  static_cast<void>(::gethostname(hostname, sizeof(hostname)));
  utsname system{};
  static_cast<void>(::uname(&system));
  const char* rmw = std::getenv("RMW_IMPLEMENTATION");
  const char* actual_rmw = rmw_get_implementation_identifier();
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  std::ostringstream stream;
  stream << "{\"schema_version\":" << kSchemaVersion
         << ",\"record_type\":\"environment\""
         << ",\"benchmark\":\"autoruntime_comparative\""
         << ",\"run_id\":\"" << JsonEscape(options.run_id) << "\""
         << ",\"timestamp_utc\":\"" << UtcNow() << "\""
         << ",\"hostname\":\"" << JsonEscape(hostname) << "\""
         << ",\"kernel_release\":\""
         << JsonEscape(system.release) << "\""
         << ",\"machine\":\"" << JsonEscape(system.machine) << "\""
         << ",\"logical_cpus\":" << std::thread::hardware_concurrency()
         << ",\"compiler\":\"" << RCLCPP_BASELINE_COMPILER << "\""
         << ",\"build_type\":\"" << RCLCPP_BASELINE_BUILD_TYPE << "\""
         << ",\"source_revision\":\""
         << RCLCPP_BASELINE_SOURCE_REVISION << "\""
         << ",\"rmw_implementation\":\""
         << JsonEscape(
                actual_rmw == nullptr ? "unknown" : actual_rmw)
         << "\""
         << ",\"requested_rmw_implementation\":\""
         << JsonEscape(rmw == nullptr ? "default" : rmw) << "\""
         << ",\"ros_domain_id\":\""
         << JsonEscape(domain == nullptr ? "default" : domain) << "\""
         << ",\"topology\":\"two_process_request_echo\""
         << ",\"clock\":\"steady_clock_round_trip\""
         << ",\"qos\":{\"reliability\":\"reliable\","
            "\"history\":\"keep_last\",\"depth\":"
         << kQueueDepth << "}"
         << ",\"payload_validation\":\"full_touch\""
         << ",\"frequency_hz\":" << options.frequency_hz
         << ",\"duration_ms\":" << options.duration.count()
         << "}";
  return stream.str();
}

[[nodiscard]] double Percentile(
    const std::vector<double>& sorted, double quantile) {
  if (sorted.empty()) {
    return 0.0;
  }
  const auto rank = static_cast<std::size_t>(
      std::ceil(quantile * static_cast<double>(sorted.size())));
  const auto index =
      std::min(sorted.size() - 1U, std::max<std::size_t>(1U, rank) - 1U);
  return sorted[index];
}

[[nodiscard]] std::string ResultJson(const CaseResult& result) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3)
         << "{\"schema_version\":" << kSchemaVersion
         << ",\"record_type\":\"result\""
         << ",\"benchmark\":\"autoruntime_comparative\""
         << ",\"run_id\":\"" << JsonEscape(result.run_id) << "\""
         << ",\"case_id\":" << result.case_id
         << ",\"trial\":" << result.trial
         << ",\"status\":\"" << (result.ok() ? "ok" : "failed") << "\""
         << ",\"implementation\":\"rclcpp\""
         << ",\"mode\":\"rclcpp_dds\""
         << ",\"comparison_group\":"
            "\"same_host_two_process_request_echo\""
         << ",\"topology\":\"two_process_request_echo\""
         << ",\"payload_validation\":\"full_touch\""
         << ",\"payload_bytes\":" << result.payload_bytes
         << ",\"frequency_hz\":" << result.frequency_hz
         << ",\"configured_duration_ms\":"
         << result.configured_duration_ms
         << ",\"scheduled_requests\":" << result.scheduled_requests
         << ",\"published_requests\":" << result.published_requests
         << ",\"request_publish_failures\":"
         << result.request_publish_failures
         << ",\"completed_round_trips\":"
         << result.completed_round_trips
         << ",\"lost_responses\":" << result.lost_responses
         << ",\"responder_requests\":" << result.responder_requests
         << ",\"responder_payload_mismatches\":"
         << result.responder_payload_mismatches
         << ",\"responder_publish_failures\":"
         << result.responder_publish_failures
         << ",\"response_payload_mismatches\":"
         << result.response_payload_mismatches
         << ",\"unexpected_responses\":"
         << result.unexpected_responses
         << ",\"logical_payload_bytes\":"
         << result.logical_payload_bytes
         << ",\"send_window_ms\":" << result.send_window_ms
         << ",\"drain_ms\":" << result.drain_ms
         << ",\"completed_round_trips_per_second\":"
         << result.completed_round_trips_per_second
         << ",\"logical_payload_mib_per_second\":"
         << result.logical_payload_mib_per_second
         << ",\"latency_us\":{\"p50\":" << result.p50_us
         << ",\"p95\":" << result.p95_us
         << ",\"p99\":" << result.p99_us
         << ",\"p99_9\":" << result.p99_9_us
         << ",\"max\":" << result.max_us << "}"
         << ",\"resources\":{\"user_cpu_ms\":"
         << result.user_cpu_ms
         << ",\"system_cpu_ms\":" << result.system_cpu_ms
         << ",\"cpu_utilization_percent\":"
         << result.cpu_utilization_percent
         << ",\"voluntary_context_switches\":"
         << result.voluntary_context_switches
         << ",\"involuntary_context_switches\":"
         << result.involuntary_context_switches
         << ",\"initiator_peak_rss_kib\":"
         << result.initiator_peak_rss_kib
         << ",\"responder_peak_rss_kib\":"
         << result.responder_peak_rss_kib << "}}";
  return stream.str();
}

[[nodiscard]] std::uint8_t PatternByte(
    std::uint64_t sequence, std::size_t index) noexcept {
  return static_cast<std::uint8_t>(
      (sequence + static_cast<std::uint64_t>(index) * 131U + 17U) %
      251U);
}

void PreparePayload(
    std::vector<std::uint8_t>& payload,
    std::uint64_t sequence) noexcept {
  std::memcpy(payload.data(), &sequence, sizeof(sequence));
  for (std::size_t index = sizeof(sequence);
       index < payload.size(); ++index) {
    payload[index] = PatternByte(sequence, index);
  }
}

[[nodiscard]] std::optional<std::uint64_t> ValidatePayload(
    std::span<const std::uint8_t> payload) noexcept {
  if (payload.size() < sizeof(std::uint64_t)) {
    return std::nullopt;
  }
  std::uint64_t sequence = 0U;
  std::memcpy(&sequence, payload.data(), sizeof(sequence));
  for (std::size_t index = sizeof(sequence);
       index < payload.size(); ++index) {
    if (payload[index] != PatternByte(sequence, index)) {
      return std::nullopt;
    }
  }
  return sequence;
}

[[nodiscard]] bool WriteExact(
    int descriptor, const void* data, std::size_t size) noexcept {
  const auto* cursor = static_cast<const std::byte*>(data);
  while (size != 0U) {
    const auto written = ::write(descriptor, cursor, size);
    if (written > 0) {
      const auto count = static_cast<std::size_t>(written);
      cursor += count;
      size -= count;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool ReadExact(
    int descriptor, void* data, std::size_t size,
    std::chrono::milliseconds timeout) noexcept {
  auto* cursor = static_cast<std::byte*>(data);
  const auto deadline = Clock::now() + timeout;
  while (size != 0U) {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
      return false;
    }
    const auto wait_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    pollfd event{descriptor, POLLIN, 0};
    const int ready =
        ::poll(&event, 1, std::max(1, static_cast<int>(wait_ms.count())));
    if (ready == 0) {
      continue;
    }
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0 || (event.revents & (POLLERR | POLLNVAL)) != 0) {
      return false;
    }
    const auto count = ::read(descriptor, cursor, size);
    if (count > 0) {
      const auto converted = static_cast<std::size_t>(count);
      cursor += converted;
      size -= converted;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

class InitiatorState {
 public:
  void Expect(std::uint64_t sequence, bool measured) {
    std::lock_guard lock(mutex_);
    pending_.emplace(sequence, Pending{Clock::now(), measured});
  }

  void Forget(std::uint64_t sequence) {
    std::lock_guard lock(mutex_);
    pending_.erase(sequence);
    warmup_outcomes_.erase(sequence);
    condition_.notify_all();
  }

  void OnResponse(const Message::ConstSharedPtr& message) {
    const auto sequence = ValidatePayload(message->data);
    std::lock_guard lock(mutex_);
    if (!sequence) {
      ++response_payload_mismatches_;
      condition_.notify_all();
      return;
    }
    const auto iterator = pending_.find(*sequence);
    if (iterator == pending_.end()) {
      if ((*sequence & kWarmupSequenceMask) == 0U) {
        ++unexpected_responses_;
      }
      condition_.notify_all();
      return;
    }
    const auto pending = iterator->second;
    pending_.erase(iterator);
    if (pending.measured) {
      latencies_us_.push_back(
          std::chrono::duration<double, std::micro>(
              Clock::now() - pending.sent_at)
              .count());
      ++completed_round_trips_;
    } else {
      warmup_outcomes_[*sequence] = true;
    }
    condition_.notify_all();
  }

  [[nodiscard]] std::optional<bool> WaitWarmup(
      std::uint64_t sequence,
      std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, timeout, [&] {
      return warmup_outcomes_.contains(sequence);
    });
    const auto iterator = warmup_outcomes_.find(sequence);
    if (iterator == warmup_outcomes_.end()) {
      pending_.erase(sequence);
      return std::nullopt;
    }
    const bool result = iterator->second;
    warmup_outcomes_.erase(iterator);
    return result;
  }

  void BeginMeasurement() {
    std::lock_guard lock(mutex_);
    pending_.clear();
    warmup_outcomes_.clear();
    latencies_us_.clear();
    completed_round_trips_ = 0U;
    response_payload_mismatches_ = 0U;
    unexpected_responses_ = 0U;
  }

  [[nodiscard]] bool WaitForDrain(
      std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [&] {
      return std::none_of(
          pending_.begin(), pending_.end(),
          [](const auto& entry) {
            return entry.second.measured;
          });
    });
  }

  [[nodiscard]] StateSnapshot Snapshot() const {
    std::lock_guard lock(mutex_);
    return StateSnapshot{
        completed_round_trips_,
        response_payload_mismatches_,
        unexpected_responses_,
        latencies_us_};
  }

 private:
  struct Pending {
    Clock::time_point sent_at;
    bool measured{false};
  };
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::unordered_map<std::uint64_t, Pending> pending_;
  std::unordered_map<std::uint64_t, bool> warmup_outcomes_;
  std::vector<double> latencies_us_;
  std::uint64_t completed_round_trips_{0U};
  std::uint64_t response_payload_mismatches_{0U};
  std::uint64_t unexpected_responses_{0U};
};

class RosEndpoint {
 public:
  using Publisher = rclcpp::Publisher<Message>;
  using HandlerFactory = std::function<
      std::function<void(Message::ConstSharedPtr)>(
          std::shared_ptr<Publisher>)>;

  [[nodiscard]] static std::unique_ptr<RosEndpoint> Create(
      std::string node_name,
      const std::string& outgoing_topic,
      const std::string& incoming_topic,
      HandlerFactory handler_factory) {
    auto endpoint = std::unique_ptr<RosEndpoint>(new RosEndpoint());
    endpoint->context_ = std::make_shared<rclcpp::Context>();
    const char* arguments[] = {"rclcpp_comparative_benchmark"};
    endpoint->context_->init(1, arguments);
    rclcpp::NodeOptions node_options;
    node_options.context(endpoint->context_);
    endpoint->node_ =
        std::make_shared<rclcpp::Node>(std::move(node_name), node_options);
    auto qos = rclcpp::QoS(rclcpp::KeepLast(kQueueDepth)).reliable();
    endpoint->publisher_ =
        endpoint->node_->create_publisher<Message>(outgoing_topic, qos);
    auto handler = handler_factory(endpoint->publisher_);
    endpoint->subscription_ =
        endpoint->node_->create_subscription<Message>(
            incoming_topic, qos, std::move(handler));
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = endpoint->context_;
    endpoint->executor_ =
        std::make_unique<
            rclcpp::executors::SingleThreadedExecutor>(
            executor_options);
    endpoint->executor_->add_node(endpoint->node_);
    endpoint->spin_thread_ = std::jthread([state = endpoint.get()] {
      state->executor_->spin();
    });
    return endpoint;
  }

  ~RosEndpoint() { Close(); }

  void SendGenerated(
      std::size_t payload_bytes, std::uint64_t sequence) {
    Message message;
    message.data.resize(payload_bytes);
    PreparePayload(message.data, sequence);
    publisher_->publish(message);
  }

  void Close() noexcept {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (executor_) {
      executor_->cancel();
    }
    if (spin_thread_.joinable()) {
      spin_thread_.join();
    }
    subscription_.reset();
    publisher_.reset();
    if (executor_ && node_) {
      static_cast<void>(executor_->remove_node(node_));
    }
    node_.reset();
    executor_.reset();
    if (context_ && context_->is_valid()) {
      static_cast<void>(context_->shutdown("benchmark complete"));
    }
    context_.reset();
  }

 private:
  RosEndpoint() = default;
  bool closed_{false};
  std::shared_ptr<rclcpp::Context> context_;
  std::shared_ptr<rclcpp::Node> node_;
  std::shared_ptr<Publisher> publisher_;
  rclcpp::Subscription<Message>::SharedPtr subscription_;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::jthread spin_thread_;
};

[[nodiscard]] bool RunWarmup(
    RosEndpoint& endpoint, InitiatorState& state,
    std::size_t payload_bytes, std::size_t messages) {
  if (messages == 0U) {
    return true;
  }
  const auto deadline = Clock::now() + 10s;
  std::size_t completed = 0U;
  std::uint64_t attempt = 1U;
  while (completed < messages && Clock::now() < deadline) {
    const auto sequence = kWarmupSequenceMask | attempt++;
    state.Expect(sequence, false);
    endpoint.SendGenerated(payload_bytes, sequence);
    const auto outcome = state.WaitWarmup(sequence, 750ms);
    if (outcome && *outcome) {
      ++completed;
    } else {
      state.Forget(sequence);
      std::this_thread::sleep_for(20ms);
    }
  }
  std::this_thread::sleep_for(50ms);
  return completed == messages;
}

[[nodiscard]] int ChildMain(
    const std::string& request_topic,
    const std::string& response_topic,
    int parent_commands,
    int parent_events) {
  try {
    auto counters = std::make_shared<ChildCounters>();
    auto endpoint = RosEndpoint::Create(
        "rclcpp_comparative_responder",
        response_topic, request_topic,
        [counters](
            std::shared_ptr<RosEndpoint::Publisher> publisher) {
          return [counters, publisher = std::move(publisher)](
                     Message::ConstSharedPtr message) {
            const auto sequence = ValidatePayload(message->data);
            if (!sequence) {
              counters->payload_mismatches.fetch_add(
                  1U, std::memory_order_relaxed);
              return;
            }
            const bool measured =
                (*sequence & kWarmupSequenceMask) == 0U;
            if (measured) {
              counters->requests_received.fetch_add(
                  1U, std::memory_order_relaxed);
            }
            try {
              Message response;
              response.data = message->data;
              publisher->publish(response);
            } catch (const std::exception&) {
              if (measured) {
                counters->response_publish_failures.fetch_add(
                    1U, std::memory_order_relaxed);
              }
            }
          };
        });
    const char producer_ready = 'P';
    if (!WriteExact(parent_events, &producer_ready, 1U)) {
      return 31;
    }
    char command = 0;
    if (!ReadExact(
            parent_commands, &command, 1U,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                kControlTimeout)) ||
        command != 'Q') {
      return 32;
    }
    const char responder_ready = 'R';
    if (!WriteExact(parent_events, &responder_ready, 1U)) {
      return 33;
    }

    CpuSnapshot before;
    bool measurement_started = false;
    for (;;) {
      if (!ReadExact(
              parent_commands, &command, 1U,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  kControlTimeout + kDrainTimeout))) {
        return 34;
      }
      if (command == 'B') {
        counters->requests_received.store(
            0U, std::memory_order_relaxed);
        counters->payload_mismatches.store(
            0U, std::memory_order_relaxed);
        counters->response_publish_failures.store(
            0U, std::memory_order_relaxed);
        before = SampleCpu();
        measurement_started = true;
        const char ready = 'B';
        if (!WriteExact(parent_events, &ready, 1U)) {
          return 35;
        }
        continue;
      }
      if (command != 'S' || !measurement_started) {
        return 36;
      }
      const auto after = SampleCpu();
      ChildReport report;
      report.requests_received =
          counters->requests_received.load(std::memory_order_relaxed);
      report.payload_mismatches =
          counters->payload_mismatches.load(std::memory_order_relaxed);
      report.response_publish_failures =
          counters->response_publish_failures.load(
              std::memory_order_relaxed);
      report.user_cpu_seconds =
          Delta(after.user_seconds, before.user_seconds);
      report.system_cpu_seconds =
          Delta(after.system_seconds, before.system_seconds);
      report.voluntary_context_switches = Delta(
          after.voluntary_context_switches,
          before.voluntary_context_switches);
      report.involuntary_context_switches = Delta(
          after.involuntary_context_switches,
          before.involuntary_context_switches);
      report.maximum_resident_bytes = after.maximum_resident_bytes;
      endpoint->Close();
      if (!WriteExact(parent_events, &report, sizeof(report))) {
        return 37;
      }
      return 0;
    }
  } catch (const std::exception&) {
    return 38;
  }
}

void StopChild(pid_t process) noexcept {
  if (process > 0) {
    static_cast<void>(::kill(process, SIGKILL));
    int status = 0;
    static_cast<void>(::waitpid(process, &status, 0));
  }
}

[[nodiscard]] std::optional<CaseResult> RunCase(
    const Options& options, std::size_t payload_bytes,
    std::size_t trial, std::uint64_t case_id) {
  int commands[2]{-1, -1};
  int events[2]{-1, -1};
  if (::pipe(commands) != 0 || ::pipe(events) != 0) {
    return std::nullopt;
  }
  const std::string token =
      "rclcpp_cmp_" + std::to_string(::getpid()) + "_" +
      std::to_string(case_id);
  const std::string request_topic = token + "_request";
  const std::string response_topic = token + "_response";
  const std::string child_command_fd = std::to_string(commands[0]);
  const std::string child_event_fd = std::to_string(events[1]);
  const pid_t child = ::fork();
  if (child < 0) {
    return std::nullopt;
  }
  if (child == 0) {
    static_cast<void>(::close(commands[1]));
    static_cast<void>(::close(events[0]));
    static_cast<void>(::execl(
        "/proc/self/exe", "rclcpp_comparative_benchmark",
        "--internal-responder", request_topic.c_str(),
        response_topic.c_str(), child_command_fd.c_str(),
        child_event_fd.c_str(), static_cast<char*>(nullptr)));
    std::_Exit(39);
  }

  static_cast<void>(::close(commands[0]));
  static_cast<void>(::close(events[1]));
  auto cleanup = [&] {
    static_cast<void>(::close(commands[1]));
    static_cast<void>(::close(events[0]));
    StopChild(child);
  };
  char event = 0;
  if (!ReadExact(
          events[0], &event, 1U,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout)) ||
      event != 'P') {
    cleanup();
    return std::nullopt;
  }

  auto state = std::make_shared<InitiatorState>();
  std::unique_ptr<RosEndpoint> endpoint;
  try {
    endpoint = RosEndpoint::Create(
        "rclcpp_comparative_initiator",
        request_topic, response_topic,
        [state](std::shared_ptr<RosEndpoint::Publisher>) {
          return [state](Message::ConstSharedPtr message) {
            state->OnResponse(message);
          };
        });
  } catch (const std::exception&) {
    cleanup();
    return std::nullopt;
  }

  const char ready = 'Q';
  if (!WriteExact(commands[1], &ready, 1U) ||
      !ReadExact(
          events[0], &event, 1U,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout)) ||
      event != 'R' ||
      !RunWarmup(
          *endpoint, *state, payload_bytes,
          options.warmup_messages)) {
    endpoint->Close();
    cleanup();
    return std::nullopt;
  }
  const char begin = 'B';
  if (!WriteExact(commands[1], &begin, 1U) ||
      !ReadExact(
          events[0], &event, 1U,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout)) ||
      event != 'B') {
    endpoint->Close();
    cleanup();
    return std::nullopt;
  }

  state->BeginMeasurement();
  const auto cpu_before = SampleCpu();
  const auto period = std::chrono::nanoseconds(
      1'000'000'000LL /
      static_cast<std::int64_t>(options.frequency_hz));
  const auto duration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          options.duration);
  const auto scheduled = std::max<std::uint64_t>(
      1U, static_cast<std::uint64_t>(
              duration.count() / period.count()));
  const auto measurement_start = Clock::now();
  std::uint64_t published = 0U;
  for (std::uint64_t ordinal = 0U; ordinal < scheduled; ++ordinal) {
    const auto release =
        measurement_start +
        period * static_cast<std::int64_t>(ordinal);
    std::this_thread::sleep_until(release);
    const auto sequence = ordinal + 1U;
    state->Expect(sequence, true);
    try {
      endpoint->SendGenerated(payload_bytes, sequence);
      ++published;
    } catch (const std::exception&) {
      state->Forget(sequence);
    }
  }
  const auto configured_end = measurement_start + duration;
  std::this_thread::sleep_until(configured_end);
  const auto send_end = Clock::now();
  const auto drain_start = send_end;
  static_cast<void>(state->WaitForDrain(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          kDrainTimeout)));
  const auto drain_end = Clock::now();
  const auto cpu_after = SampleCpu();

  const char stop = 'S';
  ChildReport child_report;
  if (!WriteExact(commands[1], &stop, 1U) ||
      !ReadExact(
          events[0], &child_report, sizeof(child_report),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout))) {
    endpoint->Close();
    cleanup();
    return std::nullopt;
  }
  endpoint->Close();
  static_cast<void>(::close(commands[1]));
  static_cast<void>(::close(events[0]));
  int child_status = 0;
  if (::waitpid(child, &child_status, 0) != child ||
      !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    return std::nullopt;
  }

  auto snapshot = state->Snapshot();
  std::sort(
      snapshot.latencies_us.begin(), snapshot.latencies_us.end());
  CaseResult result;
  result.run_id = options.run_id;
  result.case_id = case_id;
  result.trial = trial;
  result.payload_bytes = payload_bytes;
  result.frequency_hz = options.frequency_hz;
  result.configured_duration_ms =
      static_cast<std::uint64_t>(options.duration.count());
  result.scheduled_requests = scheduled;
  result.published_requests = published;
  result.request_publish_failures = scheduled - published;
  result.completed_round_trips = snapshot.completed_round_trips;
  result.lost_responses =
      published >= snapshot.completed_round_trips
          ? published - snapshot.completed_round_trips
          : 0U;
  result.responder_requests = child_report.requests_received;
  result.responder_payload_mismatches =
      child_report.payload_mismatches;
  result.responder_publish_failures =
      child_report.response_publish_failures;
  result.response_payload_mismatches =
      snapshot.response_payload_mismatches;
  result.unexpected_responses = snapshot.unexpected_responses;
  result.logical_payload_bytes =
      snapshot.completed_round_trips *
      static_cast<std::uint64_t>(payload_bytes) * 2U;
  result.send_window_ms =
      std::chrono::duration<double, std::milli>(
          send_end - measurement_start)
          .count();
  result.drain_ms =
      std::chrono::duration<double, std::milli>(
          drain_end - drain_start)
          .count();
  const double configured_seconds =
      std::chrono::duration<double>(duration).count();
  result.completed_round_trips_per_second =
      static_cast<double>(snapshot.completed_round_trips) /
      configured_seconds;
  result.logical_payload_mib_per_second =
      static_cast<double>(result.logical_payload_bytes) /
      static_cast<double>(kMebibyte) / configured_seconds;
  result.p50_us = Percentile(snapshot.latencies_us, 0.50);
  result.p95_us = Percentile(snapshot.latencies_us, 0.95);
  result.p99_us = Percentile(snapshot.latencies_us, 0.99);
  result.p99_9_us = Percentile(snapshot.latencies_us, 0.999);
  result.max_us = snapshot.latencies_us.empty()
                      ? 0.0
                      : snapshot.latencies_us.back();
  const double user_cpu =
      Delta(cpu_after.user_seconds, cpu_before.user_seconds) +
      child_report.user_cpu_seconds;
  const double system_cpu =
      Delta(cpu_after.system_seconds, cpu_before.system_seconds) +
      child_report.system_cpu_seconds;
  result.user_cpu_ms = user_cpu * 1000.0;
  result.system_cpu_ms = system_cpu * 1000.0;
  const double wall_seconds =
      std::chrono::duration<double>(
          drain_end - measurement_start)
          .count();
  result.cpu_utilization_percent =
      (user_cpu + system_cpu) / wall_seconds * 100.0;
  result.voluntary_context_switches =
      Delta(
          cpu_after.voluntary_context_switches,
          cpu_before.voluntary_context_switches) +
      child_report.voluntary_context_switches;
  result.involuntary_context_switches =
      Delta(
          cpu_after.involuntary_context_switches,
          cpu_before.involuntary_context_switches) +
      child_report.involuntary_context_switches;
  result.initiator_peak_rss_kib =
      cpu_after.maximum_resident_bytes / 1024U;
  result.responder_peak_rss_kib =
      child_report.maximum_resident_bytes / 1024U;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 6 &&
        std::string_view(argv[1]) == "--internal-responder") {
      const auto command_fd =
          ParseUnsigned(argv[4], "internal command fd");
      const auto event_fd =
          ParseUnsigned(argv[5], "internal event fd");
      if (command_fd >
              static_cast<std::uint64_t>(
                  std::numeric_limits<int>::max()) ||
          event_fd >
              static_cast<std::uint64_t>(
                  std::numeric_limits<int>::max())) {
        return 39;
      }
      return ChildMain(
          argv[2], argv[3], static_cast<int>(command_fd),
          static_cast<int>(event_fd));
    }
    auto options = ParseOptions(argc, argv);
    if (options.run_id.empty()) {
      options.run_id = DefaultRunId();
    }
    std::ofstream output;
    if (!options.output_path.empty()) {
      output.open(options.output_path, std::ios::out | std::ios::trunc);
      if (!output) {
        throw std::runtime_error("failed to open benchmark output file");
      }
    }
    auto emit = [&](const std::string& line) {
      std::cout << line << '\n';
      std::cout.flush();
      if (output) {
        output << line << '\n';
        output.flush();
      }
    };
    emit(EnvironmentJson(options));
    std::uint64_t case_id = 1U;
    bool all_ok = true;
    std::size_t results = 0U;
    for (const auto payload : options.payload_sizes) {
      for (std::size_t trial = 1U;
           trial <= options.trials; ++trial) {
        auto result = RunCase(
            options, payload, trial, case_id++);
        if (!result) {
          std::cerr << "rclcpp benchmark case setup failed\n";
          return 1;
        }
        all_ok = all_ok && result->ok();
        emit(ResultJson(*result));
        ++results;
      }
    }
    const auto expected =
        options.payload_sizes.size() * options.trials;
    return all_ok && results == expected ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "rclcpp comparative benchmark error: "
              << error.what() << '\n';
    return 2;
  }
}
