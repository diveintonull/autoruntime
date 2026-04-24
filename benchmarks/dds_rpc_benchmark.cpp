#include <autoruntime/dds_transport.hpp>
#include <autoruntime/node.hpp>
#include <autoruntime/observability.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
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
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/utsname.h>
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
constexpr std::size_t kMaximumPayloadBytes = 1024U * 1024U;
constexpr std::size_t kMaximumIterations = 1'000'000U;
constexpr std::size_t kMaximumTrials = 100U;
constexpr std::size_t kMebibyte = 1024U * 1024U;
constexpr std::string_view kServiceName = "AutoRuntimeDdsRpcBenchmark";

struct Options {
  std::vector<std::size_t> payload_sizes{
      64U, 1024U, 64U * 1024U};
  std::size_t iterations{1000U};
  std::size_t warmup_iterations{100U};
  std::size_t trials{3U};
  std::optional<std::uint32_t> domain_id;
  std::string output_path;
  bool self_test{false};
};

struct HostInfo {
  std::string hostname{"unknown"};
  std::string kernel_release{"unknown"};
  std::string machine{"unknown"};
};

struct CaseResult {
  std::string run_id;
  std::uint64_t case_id{0U};
  std::size_t trial{0U};
  std::size_t payload_bytes{0U};
  std::size_t warmup_iterations{0U};
  std::uint64_t expected_calls{0U};
  std::uint64_t completed_calls{0U};
  std::uint64_t call_failures{0U};
  std::uint64_t payload_mismatches{0U};
  std::uint64_t service_validation_failures{0U};
  std::uint64_t logical_payload_bytes{0U};
  double wall_time_ms{0.0};
  double calls_per_second{0.0};
  double payload_mib_per_second{0.0};
  double p50_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
  double p99_9_us{0.0};
  double max_us{0.0};
  double user_cpu_ms{0.0};
  double system_cpu_ms{0.0};
  double cpu_time_ms{0.0};
  double cpu_utilization_percent{0.0};
  std::uint64_t voluntary_context_switches{0U};
  std::uint64_t involuntary_context_switches{0U};
  std::uint64_t peak_rss_kib{0U};

  [[nodiscard]] bool ok() const noexcept {
    return completed_calls == expected_calls &&
           call_failures == 0U &&
           payload_mismatches == 0U &&
           service_validation_failures == 0U;
  }
};

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

[[nodiscard]] std::uint64_t ParseUnsigned(
    std::string_view text, std::string_view option_name) {
  std::uint64_t value = 0U;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument(
        std::string(option_name) + " requires an unsigned integer");
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
          << "usage: autoruntime_dds_rpc_benchmark "
             "[--payload BYTES] [--iterations N] [--warmup N] "
             "[--trials N] [--domain ID] [--output FILE] "
             "[--self-test]\n";
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
            "--payload must be between 8 and 1048576 bytes");
      }
      if (!explicit_payload) {
        options.payload_sizes.clear();
        explicit_payload = true;
      }
      options.payload_sizes.push_back(
          static_cast<std::size_t>(value));
      continue;
    }
    if (argument == "--iterations" ||
        argument.starts_with("--iterations=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--iterations"),
          "--iterations");
      if (value == 0U || value > kMaximumIterations) {
        throw std::invalid_argument(
            "--iterations must be between 1 and 1000000");
      }
      options.iterations = static_cast<std::size_t>(value);
      continue;
    }
    if (argument == "--warmup" ||
        argument.starts_with("--warmup=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--warmup"),
          "--warmup");
      if (value > kMaximumIterations) {
        throw std::invalid_argument(
            "--warmup must not exceed 1000000");
      }
      options.warmup_iterations = static_cast<std::size_t>(value);
      continue;
    }
    if (argument == "--trials" ||
        argument.starts_with("--trials=")) {
      const auto value = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--trials"),
          "--trials");
      if (value == 0U || value > kMaximumTrials) {
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
    options.payload_sizes = {64U};
    options.iterations = 50U;
    options.warmup_iterations = 10U;
    options.trials = 1U;
  }
  return options;
}

[[nodiscard]] std::string EscapeJson(std::string_view value) {
  std::ostringstream output;
  for (const char raw_character : value) {
    const auto character =
        static_cast<unsigned char>(raw_character);
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
        if (character < 0x20U) {
          output << "\\u" << std::hex << std::setw(4)
                 << std::setfill('0')
                 << static_cast<unsigned int>(character)
                 << std::dec << std::setfill(' ');
        } else {
          output << static_cast<char>(character);
        }
        break;
    }
  }
  return output.str();
}

[[nodiscard]] std::string UtcTimestamp(
    const char* format = "%Y-%m-%dT%H:%M:%SZ") {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  if (::gmtime_r(&now, &utc) == nullptr) {
    return "unknown";
  }
  std::ostringstream output;
  output << std::put_time(&utc, format);
  return output.str();
}

[[nodiscard]] std::string MakeRunId() {
  std::ostringstream output;
  output << "dds-rpc-" << UtcTimestamp("%Y%m%dT%H%M%SZ")
         << "-" << static_cast<long long>(::getpid());
  return output.str();
}

[[nodiscard]] HostInfo CaptureHostInfo() {
  HostInfo result;
  utsname value{};
  if (::uname(&value) == 0) {
    result.hostname = value.nodename;
    result.kernel_release = value.release;
    result.machine = value.machine;
  }
  return result;
}

[[nodiscard]] std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1U);
}

[[nodiscard]] std::string ReadCpuModel() {
  std::ifstream input("/proc/cpuinfo");
  std::string line;
  while (std::getline(input, line)) {
    constexpr std::string_view label = "model name";
    if (!std::string_view(line).starts_with(label)) {
      continue;
    }
    const auto separator = line.find(':');
    if (separator != std::string::npos) {
      return Trim(line.substr(separator + 1U));
    }
  }
  return "unknown";
}

[[nodiscard]] std::uint64_t ReadMemoryTotalKib() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  std::uint64_t value = 0U;
  std::string unit;
  while (input >> key >> value >> unit) {
    if (key == "MemTotal:") {
      return value;
    }
  }
  return 0U;
}

[[nodiscard]] std::uint32_t DefaultDomainId() noexcept {
  return 20U +
      static_cast<std::uint32_t>(
          static_cast<unsigned long long>(::getpid()) % 180ULL);
}

[[nodiscard]] std::byte ExpectedByte(
    std::uint64_t sequence, std::size_t index) noexcept {
  return static_cast<std::byte>(
      (sequence + static_cast<std::uint64_t>(index) * 131U +
       17U) %
      251U);
}

[[nodiscard]] std::vector<std::byte> PreparePayload(
    std::size_t size, std::uint64_t sequence) {
  std::vector<std::byte> payload(size);
  std::memcpy(payload.data(), &sequence, sizeof(sequence));
  for (std::size_t index = sizeof(sequence);
       index < payload.size(); ++index) {
    payload[index] = ExpectedByte(sequence, index);
  }
  return payload;
}

[[nodiscard]] bool ValidatePayload(
    std::span<const std::byte> payload) noexcept {
  if (payload.size() < sizeof(std::uint64_t)) {
    return false;
  }
  std::uint64_t sequence = 0U;
  std::memcpy(&sequence, payload.data(), sizeof(sequence));
  for (std::size_t index = sizeof(sequence);
       index < payload.size(); ++index) {
    if (payload[index] != ExpectedByte(sequence, index)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] double Quantile(
    const std::vector<double>& sorted, double probability) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double rank =
      std::ceil(probability * static_cast<double>(sorted.size()));
  const auto index = static_cast<std::size_t>(
      std::max(1.0, rank) - 1.0);
  return sorted[std::min(index, sorted.size() - 1U)];
}

[[nodiscard]] std::uint64_t CounterDelta(
    std::uint64_t after, std::uint64_t before) noexcept {
  return after >= before ? after - before : 0U;
}

class RpcHarness {
 public:
  explicit RpcHarness(std::uint32_t domain_id)
      : executor_(std::make_shared<autoruntime::Executor>()) {
    autoruntime::DdsTransportConfig server_config;
    server_config.domain_id = domain_id;
    server_config.participant_name = "dds-rpc-benchmark-server";
    server_config.receive_poll_interval = 1ms;
    server_config.reliability_max_blocking_time = 250ms;

    auto client_config = server_config;
    client_config.participant_name = "dds-rpc-benchmark-client";

    server_transport_ = Take(
        autoruntime::DdsTransport::Create(std::move(server_config)),
        "create benchmark server DDS participant");
    client_transport_ = Take(
        autoruntime::DdsTransport::Create(std::move(client_config)),
        "create benchmark client DDS participant");

    autoruntime::CallbackGroupConfig group_config;
    group_config.name = "dds-rpc-benchmark-service";
    group_config.worker_count = 2U;
    group_config.queue_capacity = 256U;
    const auto group = Take(
        executor_->CreateCallbackGroup(std::move(group_config)),
        "create benchmark service callback group");

    autoruntime::Node server(
        {"dds-rpc-benchmark-server", 1U}, executor_,
        server_transport_);
    autoruntime::Node client(
        {"dds-rpc-benchmark-client", 1U}, executor_,
        client_transport_);

    service_ = Take(
        server.CreateService(
            std::string(kServiceName), group,
            [this](const autoruntime::Message& request)
                -> autoruntime::Result<std::vector<std::byte>> {
              if (!ValidatePayload(request.payload)) {
                service_validation_failures_.fetch_add(
                    1U, std::memory_order_relaxed);
                return autoruntime::Status(
                    autoruntime::StatusCode::InvalidArgument,
                    "DDS RPC benchmark request payload is corrupt");
              }
              return request.payload;
            }),
        "create benchmark DDS service");
    client_ = Take(
        client.CreateClient(std::string(kServiceName)),
        "create benchmark DDS client");
    RequireStatus(executor_->Start(), "start benchmark executor");
    WaitForDiscovery();
  }

  ~RpcHarness() {
    static_cast<void>(service_.Close());
    static_cast<void>(
        executor_->Stop(autoruntime::Deadline::After(2s)));
    static_cast<void>(client_transport_->Close());
    static_cast<void>(server_transport_->Close());
  }

  RpcHarness(const RpcHarness&) = delete;
  RpcHarness& operator=(const RpcHarness&) = delete;

  [[nodiscard]] autoruntime::Result<autoruntime::Message> Call(
      std::span<const std::byte> payload,
      std::uint64_t sequence) const {
    return client_.Call(
        payload, autoruntime::Deadline::After(5s),
        autoruntime::TraceContext{
            sequence + 1U, 0U,
            autoruntime::MonotonicNanoseconds()});
  }

  [[nodiscard]] std::uint64_t service_validation_failures()
      const noexcept {
    return service_validation_failures_.load(
        std::memory_order_relaxed);
  }

 private:
  void WaitForDiscovery() {
    const auto probe = PreparePayload(64U, 0U);
    const auto expires = Clock::now() + 5s;
    while (Clock::now() < expires) {
      auto response = client_.Call(
          probe, autoruntime::Deadline::After(300ms));
      if (response) {
        Require(
            response.value().payload == probe &&
                ValidatePayload(response.value().payload),
            "DDS discovery probe returned an invalid payload");
        return;
      }
      if (response.status().code() !=
          autoruntime::StatusCode::Timeout) {
        throw std::runtime_error(
            "DDS discovery probe failed: " +
            response.status().detail());
      }
      std::this_thread::sleep_for(50ms);
    }
    throw std::runtime_error(
        "DDS discovery did not complete within 5 seconds");
  }

  std::shared_ptr<autoruntime::Executor> executor_;
  std::shared_ptr<autoruntime::DdsTransport> server_transport_;
  std::shared_ptr<autoruntime::DdsTransport> client_transport_;
  autoruntime::Service service_;
  autoruntime::Client client_;
  std::atomic<std::uint64_t> service_validation_failures_{0U};
};

void RunWarmup(RpcHarness& harness, std::size_t payload_bytes,
               std::size_t warmup_iterations,
               std::uint64_t sequence_base) {
  for (std::size_t index = 0U;
       index < warmup_iterations; ++index) {
    const auto sequence =
        sequence_base + static_cast<std::uint64_t>(index);
    const auto payload = PreparePayload(payload_bytes, sequence);
    auto response = harness.Call(payload, sequence);
    if (!response) {
      throw std::runtime_error(
          "DDS RPC benchmark warmup failed: " +
          response.status().detail());
    }
    Require(
        response.value().payload == payload &&
            ValidatePayload(response.value().payload),
        "DDS RPC benchmark warmup payload mismatch");
  }
}

[[nodiscard]] CaseResult RunCase(
    RpcHarness& harness, const Options& options,
    std::string run_id, std::uint64_t case_id,
    std::size_t trial, std::size_t payload_bytes) {
  const auto sequence_base =
      case_id * 1'000'000'000ULL +
      static_cast<std::uint64_t>(trial) * 10'000'000ULL;
  RunWarmup(
      harness, payload_bytes, options.warmup_iterations,
      sequence_base);

  CaseResult result;
  result.run_id = std::move(run_id);
  result.case_id = case_id;
  result.trial = trial;
  result.payload_bytes = payload_bytes;
  result.warmup_iterations = options.warmup_iterations;
  result.expected_calls =
      static_cast<std::uint64_t>(options.iterations);

  std::vector<double> latencies_us;
  latencies_us.reserve(options.iterations);
  const auto validation_before =
      harness.service_validation_failures();
  const auto cpu_before = autoruntime::SampleProcessCpu();
  const auto wall_start = Clock::now();

  for (std::size_t index = 0U;
       index < options.iterations; ++index) {
    const auto sequence =
        sequence_base +
        static_cast<std::uint64_t>(
            options.warmup_iterations + index);
    const auto payload = PreparePayload(payload_bytes, sequence);
    const auto call_start = Clock::now();
    auto response = harness.Call(payload, sequence);
    const auto call_finish = Clock::now();
    if (!response) {
      ++result.call_failures;
      continue;
    }
    if (response.value().payload != payload ||
        !ValidatePayload(response.value().payload)) {
      ++result.payload_mismatches;
      continue;
    }
    ++result.completed_calls;
    latencies_us.push_back(
        std::chrono::duration<double, std::micro>(
            call_finish - call_start)
            .count());
  }

  const auto wall_finish = Clock::now();
  const auto cpu_after = autoruntime::SampleProcessCpu();
  const auto validation_after =
      harness.service_validation_failures();
  result.service_validation_failures =
      CounterDelta(validation_after, validation_before);
  result.logical_payload_bytes =
      result.completed_calls *
      static_cast<std::uint64_t>(payload_bytes) * 2U;

  const auto wall_seconds =
      std::chrono::duration<double>(wall_finish - wall_start).count();
  result.wall_time_ms = wall_seconds * 1000.0;
  if (wall_seconds > 0.0) {
    result.calls_per_second =
        static_cast<double>(result.completed_calls) / wall_seconds;
    result.payload_mib_per_second =
        static_cast<double>(result.logical_payload_bytes) /
        static_cast<double>(kMebibyte) / wall_seconds;
  }

  std::sort(latencies_us.begin(), latencies_us.end());
  result.p50_us = Quantile(latencies_us, 0.50);
  result.p95_us = Quantile(latencies_us, 0.95);
  result.p99_us = Quantile(latencies_us, 0.99);
  result.p99_9_us = Quantile(latencies_us, 0.999);
  result.max_us =
      latencies_us.empty() ? 0.0 : latencies_us.back();

  if (cpu_before.available && cpu_after.available) {
    result.user_cpu_ms =
        std::max(
            0.0,
            cpu_after.user_seconds - cpu_before.user_seconds) *
        1000.0;
    result.system_cpu_ms =
        std::max(
            0.0,
            cpu_after.system_seconds -
                cpu_before.system_seconds) *
        1000.0;
    result.cpu_time_ms =
        result.user_cpu_ms + result.system_cpu_ms;
    if (result.wall_time_ms > 0.0) {
      result.cpu_utilization_percent =
          result.cpu_time_ms / result.wall_time_ms * 100.0;
    }
    result.voluntary_context_switches = CounterDelta(
        cpu_after.voluntary_context_switches,
        cpu_before.voluntary_context_switches);
    result.involuntary_context_switches = CounterDelta(
        cpu_after.involuntary_context_switches,
        cpu_before.involuntary_context_switches);
    result.peak_rss_kib =
        cpu_after.maximum_resident_bytes / 1024U;
  }
  return result;
}

[[nodiscard]] std::string EnvironmentJson(
    std::string_view run_id, std::uint32_t domain_id) {
  const auto host = CaptureHostInfo();
  const long page_size = ::sysconf(_SC_PAGESIZE);
  std::ostringstream output;
  output << "{\"type\":\"environment\","
         << "\"schema_version\":" << kSchemaVersion << ','
         << "\"benchmark\":\"autoruntime_dds_rpc\","
         << "\"run_id\":\"" << EscapeJson(run_id) << "\","
         << "\"timestamp_utc\":\""
         << EscapeJson(UtcTimestamp()) << "\","
         << "\"hostname\":\"" << EscapeJson(host.hostname) << "\","
         << "\"kernel_release\":\""
         << EscapeJson(host.kernel_release) << "\","
         << "\"machine\":\"" << EscapeJson(host.machine) << "\","
         << "\"cpu_model\":\""
         << EscapeJson(ReadCpuModel()) << "\","
         << "\"compiler\":\""
         << EscapeJson(AUTORUNTIME_COMPILER) << "\","
         << "\"build_type\":\""
         << EscapeJson(AUTORUNTIME_BUILD_TYPE) << "\","
         << "\"source_revision\":\""
         << EscapeJson(AUTORUNTIME_SOURCE_REVISION) << "\","
         << "\"logical_cpus\":"
         << std::thread::hardware_concurrency() << ','
         << "\"page_size_bytes\":"
         << (page_size > 0 ? page_size : 0L) << ','
         << "\"memory_total_kib\":" << ReadMemoryTotalKib() << ','
         << "\"transport\":\"cyclonedds\","
         << "\"rpc_encoding\":\"autoruntime_topic_rpc_v1\","
         << "\"topology\":\"same_process_two_participants\","
         << "\"domain_id\":" << domain_id << "}";
  return output.str();
}

[[nodiscard]] std::string ResultJson(const CaseResult& result) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(3)
         << "{\"type\":\"result\","
         << "\"schema_version\":" << kSchemaVersion << ','
         << "\"benchmark\":\"autoruntime_dds_rpc\","
         << "\"run_id\":\"" << EscapeJson(result.run_id) << "\","
         << "\"case_id\":" << result.case_id << ','
         << "\"trial\":" << result.trial << ','
         << "\"status\":\"" << (result.ok() ? "ok" : "failed")
         << "\","
         << "\"transport\":\"cyclonedds\","
         << "\"api\":\"autoruntime_node_service_client\","
         << "\"topology\":\"same_process_two_participants\","
         << "\"service\":\"" << kServiceName << "\","
         << "\"payload_bytes\":" << result.payload_bytes << ','
         << "\"warmup_iterations\":"
         << result.warmup_iterations << ','
         << "\"expected_calls\":" << result.expected_calls << ','
         << "\"completed_calls\":" << result.completed_calls << ','
         << "\"call_failures\":" << result.call_failures << ','
         << "\"payload_mismatches\":"
         << result.payload_mismatches << ','
         << "\"service_validation_failures\":"
         << result.service_validation_failures << ','
         << "\"logical_payload_bytes\":"
         << result.logical_payload_bytes << ','
         << "\"wall_time_ms\":" << result.wall_time_ms << ','
         << "\"calls_per_second\":"
         << result.calls_per_second << ','
         << "\"payload_mib_per_second\":"
         << result.payload_mib_per_second << ','
         << "\"p50_us\":" << result.p50_us << ','
         << "\"p95_us\":" << result.p95_us << ','
         << "\"p99_us\":" << result.p99_us << ','
         << "\"p99_9_us\":" << result.p99_9_us << ','
         << "\"max_us\":" << result.max_us << ','
         << "\"user_cpu_ms\":" << result.user_cpu_ms << ','
         << "\"system_cpu_ms\":" << result.system_cpu_ms << ','
         << "\"cpu_time_ms\":" << result.cpu_time_ms << ','
         << "\"cpu_utilization_percent\":"
         << result.cpu_utilization_percent << ','
         << "\"voluntary_context_switches\":"
         << result.voluntary_context_switches << ','
         << "\"involuntary_context_switches\":"
         << result.involuntary_context_switches << ','
         << "\"peak_rss_kib\":" << result.peak_rss_kib << "}";
  return output.str();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    const auto domain_id =
        options.domain_id.value_or(DefaultDomainId());
    const auto run_id = MakeRunId();

    std::ofstream output_file;
    std::ostream* output = &std::cout;
    if (!options.output_path.empty()) {
      output_file.open(
          options.output_path, std::ios::out | std::ios::trunc);
      Require(output_file.is_open(), "open benchmark output file");
      output = &output_file;
    }

    *output << EnvironmentJson(run_id, domain_id) << '\n';
    output->flush();

    RpcHarness harness(domain_id);
    bool all_ok = true;
    std::uint64_t case_id = 0U;
    for (const auto payload_bytes : options.payload_sizes) {
      ++case_id;
      for (std::size_t trial = 1U;
           trial <= options.trials; ++trial) {
        const auto result = RunCase(
            harness, options, run_id, case_id, trial,
            payload_bytes);
        *output << ResultJson(result) << '\n';
        output->flush();
        all_ok = all_ok && result.ok();
      }
    }
    return all_ok ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "DDS RPC benchmark failed: "
              << error.what() << '\n';
    return 1;
  }
}
