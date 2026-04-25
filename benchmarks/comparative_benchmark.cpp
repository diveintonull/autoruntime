#include <autoruntime/fastipc_transport.hpp>
#include <autoruntime/node.hpp>
#include <autoruntime/observability.hpp>

#if defined(AUTORUNTIME_HAS_DDS)
#include <autoruntime/dds_transport.hpp>
#endif

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
#include <functional>
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

#include <poll.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AUTORUNTIME_SOURCE_REVISION
#define AUTORUNTIME_SOURCE_REVISION "unknown"
#endif

#ifndef AUTORUNTIME_COMPILER
#define AUTORUNTIME_COMPILER "unknown"
#endif

#ifndef AUTORUNTIME_BUILD_TYPE
#define AUTORUNTIME_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kSchemaVersion = 1U;
constexpr std::size_t kMaximumPayloadBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMebibyte = 1024U * 1024U;
constexpr std::uint64_t kWarmupSequenceMask = 1ULL << 63U;
constexpr std::uint32_t kQueueDepth = 64U;
constexpr auto kControlTimeout = 10s;
constexpr auto kDrainTimeout = 5s;

enum class Mode : std::uint8_t {
  FastIpcCopy,
  FastIpcLoan,
  Dds,
};

struct Options {
  std::vector<Mode> modes{Mode::FastIpcCopy, Mode::FastIpcLoan
#if defined(AUTORUNTIME_HAS_DDS)
                          , Mode::Dds
#endif
  };
  std::vector<std::size_t> payload_sizes{
      64U, 1024U, 64U * 1024U, 1024U * 1024U};
  std::uint32_t frequency_hz{100U};
  std::chrono::milliseconds duration{3000};
  std::size_t warmup_messages{20U};
  std::size_t trials{3U};
  std::uint32_t domain_id{0U};
  std::string run_id;
  std::string output_path;
  bool self_test{false};
};

struct HostInfo {
  std::string hostname{"unknown"};
  std::string kernel_release{"unknown"};
  std::string machine{"unknown"};
  std::string cpu_model{"unknown"};
  std::uint64_t memory_total_kib{0U};
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

struct InitiatorSnapshot {
  std::uint64_t completed_round_trips{0U};
  std::uint64_t response_payload_mismatches{0U};
  std::uint64_t unexpected_responses{0U};
  std::size_t pending_requests{0U};
  std::vector<double> latencies_us;
};

struct CaseResult {
  std::string run_id;
  std::uint64_t case_id{0U};
  std::size_t trial{0U};
  Mode mode{Mode::FastIpcCopy};
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

[[nodiscard]] const char* ModeName(Mode mode) noexcept {
  switch (mode) {
    case Mode::FastIpcCopy:
      return "autoruntime_fastipc_copy";
    case Mode::FastIpcLoan:
      return "autoruntime_fastipc_loan";
    case Mode::Dds:
      return "autoruntime_dds";
  }
  return "unknown";
}

[[nodiscard]] const char* ComparisonGroup() noexcept {
  return "same_host_two_process_request_echo";
}

[[nodiscard]] bool UsesLoan(Mode mode) noexcept {
  return mode == Mode::FastIpcLoan;
}

[[nodiscard]] std::optional<Mode> ParseMode(
    std::string_view text) noexcept {
  if (text == "fastipc-copy") {
    return Mode::FastIpcCopy;
  }
  if (text == "fastipc-loan") {
    return Mode::FastIpcLoan;
  }
  if (text == "dds") {
#if defined(AUTORUNTIME_HAS_DDS)
    return Mode::Dds;
#else
    return std::nullopt;
#endif
  }
  return std::nullopt;
}

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
  bool explicit_mode = false;
  bool explicit_payload = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      std::cout
          << "usage: autoruntime_comparative_benchmark "
             "[--mode fastipc-copy|fastipc-loan|dds] "
             "[--payload BYTES] [--frequency HZ] "
             "[--duration-ms MS] [--warmup N] [--trials N] "
             "[--domain ID] [--run-id ID] [--output FILE] "
             "[--self-test]\n";
      std::exit(0);
    }
    if (argument == "--self-test") {
      options.self_test = true;
      continue;
    }
    if (argument == "--mode" || argument.starts_with("--mode=")) {
      const auto text =
          OptionValue(index, argc, argv, argument, "--mode");
      const auto mode = ParseMode(text);
      if (!mode) {
        throw std::invalid_argument(
            "unsupported or unavailable --mode: " + std::string(text));
      }
      if (!explicit_mode) {
        options.modes.clear();
        explicit_mode = true;
      }
      options.modes.push_back(*mode);
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
        throw std::invalid_argument(
            "--warmup must not exceed 10000");
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
    if (argument == "--domain" ||
        argument.starts_with("--domain=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--domain"),
          "--domain");
      if (value > 232U) {
        throw std::invalid_argument(
            "--domain must be between 0 and 232");
      }
      options.domain_id = static_cast<std::uint32_t>(value);
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

[[nodiscard]] std::string ReadValue(
    const std::string& path, std::string_view prefix) {
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with(prefix)) {
      auto value = line.substr(prefix.size());
      const auto first = value.find_first_not_of(" \t:");
      return first == std::string::npos ? std::string{} :
                                         value.substr(first);
    }
  }
  return {};
}

[[nodiscard]] HostInfo CaptureHostInfo() {
  HostInfo result;
  std::vector<char> hostname(256U, '\0');
  if (::gethostname(hostname.data(), hostname.size()) == 0) {
    result.hostname = hostname.data();
  }
  utsname system{};
  if (::uname(&system) == 0) {
    result.kernel_release = system.release;
    result.machine = system.machine;
  }
  const auto cpu = ReadValue("/proc/cpuinfo", "model name");
  if (!cpu.empty()) {
    result.cpu_model = cpu;
  }
  const auto memory = ReadValue("/proc/meminfo", "MemTotal");
  std::uint64_t parsed = 0U;
  const auto conversion =
      std::from_chars(memory.data(),
                      memory.data() + memory.size(), parsed);
  if (conversion.ec == std::errc{}) {
    result.memory_total_kib = parsed;
  }
  return result;
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

[[nodiscard]] std::string EnvironmentJson(
    const Options& options, const HostInfo& host) {
  std::ostringstream stream;
  stream << "{\"schema_version\":" << kSchemaVersion
         << ",\"record_type\":\"environment\""
         << ",\"benchmark\":\"autoruntime_comparative\""
         << ",\"run_id\":\"" << JsonEscape(options.run_id) << "\""
         << ",\"timestamp_utc\":\"" << UtcNow() << "\""
         << ",\"hostname\":\"" << JsonEscape(host.hostname) << "\""
         << ",\"kernel_release\":\""
         << JsonEscape(host.kernel_release) << "\""
         << ",\"machine\":\"" << JsonEscape(host.machine) << "\""
         << ",\"cpu_model\":\"" << JsonEscape(host.cpu_model) << "\""
         << ",\"logical_cpus\":" << std::thread::hardware_concurrency()
         << ",\"memory_total_kib\":" << host.memory_total_kib
         << ",\"compiler\":\"" << AUTORUNTIME_COMPILER << "\""
         << ",\"build_type\":\"" << AUTORUNTIME_BUILD_TYPE << "\""
         << ",\"source_revision\":\""
         << AUTORUNTIME_SOURCE_REVISION << "\""
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
         << ",\"implementation\":\"autoruntime\""
         << ",\"mode\":\"" << ModeName(result.mode) << "\""
         << ",\"comparison_group\":\""
         << ComparisonGroup() << "\""
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

[[nodiscard]] std::byte PatternByte(
    std::uint64_t sequence, std::size_t index) noexcept {
  return static_cast<std::byte>(
      (sequence + static_cast<std::uint64_t>(index) * 131U + 17U) %
      251U);
}

void PreparePayload(
    std::span<std::byte> payload, std::uint64_t sequence) noexcept {
  std::memcpy(payload.data(), &sequence, sizeof(sequence));
  for (std::size_t index = sizeof(sequence);
       index < payload.size(); ++index) {
    payload[index] = PatternByte(sequence, index);
  }
}

[[nodiscard]] std::optional<std::uint64_t> ValidatePayload(
    std::span<const std::byte> payload) noexcept {
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

template <typename T>
T Take(autoruntime::Result<T> result, std::string_view operation) {
  if (!result) {
    throw std::runtime_error(
        std::string(operation) + ": " + result.status().detail());
  }
  return std::move(result).take_value();
}


[[nodiscard]] autoruntime::QosProfile BenchmarkQos() {
  autoruntime::QosProfile qos;
  qos.reliability = autoruntime::Reliability::Reliable;
  qos.history = autoruntime::HistoryKind::KeepLast;
  qos.depth = kQueueDepth;
  qos.deadline = 500ms;
  return qos;
}

class Sender {
 public:
  Sender(autoruntime::Publisher publisher, bool loaned)
      : publisher_(std::move(publisher)), loaned_(loaned) {}

  autoruntime::Status SendGenerated(
      std::size_t payload_bytes, std::uint64_t sequence) {
    if (loaned_) {
      auto result = publisher_.Loan(payload_bytes);
      if (!result) {
        return result.status();
      }
      auto loan = std::move(result).take_value();
      PreparePayload(loan.Data(), sequence);
      return loan.Publish();
    }
    scratch_.resize(payload_bytes);
    PreparePayload(scratch_, sequence);
    return publisher_.Publish(scratch_);
  }

  autoruntime::Status Echo(
      std::span<const std::byte> payload,
      autoruntime::TraceContext trace) {
    if (loaned_) {
      auto result = publisher_.Loan(payload.size(), trace);
      if (!result) {
        return result.status();
      }
      auto loan = std::move(result).take_value();
      std::copy(payload.begin(), payload.end(), loan.Data().begin());
      return loan.Publish();
    }
    return publisher_.Publish(payload, trace);
  }

 private:
  autoruntime::Publisher publisher_;
  bool loaned_{false};
  std::vector<std::byte> scratch_;
};

using PayloadHandler = std::function<void(
    const autoruntime::MessageEnvelope&,
    std::span<const std::byte>)>;
using HandlerFactory =
    std::function<PayloadHandler(std::shared_ptr<Sender>)>;

struct ProcessEndpoint {
  std::shared_ptr<autoruntime::Transport> outgoing;
  std::shared_ptr<autoruntime::Transport> incoming;
  std::shared_ptr<autoruntime::Executor> executor;
  std::shared_ptr<Sender> sender;
  std::optional<autoruntime::Subscriber> subscriber;
  std::optional<autoruntime::LoanedSubscriber> loaned_subscriber;

  void Close() noexcept {
    if (loaned_subscriber) {
      static_cast<void>(loaned_subscriber->Close());
    }
    if (subscriber) {
      static_cast<void>(subscriber->Close());
    }
    if (executor) {
      static_cast<void>(
          executor->Stop(autoruntime::Deadline::After(1s)));
    }
    if (incoming) {
      static_cast<void>(incoming->Close());
    }
    if (outgoing && outgoing.get() != incoming.get()) {
      static_cast<void>(outgoing->Close());
    }
  }

  ~ProcessEndpoint() { Close(); }
};

[[nodiscard]] autoruntime::Result<std::unique_ptr<ProcessEndpoint>>
CreateEndpoint(
    Mode mode,
    std::shared_ptr<autoruntime::Transport> outgoing,
    std::shared_ptr<autoruntime::Transport> incoming,
    std::string outgoing_topic,
    std::string incoming_topic,
    HandlerFactory handler_factory,
    std::string node_prefix) {
  auto endpoint = std::make_unique<ProcessEndpoint>();
  endpoint->outgoing = std::move(outgoing);
  endpoint->incoming = std::move(incoming);
  endpoint->executor = std::make_shared<autoruntime::Executor>();
  auto group = endpoint->executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{
          node_prefix + "-callbacks", 1U, 256U});
  if (!group) {
    return group.status();
  }

  autoruntime::Node sender_node(
      {node_prefix + "-sender", 1U},
      endpoint->executor, endpoint->outgoing);
  autoruntime::Node receiver_node(
      {node_prefix + "-receiver", 1U},
      endpoint->executor, endpoint->incoming);
  auto publisher =
      sender_node.CreatePublisher(outgoing_topic, BenchmarkQos());
  if (!publisher) {
    return publisher.status();
  }
  endpoint->sender = std::make_shared<Sender>(
      publisher.value(), UsesLoan(mode));
  auto handler = handler_factory(endpoint->sender);
  autoruntime::SubscriptionOptions subscription_options;
  subscription_options.callback_group = group.value();
  subscription_options.queue_capacity = 128U;
  subscription_options.overflow_policy =
      autoruntime::OverflowPolicy::DropNewest;
  subscription_options.qos = BenchmarkQos();

  if (UsesLoan(mode)) {
    auto subscriber = receiver_node.CreateLoanedSubscriber(
        incoming_topic, subscription_options,
        [handler = std::move(handler)](
            const autoruntime::LoanedMessage& message) {
          handler(message.envelope(), message.Data());
        });
    if (!subscriber) {
      return subscriber.status();
    }
    endpoint->loaned_subscriber.emplace(subscriber.value());
  } else {
    auto subscriber = receiver_node.CreateSubscriber(
        incoming_topic, subscription_options,
        [handler = std::move(handler)](
            const autoruntime::Message& message) {
          handler(message.envelope, message.payload);
        });
    if (!subscriber) {
      return subscriber.status();
    }
    endpoint->subscriber.emplace(subscriber.value());
  }
  const auto start = endpoint->executor->Start();
  if (!start) {
    return start;
  }
  return endpoint;
}

[[nodiscard]] autoruntime::Result<
    std::shared_ptr<autoruntime::Transport>>
CreateFastIpcTransport(
    std::string topic, std::string channel,
    autoruntime::FastIpcDirection direction,
    std::size_t maximum_payload,
    bool unlink_on_close) {
  autoruntime::FastIpcTransportConfig config;
  config.open_timeout = 3s;
  config.endpoints.push_back(autoruntime::FastIpcEndpointConfig{
      std::move(topic), std::move(channel), direction, kQueueDepth,
      static_cast<std::uint32_t>(maximum_payload), 2s,
      unlink_on_close});
  auto result = autoruntime::FastIpcTransport::Create(std::move(config));
  if (!result) {
    return result.status();
  }
  return std::static_pointer_cast<autoruntime::Transport>(
      result.value());
}

#if defined(AUTORUNTIME_HAS_DDS)
[[nodiscard]] autoruntime::Result<
    std::shared_ptr<autoruntime::Transport>>
CreateDdsTransport(
    std::uint32_t domain_id, std::string participant_name) {
  autoruntime::DdsTransportConfig config;
  config.domain_id = domain_id;
  config.participant_name = std::move(participant_name);
  config.receive_poll_interval = 1ms;
  config.reliability_max_blocking_time = 500ms;
  config.max_payload_size = kMaximumPayloadBytes;
  auto result = autoruntime::DdsTransport::Create(std::move(config));
  if (!result) {
    return result.status();
  }
  return std::static_pointer_cast<autoruntime::Transport>(
      result.value());
}
#endif

class InitiatorState {
 public:
  void Expect(std::uint64_t sequence, bool measured) {
    std::lock_guard lock(mutex_);
    pending_.emplace(
        sequence, Pending{Clock::now(), measured});
  }

  void Forget(std::uint64_t sequence) {
    std::lock_guard lock(mutex_);
    pending_.erase(sequence);
    warmup_outcomes_.erase(sequence);
    condition_.notify_all();
  }

  void SendFailed(std::uint64_t sequence) {
    Forget(sequence);
  }

  void OnResponse(
      const autoruntime::MessageEnvelope&,
      std::span<const std::byte> payload) {
    const auto sequence = ValidatePayload(payload);
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
      const auto latency =
          std::chrono::duration<double, std::micro>(
              Clock::now() - pending.sent_at)
              .count();
      latencies_us_.push_back(latency);
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

  [[nodiscard]] InitiatorSnapshot Snapshot() const {
    std::lock_guard lock(mutex_);
    InitiatorSnapshot result;
    result.completed_round_trips = completed_round_trips_;
    result.response_payload_mismatches =
        response_payload_mismatches_;
    result.unexpected_responses = unexpected_responses_;
    result.latencies_us = latencies_us_;
    result.pending_requests =
        static_cast<std::size_t>(std::count_if(
            pending_.begin(), pending_.end(),
            [](const auto& entry) {
              return entry.second.measured;
            }));
    return result;
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

[[nodiscard]] bool RunWarmup(
    ProcessEndpoint& endpoint, InitiatorState& state,
    std::size_t payload_bytes, std::size_t messages) {
  if (messages == 0U) {
    return true;
  }
  const auto deadline = Clock::now() + 8s;
  std::size_t completed = 0U;
  std::uint64_t attempt = 1U;
  while (completed < messages && Clock::now() < deadline) {
    const auto sequence = kWarmupSequenceMask | attempt++;
    state.Expect(sequence, false);
    const auto status =
        endpoint.sender->SendGenerated(payload_bytes, sequence);
    if (!status) {
      state.Forget(sequence);
      std::this_thread::sleep_for(20ms);
      continue;
    }
    const auto outcome = state.WaitWarmup(sequence, 500ms);
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

[[nodiscard]] double Delta(
    double after, double before) noexcept {
  return std::max(0.0, after - before);
}

[[nodiscard]] std::uint64_t Delta(
    std::uint64_t after, std::uint64_t before) noexcept {
  return after >= before ? after - before : 0U;
}

[[nodiscard]] int ChildMain(
    Mode mode, std::size_t payload_bytes,
    std::uint32_t domain_id,
    const std::string& request_topic,
    const std::string& response_topic,
    const std::string& request_channel,
    const std::string& response_channel,
    int parent_commands,
    int parent_events) {
#if !defined(AUTORUNTIME_HAS_DDS)
  static_cast<void>(domain_id);
#endif
  try {
    std::shared_ptr<autoruntime::Transport> outgoing;
    std::shared_ptr<autoruntime::Transport> incoming;
    if (mode == Mode::Dds) {
#if defined(AUTORUNTIME_HAS_DDS)
      outgoing = Take(
          CreateDdsTransport(domain_id, "comparative-responder"),
          "create responder DDS transport");
      incoming = outgoing;
#else
      return 31;
#endif
    } else {
      outgoing = Take(
          CreateFastIpcTransport(
              response_topic, response_channel,
              autoruntime::FastIpcDirection::Publish,
              payload_bytes, true),
          "create responder FastIPC publisher");
    }

    const char producer_ready = 'P';
    if (!WriteExact(parent_events, &producer_ready, 1U)) {
      return 32;
    }
    char command = 0;
    if (!ReadExact(
            parent_commands, &command, 1U,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                kControlTimeout)) ||
        command != 'Q') {
      return 33;
    }

    if (mode != Mode::Dds) {
      incoming = Take(
          CreateFastIpcTransport(
              request_topic, request_channel,
              autoruntime::FastIpcDirection::Subscribe,
              payload_bytes, false),
          "create responder FastIPC subscriber");
    }

    auto counters = std::make_shared<ChildCounters>();
    auto endpoint = Take(
        CreateEndpoint(
            mode, outgoing, incoming, response_topic, request_topic,
            [counters](std::shared_ptr<Sender> sender) {
              return [counters, sender = std::move(sender)](
                         const autoruntime::MessageEnvelope& envelope,
                         std::span<const std::byte> payload) {
                std::uint64_t raw_sequence = 0U;
                if (payload.size() >= sizeof(raw_sequence)) {
                  std::memcpy(
                      &raw_sequence, payload.data(),
                      sizeof(raw_sequence));
                }
                const bool measured =
                    (raw_sequence & kWarmupSequenceMask) == 0U;
                if (!ValidatePayload(payload)) {
                  if (measured) {
                    counters->payload_mismatches.fetch_add(
                        1U, std::memory_order_relaxed);
                  }
                  return;
                }
                if (measured) {
                  counters->requests_received.fetch_add(
                      1U, std::memory_order_relaxed);
                }
                const autoruntime::TraceContext trace{
                    envelope.trace_id,
                    envelope.span_id,
                    envelope.source_timestamp_ns};
                if (!sender->Echo(payload, trace) && measured) {
                  counters->response_publish_failures.fetch_add(
                      1U, std::memory_order_relaxed);
                }
              };
            },
            "comparative-responder"),
        "create responder runtime endpoint");

    const char responder_ready = 'R';
    if (!WriteExact(parent_events, &responder_ready, 1U)) {
      return 34;
    }

    autoruntime::ProcessCpuMetrics before;
    bool measurement_started = false;
    for (;;) {
      if (!ReadExact(
              parent_commands, &command, 1U,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  kControlTimeout + kDrainTimeout))) {
        return 35;
      }
      if (command == 'B') {
        counters->requests_received.store(
            0U, std::memory_order_relaxed);
        counters->payload_mismatches.store(
            0U, std::memory_order_relaxed);
        counters->response_publish_failures.store(
            0U, std::memory_order_relaxed);
        before = autoruntime::SampleProcessCpu();
        measurement_started = true;
        const char baseline_ready = 'B';
        if (!WriteExact(parent_events, &baseline_ready, 1U)) {
          return 36;
        }
        continue;
      }
      if (command != 'S' || !measurement_started) {
        return 37;
      }

      const auto after = autoruntime::SampleProcessCpu();
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
        return 38;
      }
      return 0;
    }
  } catch (const std::exception&) {
    return 39;
  }
}

void StopChild(pid_t process) noexcept {
  if (process > 0) {
    static_cast<void>(::kill(process, SIGKILL));
    int status = 0;
    static_cast<void>(::waitpid(process, &status, 0));
  }
}

[[nodiscard]] autoruntime::Result<CaseResult> RunCase(
    const Options& options, Mode mode,
    std::size_t payload_bytes, std::size_t trial,
    std::uint64_t case_id) {
  int commands[2]{-1, -1};
  int events[2]{-1, -1};
  if (::pipe(commands) != 0 || ::pipe(events) != 0) {
    return autoruntime::Status(
        autoruntime::StatusCode::Internal,
        "failed to create benchmark control pipes");
  }

  const std::string token =
      "autoruntime_cmp_" + std::to_string(::getpid()) + "_" +
      std::to_string(case_id);
  const std::string request_topic = token + "_request";
  const std::string response_topic = token + "_response";
  const std::string request_channel = token + "_request_shm";
  const std::string response_channel = token + "_response_shm";
  const pid_t child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(commands[0]));
    static_cast<void>(::close(commands[1]));
    static_cast<void>(::close(events[0]));
    static_cast<void>(::close(events[1]));
    return autoruntime::Status(
        autoruntime::StatusCode::Internal,
        "failed to fork benchmark responder");
  }
  if (child == 0) {
    static_cast<void>(::close(commands[1]));
    static_cast<void>(::close(events[0]));
    const int code = ChildMain(
        mode, payload_bytes, options.domain_id,
        request_topic, response_topic,
        request_channel, response_channel,
        commands[0], events[1]);
    static_cast<void>(::close(commands[0]));
    static_cast<void>(::close(events[1]));
    std::_Exit(code);
  }

  static_cast<void>(::close(commands[0]));
  static_cast<void>(::close(events[1]));
  auto close_parent_descriptors = [&] {
    static_cast<void>(::close(commands[1]));
    static_cast<void>(::close(events[0]));
  };
  auto fail = [&](std::string detail)
      -> autoruntime::Result<CaseResult> {
    close_parent_descriptors();
    StopChild(child);
    return autoruntime::Status(
        autoruntime::StatusCode::Internal, std::move(detail));
  };

  char event = 0;
  if (!ReadExact(
          events[0], &event, 1U,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout)) ||
      event != 'P') {
    return fail("responder did not create its outbound transport");
  }

  std::shared_ptr<autoruntime::Transport> outgoing;
  std::shared_ptr<autoruntime::Transport> incoming;
  try {
    if (mode == Mode::Dds) {
#if defined(AUTORUNTIME_HAS_DDS)
      outgoing = Take(
          CreateDdsTransport(options.domain_id, "comparative-initiator"),
          "create initiator DDS transport");
      incoming = outgoing;
#else
      return fail("DDS mode is unavailable in this build");
#endif
    } else {
      outgoing = Take(
          CreateFastIpcTransport(
              request_topic, request_channel,
              autoruntime::FastIpcDirection::Publish,
              payload_bytes, true),
          "create initiator FastIPC publisher");
      incoming = Take(
          CreateFastIpcTransport(
              response_topic, response_channel,
              autoruntime::FastIpcDirection::Subscribe,
              payload_bytes, false),
          "create initiator FastIPC subscriber");
    }
  } catch (const std::exception& error) {
    return fail(error.what());
  }

  auto state = std::make_shared<InitiatorState>();
  std::unique_ptr<ProcessEndpoint> endpoint;
  try {
    endpoint = Take(
        CreateEndpoint(
            mode, outgoing, incoming, request_topic, response_topic,
            [state](std::shared_ptr<Sender>) {
              return [state](
                         const autoruntime::MessageEnvelope& envelope,
                         std::span<const std::byte> payload) {
                state->OnResponse(envelope, payload);
              };
            },
            "comparative-initiator"),
        "create initiator runtime endpoint");
  } catch (const std::exception& error) {
    return fail(error.what());
  }

  const char initiator_ready = 'Q';
  if (!WriteExact(commands[1], &initiator_ready, 1U) ||
      !ReadExact(
          events[0], &event, 1U,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout)) ||
      event != 'R') {
    endpoint->Close();
    return fail("responder did not start its runtime endpoint");
  }

  if (!RunWarmup(
          *endpoint, *state, payload_bytes,
          options.warmup_messages)) {
    endpoint->Close();
    return fail("benchmark discovery/warmup did not converge");
  }

  const char begin = 'B';
  if (!WriteExact(commands[1], &begin, 1U) ||
      !ReadExact(
          events[0], &event, 1U,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout)) ||
      event != 'B') {
    endpoint->Close();
    return fail("responder did not acknowledge measurement baseline");
  }

  state->BeginMeasurement();
  const auto parent_cpu_before = autoruntime::SampleProcessCpu();
  const auto period = std::chrono::nanoseconds(
      1'000'000'000LL /
      static_cast<std::int64_t>(options.frequency_hz));
  const auto configured_duration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          options.duration);
  const auto scheduled_requests = std::max<std::uint64_t>(
      1U, static_cast<std::uint64_t>(
              configured_duration.count() / period.count()));
  const auto measurement_start = Clock::now();
  std::uint64_t published_requests = 0U;
  std::uint64_t publish_failures = 0U;
  for (std::uint64_t ordinal = 0U;
       ordinal < scheduled_requests; ++ordinal) {
    const auto release =
        measurement_start + period *
            static_cast<std::int64_t>(ordinal);
    std::this_thread::sleep_until(release);
    const std::uint64_t sequence = ordinal + 1U;
    state->Expect(sequence, true);
    const auto status =
        endpoint->sender->SendGenerated(payload_bytes, sequence);
    if (status) {
      ++published_requests;
    } else {
      ++publish_failures;
      state->SendFailed(sequence);
    }
  }
  const auto configured_end = measurement_start + configured_duration;
  std::this_thread::sleep_until(configured_end);
  const auto send_end = Clock::now();
  const auto drain_start = send_end;
  static_cast<void>(state->WaitForDrain(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          kDrainTimeout)));
  const auto drain_end = Clock::now();
  const auto parent_cpu_after = autoruntime::SampleProcessCpu();

  const char stop = 'S';
  if (!WriteExact(commands[1], &stop, 1U)) {
    endpoint->Close();
    return fail("failed to stop benchmark responder");
  }
  ChildReport child_report;
  if (!ReadExact(
          events[0], &child_report, sizeof(child_report),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kControlTimeout))) {
    endpoint->Close();
    return fail("responder did not return its result counters");
  }
  endpoint->Close();
  close_parent_descriptors();

  int child_status = 0;
  if (::waitpid(child, &child_status, 0) != child ||
      !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    return autoruntime::Status(
        autoruntime::StatusCode::Internal,
        "benchmark responder exited unsuccessfully");
  }

  auto snapshot = state->Snapshot();
  std::sort(
      snapshot.latencies_us.begin(), snapshot.latencies_us.end());
  CaseResult result;
  result.run_id = options.run_id;
  result.case_id = case_id;
  result.trial = trial;
  result.mode = mode;
  result.payload_bytes = payload_bytes;
  result.frequency_hz = options.frequency_hz;
  result.configured_duration_ms =
      static_cast<std::uint64_t>(options.duration.count());
  result.scheduled_requests = scheduled_requests;
  result.published_requests = published_requests;
  result.request_publish_failures = publish_failures;
  result.completed_round_trips = snapshot.completed_round_trips;
  result.lost_responses =
      published_requests >= snapshot.completed_round_trips
          ? published_requests - snapshot.completed_round_trips
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
      std::chrono::duration<double>(configured_duration).count();
  result.completed_round_trips_per_second =
      configured_seconds > 0.0
          ? static_cast<double>(snapshot.completed_round_trips) /
                configured_seconds
          : 0.0;
  result.logical_payload_mib_per_second =
      configured_seconds > 0.0
          ? static_cast<double>(result.logical_payload_bytes) /
                static_cast<double>(kMebibyte) /
                configured_seconds
          : 0.0;
  result.p50_us = Percentile(snapshot.latencies_us, 0.50);
  result.p95_us = Percentile(snapshot.latencies_us, 0.95);
  result.p99_us = Percentile(snapshot.latencies_us, 0.99);
  result.p99_9_us = Percentile(snapshot.latencies_us, 0.999);
  result.max_us = snapshot.latencies_us.empty()
                      ? 0.0
                      : snapshot.latencies_us.back();

  const double parent_user = Delta(
      parent_cpu_after.user_seconds, parent_cpu_before.user_seconds);
  const double parent_system = Delta(
      parent_cpu_after.system_seconds,
      parent_cpu_before.system_seconds);
  const double user_cpu = parent_user + child_report.user_cpu_seconds;
  const double system_cpu =
      parent_system + child_report.system_cpu_seconds;
  result.user_cpu_ms = user_cpu * 1000.0;
  result.system_cpu_ms = system_cpu * 1000.0;
  const double measured_wall_seconds =
      std::chrono::duration<double>(drain_end - measurement_start).count();
  result.cpu_utilization_percent =
      measured_wall_seconds > 0.0
          ? (user_cpu + system_cpu) / measured_wall_seconds * 100.0
          : 0.0;
  result.voluntary_context_switches =
      Delta(
          parent_cpu_after.voluntary_context_switches,
          parent_cpu_before.voluntary_context_switches) +
      child_report.voluntary_context_switches;
  result.involuntary_context_switches =
      Delta(
          parent_cpu_after.involuntary_context_switches,
          parent_cpu_before.involuntary_context_switches) +
      child_report.involuntary_context_switches;
  result.initiator_peak_rss_kib =
      parent_cpu_after.maximum_resident_bytes / 1024U;
  result.responder_peak_rss_kib =
      child_report.maximum_resident_bytes / 1024U;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    auto options = ParseOptions(argc, argv);
    if (options.run_id.empty()) {
      options.run_id = DefaultRunId();
    }
    if (options.domain_id == 0U) {
      options.domain_id =
          20U +
          (static_cast<std::uint32_t>(::getpid()) % 180U);
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
      if (output) {
        output << line << '\n';
        output.flush();
      }
    };

    emit(EnvironmentJson(options, CaptureHostInfo()));
    std::uint64_t case_id = 1U;
    std::size_t result_count = 0U;
    bool all_ok = true;
    for (const auto mode : options.modes) {
      for (const auto payload : options.payload_sizes) {
        for (std::size_t trial = 1U;
             trial <= options.trials; ++trial) {
          auto result = RunCase(
              options, mode, payload, trial, case_id++);
          if (!result) {
            std::cerr << "comparative benchmark case failed: "
                      << result.status().detail() << '\n';
            return 1;
          }
          all_ok = all_ok && result.value().ok();
          emit(ResultJson(result.value()));
          ++result_count;
        }
      }
    }
    const auto expected =
        options.modes.size() * options.payload_sizes.size() *
        options.trials;
    if (result_count != expected || !all_ok) {
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "comparative benchmark error: "
              << error.what() << '\n';
    return 2;
  }
}
