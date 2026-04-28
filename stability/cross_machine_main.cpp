#include "cross_machine_stability.hpp"

#include <autoruntime/dds_transport.hpp>
#include <autoruntime/distributed.hpp>
#include <autoruntime/message.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kSensorTopic{
    "autoruntime/cross_machine/sensor"};
constexpr std::string_view kPlanningMethod{"planning.process"};
constexpr std::string_view kMonitorMethod{"monitor.status"};
constexpr std::size_t kPayloadBytes = 16U;

std::atomic<bool> g_stop_requested{false};

void RequestStop(int) {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

enum class Role {
  MachineA,
  MachineB,
};

struct Options {
  Role role{Role::MachineA};
  std::string address;
  std::string peer_address;
  std::uint16_t discovery_port{0U};
  std::uint16_t peer_discovery_port{0U};
  std::uint16_t rpc_port{0U};
  std::uint32_t domain_id{73U};
  std::uint64_t generation{1U};
  std::chrono::milliseconds publish_period{20};
  std::chrono::milliseconds probe_period{50};
  std::chrono::milliseconds rpc_timeout{150};
  std::chrono::milliseconds dds_stall_timeout{300};
  std::chrono::milliseconds heartbeat_period{50};
  std::chrono::milliseconds lease_timeout{300};
  std::chrono::milliseconds minimum_duration{0};
  std::size_t latency_window{512U};
  bool require_faults{false};
  std::string output;
};

std::string_view OptionValue(int& index, int argc, char** argv,
                             std::string_view argument,
                             std::string_view name) {
  const std::string prefix = std::string(name) + "=";
  if (argument.starts_with(prefix)) {
    return argument.substr(prefix.size());
  }
  if (index + 1 >= argc) {
    throw std::invalid_argument(
        "missing value for " + std::string(name));
  }
  ++index;
  return argv[index];
}

std::uint64_t ParseUnsigned(std::string_view value,
                            std::string_view name) {
  std::uint64_t parsed = 0U;
  const auto result = std::from_chars(
      value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size()) {
    throw std::invalid_argument(
        "invalid unsigned value for " + std::string(name));
  }
  return parsed;
}

template <typename T>
T CheckedCast(std::uint64_t value, std::string_view name) {
  if (value > static_cast<std::uint64_t>(
                  std::numeric_limits<T>::max())) {
    throw std::invalid_argument(
        std::string(name) + " is out of range");
  }
  return static_cast<T>(value);
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--role" || argument.starts_with("--role=")) {
      const auto value =
          OptionValue(index, argc, argv, argument, "--role");
      if (value == "machine-a") {
        options.role = Role::MachineA;
      } else if (value == "machine-b") {
        options.role = Role::MachineB;
      } else {
        throw std::invalid_argument(
            "role must be machine-a or machine-b");
      }
      continue;
    }
    if (argument == "--address" ||
        argument.starts_with("--address=")) {
      options.address = std::string(
          OptionValue(index, argc, argv, argument, "--address"));
      continue;
    }
    if (argument == "--peer-address" ||
        argument.starts_with("--peer-address=")) {
      options.peer_address = std::string(
          OptionValue(index, argc, argv, argument, "--peer-address"));
      continue;
    }
    if (argument == "--discovery-port" ||
        argument.starts_with("--discovery-port=")) {
      options.discovery_port = CheckedCast<std::uint16_t>(
          ParseUnsigned(
              OptionValue(
                  index, argc, argv, argument, "--discovery-port"),
              "discovery-port"),
          "discovery-port");
      continue;
    }
    if (argument == "--peer-discovery-port" ||
        argument.starts_with("--peer-discovery-port=")) {
      options.peer_discovery_port = CheckedCast<std::uint16_t>(
          ParseUnsigned(
              OptionValue(
                  index, argc, argv, argument,
                  "--peer-discovery-port"),
              "peer-discovery-port"),
          "peer-discovery-port");
      continue;
    }
    if (argument == "--rpc-port" ||
        argument.starts_with("--rpc-port=")) {
      options.rpc_port = CheckedCast<std::uint16_t>(
          ParseUnsigned(
              OptionValue(index, argc, argv, argument, "--rpc-port"),
              "rpc-port"),
          "rpc-port");
      continue;
    }
    if (argument == "--domain-id" ||
        argument.starts_with("--domain-id=")) {
      options.domain_id = CheckedCast<std::uint32_t>(
          ParseUnsigned(
              OptionValue(index, argc, argv, argument, "--domain-id"),
              "domain-id"),
          "domain-id");
      continue;
    }
    if (argument == "--generation" ||
        argument.starts_with("--generation=")) {
      options.generation = ParseUnsigned(
          OptionValue(index, argc, argv, argument, "--generation"),
          "generation");
      continue;
    }
    if (argument == "--publish-period-ms" ||
        argument.starts_with("--publish-period-ms=")) {
      options.publish_period = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument,
                      "--publish-period-ms"),
                  "publish-period-ms"),
              "publish-period-ms"));
      continue;
    }
    if (argument == "--probe-period-ms" ||
        argument.starts_with("--probe-period-ms=")) {
      options.probe_period = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument,
                      "--probe-period-ms"),
                  "probe-period-ms"),
              "probe-period-ms"));
      continue;
    }
    if (argument == "--rpc-timeout-ms" ||
        argument.starts_with("--rpc-timeout-ms=")) {
      options.rpc_timeout = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument, "--rpc-timeout-ms"),
                  "rpc-timeout-ms"),
              "rpc-timeout-ms"));
      continue;
    }
    if (argument == "--dds-stall-ms" ||
        argument.starts_with("--dds-stall-ms=")) {
      options.dds_stall_timeout = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument, "--dds-stall-ms"),
                  "dds-stall-ms"),
              "dds-stall-ms"));
      continue;
    }
    if (argument == "--heartbeat-ms" ||
        argument.starts_with("--heartbeat-ms=")) {
      options.heartbeat_period = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument, "--heartbeat-ms"),
                  "heartbeat-ms"),
              "heartbeat-ms"));
      continue;
    }
    if (argument == "--lease-ms" ||
        argument.starts_with("--lease-ms=")) {
      options.lease_timeout = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(index, argc, argv, argument, "--lease-ms"),
                  "lease-ms"),
              "lease-ms"));
      continue;
    }
    if (argument == "--minimum-duration-ms" ||
        argument.starts_with("--minimum-duration-ms=")) {
      options.minimum_duration = std::chrono::milliseconds(
          CheckedCast<std::chrono::milliseconds::rep>(
              ParseUnsigned(
                  OptionValue(
                      index, argc, argv, argument,
                      "--minimum-duration-ms"),
                  "minimum-duration-ms"),
              "minimum-duration-ms"));
      continue;
    }
    if (argument == "--latency-window" ||
        argument.starts_with("--latency-window=")) {
      options.latency_window = CheckedCast<std::size_t>(
          ParseUnsigned(
              OptionValue(
                  index, argc, argv, argument, "--latency-window"),
              "latency-window"),
          "latency-window");
      continue;
    }
    if (argument == "--output" ||
        argument.starts_with("--output=")) {
      options.output = std::string(
          OptionValue(index, argc, argv, argument, "--output"));
      continue;
    }
    if (argument == "--require-faults") {
      options.require_faults = true;
      continue;
    }
    if (argument == "--help") {
      std::cout
          << "usage: autoruntime_cross_machine_stability "
             "--role machine-a|machine-b --address IP "
             "--peer-address IP --discovery-port PORT "
             "--peer-discovery-port PORT --rpc-port PORT "
             "--output FILE [options]\n";
      std::exit(0);
    }
    throw std::invalid_argument(
        "unknown argument: " + std::string(argument));
  }

  if (options.address.empty() || options.peer_address.empty() ||
      options.discovery_port == 0U ||
      options.peer_discovery_port == 0U || options.rpc_port == 0U ||
      options.generation == 0U || options.publish_period <= 0ms ||
      options.probe_period <= 0ms || options.rpc_timeout <= 0ms ||
      options.dds_stall_timeout <= 0ms ||
      options.heartbeat_period <= 0ms ||
      options.lease_timeout <= options.heartbeat_period ||
      options.latency_window == 0U || options.output.empty()) {
    throw std::invalid_argument(
        "invalid or incomplete cross-machine options");
  }
  return options;
}

template <typename T>
T Take(autoruntime::Result<T> result, std::string_view operation) {
  if (!result) {
    throw std::runtime_error(
        std::string(operation) + ": " + result.status().detail());
  }
  return std::move(result).take_value();
}

void Require(autoruntime::Status status, std::string_view operation) {
  if (!status) {
    throw std::runtime_error(
        std::string(operation) + ": " + status.detail());
  }
}

void WriteLine(std::ofstream& output, const std::string& line) {
  output << line << '\n';
  output.flush();
  if (!output) {
    throw std::runtime_error("failed to write cross-machine JSONL");
  }
}

std::string EscapeJson(std::string_view input) {
  std::string escaped;
  escaped.reserve(input.size());
  for (const char value : input) {
    switch (value) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(value);
        break;
    }
  }
  return escaped;
}

std::string Hostname() {
  std::array<char, 256U> name{};
  if (::gethostname(name.data(), name.size()) != 0) {
    return "unknown";
  }
  name.back() = '\0';
  return name.data();
}

std::string KernelRelease() {
  utsname information{};
  if (::uname(&information) != 0) {
    return "unknown";
  }
  return information.release;
}

std::uint64_t MonotonicNanoseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

void StoreU64(std::byte* destination, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    const auto shift = static_cast<unsigned int>((7U - index) * 8U);
    destination[index] =
        static_cast<std::byte>((value >> shift) & 0xFFU);
  }
}

std::uint64_t LoadU64(const std::byte* source) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value = (value << 8U) |
            static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(source[index]));
  }
  return value;
}

std::vector<std::byte> Payload(std::uint64_t sequence) {
  std::vector<std::byte> payload(kPayloadBytes);
  StoreU64(payload.data(), sequence);
  StoreU64(payload.data() + 8U, ~sequence);
  return payload;
}

bool ValidPayload(std::span<const std::byte> payload) {
  if (payload.size() != kPayloadBytes) {
    return false;
  }
  const auto sequence = LoadU64(payload.data());
  return LoadU64(payload.data() + 8U) == ~sequence;
}

autoruntime::QosProfile SensorQos() {
  autoruntime::QosProfile qos;
  qos.reliability = autoruntime::Reliability::BestEffort;
  qos.history = autoruntime::HistoryKind::KeepLast;
  qos.depth = 32U;
  qos.liveliness = autoruntime::LivelinessKind::Automatic;
  qos.liveliness_lease = 500ms;
  return qos;
}

autoruntime::DdsTransportConfig DdsConfig(
    const Options& options, std::string participant) {
  autoruntime::DdsTransportConfig config;
  config.domain_id = options.domain_id;
  config.participant_name = std::move(participant);
  config.receive_poll_interval = 2ms;
  config.reliability_max_blocking_time = 100ms;
  config.max_payload_size = 4096U;
  return config;
}

autoruntime::DiscoveryConfig DiscoveryConfigFor(
    const Options& options, std::string node_id,
    autoruntime::NetworkEndpoint rpc_endpoint) {
  autoruntime::DiscoveryConfig config;
  config.node_id = std::move(node_id);
  config.generation = options.generation;
  config.bind_endpoint = {options.address, options.discovery_port};
  config.advertised_rpc_endpoint = std::move(rpc_endpoint);
  config.peers.push_back(
      {options.peer_address, options.peer_discovery_port});
  config.heartbeat_period = options.heartbeat_period;
  config.lease_timeout = options.lease_timeout;
  config.max_members = 4U;
  config.max_peers = 4U;
  return config;
}

std::string StatusCodeName(autoruntime::StatusCode code) {
  switch (code) {
    case autoruntime::StatusCode::Ok:
      return "ok";
    case autoruntime::StatusCode::InvalidArgument:
      return "invalid_argument";
    case autoruntime::StatusCode::AlreadyExists:
      return "already_exists";
    case autoruntime::StatusCode::NotFound:
      return "not_found";
    case autoruntime::StatusCode::Closed:
      return "closed";
    case autoruntime::StatusCode::Timeout:
      return "timeout";
    case autoruntime::StatusCode::QueueFull:
      return "queue_full";
    case autoruntime::StatusCode::Dropped:
      return "dropped";
    case autoruntime::StatusCode::Cancelled:
      return "cancelled";
    case autoruntime::StatusCode::StaleGeneration:
      return "stale_generation";
    case autoruntime::StatusCode::TransportError:
      return "transport_error";
    case autoruntime::StatusCode::Unsupported:
      return "unsupported";
    case autoruntime::StatusCode::Internal:
      return "internal";
  }
  return "unknown";
}

void WriteEnvironment(std::ofstream& output, const Options& options,
                      std::string_view role) {
  WriteLine(
      output,
      "{\"schema_version\":1,\"record_type\":\"environment\","
      "\"runner\":\"autoruntime_cross_machine_stability\","
      "\"role\":\"" + std::string(role) + "\","
      "\"source_revision\":\"" AUTORUNTIME_SOURCE_REVISION "\","
      "\"build_type\":\"" AUTORUNTIME_BUILD_TYPE "\","
      "\"compiler\":\"" AUTORUNTIME_COMPILER "\","
      "\"hostname\":\"" + EscapeJson(Hostname()) + "\","
      "\"kernel_release\":\"" + EscapeJson(KernelRelease()) + "\","
      "\"address\":\"" + EscapeJson(options.address) + "\","
      "\"peer_address\":\"" + EscapeJson(options.peer_address) + "\","
      "\"generation\":" + std::to_string(options.generation) + ","
      "\"domain_id\":" + std::to_string(options.domain_id) + "}");
}

bool DurationReached(const Options& options, Clock::time_point started) {
  return options.minimum_duration > 0ms &&
         Clock::now() - started >= options.minimum_duration;
}

int RunMachineA(const Options& options, std::ofstream& output) {
  WriteEnvironment(output, options, "machine_a_sensor_planning");

  autoruntime::RpcServerConfig rpc_config;
  rpc_config.bind_endpoint = {options.address, options.rpc_port};
  rpc_config.max_request_bytes = 4096U;
  rpc_config.io_timeout = 1s;
  auto rpc = Take(
      autoruntime::RpcServer::Create(rpc_config), "create planning RPC");
  Require(
      rpc->RegisterHandler(
          std::string(kPlanningMethod),
          [](std::span<const std::byte> request)
              -> autoruntime::Result<std::vector<std::byte>> {
            if (!ValidPayload(request)) {
              return autoruntime::Status(
                  autoruntime::StatusCode::InvalidArgument,
                  "planning request payload is malformed");
            }
            return std::vector<std::byte>(request.begin(), request.end());
          }),
      "register planning RPC");
  Require(rpc->Start(), "start planning RPC");

  auto discovery = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfigFor(
              options, "machine-a",
              {options.address, options.rpc_port})),
      "create machine-a discovery");
  Require(discovery->Start(), "start machine-a discovery");

  auto dds = Take(
      autoruntime::DdsTransport::Create(
          DdsConfig(
              options,
              "machine-a-sensor-g" +
                  std::to_string(options.generation))),
      "create machine-a DDS");
  const auto qos = SensorQos();
  WriteLine(
      output,
      "{\"schema_version\":1,\"record_type\":\"ready\","
      "\"role\":\"machine_a\",\"generation\":" +
          std::to_string(options.generation) + "}");

  std::uint64_t sequence = 0U;
  std::uint64_t local_publish_failures = 0U;
  const auto started = Clock::now();
  auto next_release = started;
  while (!g_stop_requested.load(std::memory_order_relaxed) &&
         !DurationReached(options, started)) {
    ++sequence;
    autoruntime::Message message;
    message.envelope.trace_id = sequence;
    message.envelope.span_id = sequence;
    message.envelope.sequence = sequence;
    message.envelope.source_timestamp_ns =
        autoruntime::MonotonicNanoseconds();
    message.envelope.publish_timestamp_ns =
        autoruntime::MonotonicNanoseconds();
    message.envelope.source_generation = options.generation;
    message.payload = Payload(sequence);
    const auto status =
        dds->Publish(kSensorTopic, std::move(message), qos);
    if (!status) {
      ++local_publish_failures;
    }
    if (sequence % 50U == 0U) {
      const auto dds_stats = dds->Stats();
      const auto discovery_stats = discovery->Stats();
      const auto rpc_stats = rpc->Stats();
      WriteLine(
          output,
          "{\"schema_version\":1,\"record_type\":\"observation\","
          "\"role\":\"machine_a\",\"sequence\":" +
              std::to_string(sequence) +
              ",\"dds_published\":" +
              std::to_string(dds_stats.published_messages) +
              ",\"discovery_sent\":" +
              std::to_string(discovery_stats.announcements_sent) +
              ",\"rpc_requests\":" +
              std::to_string(rpc_stats.requests) + "}");
    }
    next_release += options.publish_period;
    if (next_release < Clock::now()) {
      next_release = Clock::now();
    }
    std::this_thread::sleep_until(next_release);
  }

  Require(discovery->Stop(), "stop machine-a discovery");
  Require(rpc->Stop(), "stop planning RPC");
  Require(dds->Close(), "close machine-a DDS");
  const auto dds_stats = dds->Stats();
  const auto discovery_stats = discovery->Stats();
  const auto rpc_stats = rpc->Stats();
  const bool passed =
      local_publish_failures == 0U && rpc_stats.malformed_requests == 0U;
  WriteLine(
      output,
      "{\"schema_version\":1,\"record_type\":\"summary\","
      "\"role\":\"machine_a\",\"status\":\"" +
          std::string(passed ? "passed" : "failed") +
          "\",\"generation\":" + std::to_string(options.generation) +
          ",\"dds_published\":" +
          std::to_string(dds_stats.published_messages) +
          ",\"dds_publish_failures\":" +
          std::to_string(dds_stats.publish_failures) +
          ",\"discovery_parse_errors\":" +
          std::to_string(discovery_stats.parse_errors) +
          ",\"rpc_requests\":" + std::to_string(rpc_stats.requests) +
          ",\"rpc_parse_errors\":" +
          std::to_string(rpc_stats.malformed_requests) +
          ",\"rpc_io_failures\":" +
          std::to_string(rpc_stats.io_failures) + "}");
  return passed ? 0 : 1;
}

int RunMachineB(const Options& options, std::ofstream& output) {
  WriteEnvironment(output, options, "machine_b_control_monitor");

  autoruntime::stability::StabilityTracker tracker(
      options.latency_window);
  std::atomic<std::uint64_t> last_dds_at_ns{0U};
  std::atomic<std::uint64_t> payload_errors{0U};

  auto dds = Take(
      autoruntime::DdsTransport::Create(
          DdsConfig(options, "machine-b-control-monitor")),
      "create machine-b DDS");
  const auto qos = SensorQos();
  auto subscription = Take(
      dds->Subscribe(
          kSensorTopic, qos,
          [&](autoruntime::Message message) {
            if (!ValidPayload(message.payload) ||
                LoadU64(message.payload.data()) !=
                    message.envelope.sequence) {
              payload_errors.fetch_add(1U, std::memory_order_relaxed);
              return;
            }
            tracker.ObserveDds(
                message.envelope.source_generation,
                message.envelope.sequence);
            last_dds_at_ns.store(
                MonotonicNanoseconds(), std::memory_order_release);
          }),
      "subscribe machine-b DDS");

  autoruntime::RpcServerConfig rpc_config;
  rpc_config.bind_endpoint = {options.address, options.rpc_port};
  rpc_config.max_request_bytes = 4096U;
  rpc_config.io_timeout = 1s;
  auto rpc = Take(
      autoruntime::RpcServer::Create(rpc_config), "create monitor RPC");
  Require(
      rpc->RegisterHandler(
          std::string(kMonitorMethod),
          [](std::span<const std::byte> request)
              -> autoruntime::Result<std::vector<std::byte>> {
            return std::vector<std::byte>(request.begin(), request.end());
          }),
      "register monitor RPC");
  Require(rpc->Start(), "start monitor RPC");

  auto discovery = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfigFor(
              options, "machine-b",
              {options.address, options.rpc_port})),
      "create machine-b discovery");
  Require(discovery->Start(), "start machine-b discovery");
  WriteLine(
      output,
      "{\"schema_version\":1,\"record_type\":\"ready\","
      "\"role\":\"machine_b\",\"generation\":" +
          std::to_string(options.generation) + "}");

  const auto started = Clock::now();
  auto next_release = started;
  std::uint64_t probe_index = 0U;
  while (!g_stop_requested.load(std::memory_order_relaxed) &&
         !DurationReached(options, started)) {
    ++probe_index;
    autoruntime::stability::ProbeObservation observation;
    const auto member = discovery->Find("machine-a");
    if (member) {
      observation.discovery_available = true;
      observation.peer_generation = member.value().generation;
      const auto request = Payload(probe_index);
      const auto rpc_started = Clock::now();
      const auto response = autoruntime::RpcClient::Call(
          member.value().rpc_endpoint, kPlanningMethod, request,
          autoruntime::Deadline::After(options.rpc_timeout));
      if (!response) {
        observation.rpc_status = response.status().code();
      } else if (response.value() != request) {
        observation.rpc_status =
            autoruntime::StatusCode::InvalidArgument;
      } else {
        observation.rpc_status = autoruntime::StatusCode::Ok;
        observation.rpc_latency_us =
            std::chrono::duration<double, std::micro>(
                Clock::now() - rpc_started)
                .count();
      }
    } else {
      observation.discovery_available = false;
      observation.rpc_status = autoruntime::StatusCode::NotFound;
    }

    const auto last_dds =
        last_dds_at_ns.load(std::memory_order_acquire);
    const auto now_ns = MonotonicNanoseconds();
    const auto stall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            options.dds_stall_timeout)
            .count());
    observation.dds_recent =
        last_dds != 0U && now_ns >= last_dds &&
        now_ns - last_dds <= stall_ns;

    const auto transition = tracker.ObserveProbe(observation);
    if (transition) {
      WriteLine(
          output,
          "{\"schema_version\":1,\"record_type\":\"health_transition\","
          "\"observation_index\":" +
              std::to_string(transition->observation_index) +
              ",\"previous\":\"" +
              std::string(
                  autoruntime::stability::NetworkHealthStateName(
                      transition->previous)) +
              "\",\"current\":\"" +
              std::string(
                  autoruntime::stability::NetworkHealthStateName(
                      transition->current)) +
              "\",\"reason\":\"" +
              std::string(
                  autoruntime::stability::NetworkHealthReasonName(
                      transition->reason)) +
              "\"}");
    }

    WriteLine(
        output,
        "{\"schema_version\":1,\"record_type\":\"probe\","
        "\"observation_index\":" + std::to_string(probe_index) +
        ",\"discovery_available\":" +
        std::string(observation.discovery_available ? "true" : "false") +
        ",\"peer_generation\":" +
        std::to_string(observation.peer_generation) +
        ",\"rpc_status\":\"" +
        StatusCodeName(observation.rpc_status) +
        "\",\"dds_recent\":" +
        std::string(observation.dds_recent ? "true" : "false") +
        ",\"rpc_latency_us\":" +
        (observation.rpc_latency_us
             ? std::to_string(*observation.rpc_latency_us)
             : std::string("null")) +
        "}");

    next_release += options.probe_period;
    if (next_release < Clock::now()) {
      next_release = Clock::now();
    }
    std::this_thread::sleep_until(next_release);
  }

  Require(discovery->Stop(), "stop machine-b discovery");
  Require(rpc->Stop(), "stop monitor RPC");
  Require(dds->Unsubscribe(subscription), "unsubscribe machine-b DDS");
  Require(dds->Close(), "close machine-b DDS");
  const auto rpc_stats = rpc->Stats();
  tracker.ObserveRpcParseErrors(rpc_stats.malformed_requests);
  const auto summary = tracker.Summary();
  const auto transport_failures =
      summary.rpc_timeouts + summary.rpc_transport_errors;
  bool passed =
      summary.rpc_unexpected_errors == 0U &&
      summary.rpc_parse_errors == 0U &&
      payload_errors.load(std::memory_order_relaxed) == 0U &&
      summary.final_state ==
          autoruntime::stability::NetworkHealthState::Running;
  if (options.require_faults) {
    passed =
        passed && summary.discovery_loss_events > 0U &&
        summary.discovery_recovery_events > 0U &&
        summary.peer_generation_changes > 0U &&
        transport_failures > 0U && summary.dds_stall_observations > 0U &&
        summary.message_loss > 0U &&
        summary.health_transitions > 0U &&
        summary.recovery_count > 0U;
  }

  WriteLine(
      output,
      "{\"schema_version\":1,\"record_type\":\"summary\","
      "\"role\":\"machine_b\",\"status\":\"" +
          std::string(passed ? "passed" : "failed") +
          "\",\"actual_duration_ms\":" +
          std::to_string(
              std::chrono::duration<double, std::milli>(
                  Clock::now() - started)
                  .count()) +
          ",\"observation_count\":" +
          std::to_string(summary.observation_count) +
          ",\"discovery_failures\":" +
          std::to_string(summary.discovery_failures) +
          ",\"discovery_loss_events\":" +
          std::to_string(summary.discovery_loss_events) +
          ",\"discovery_recovery_events\":" +
          std::to_string(summary.discovery_recovery_events) +
          ",\"peer_generation_changes\":" +
          std::to_string(summary.peer_generation_changes) +
          ",\"rpc_successes\":" +
          std::to_string(summary.rpc_successes) +
          ",\"rpc_timeouts\":" +
          std::to_string(summary.rpc_timeouts) +
          ",\"rpc_transport_errors\":" +
          std::to_string(summary.rpc_transport_errors) +
          ",\"rpc_unexpected_errors\":" +
          std::to_string(summary.rpc_unexpected_errors) +
          ",\"rpc_parse_errors\":" +
          std::to_string(summary.rpc_parse_errors) +
          ",\"dds_messages\":" +
          std::to_string(summary.dds_messages) +
          ",\"dds_stall_observations\":" +
          std::to_string(summary.dds_stall_observations) +
          ",\"message_loss\":" +
          std::to_string(summary.message_loss) +
          ",\"duplicate_messages\":" +
          std::to_string(summary.duplicate_messages) +
          ",\"payload_errors\":" +
          std::to_string(
              payload_errors.load(std::memory_order_relaxed)) +
          ",\"health_transitions\":" +
          std::to_string(summary.health_transitions) +
          ",\"recovery_count\":" +
          std::to_string(summary.recovery_count) +
          ",\"latency_samples\":" +
          std::to_string(summary.latency_samples) +
          ",\"retained_latency_samples\":" +
          std::to_string(summary.retained_latency_samples) +
          ",\"baseline_p99_us\":" +
          std::to_string(summary.baseline_p99_us) +
          ",\"final_p99_us\":" +
          std::to_string(summary.final_p99_us) +
          ",\"p99_drift_us\":" +
          std::to_string(summary.p99_drift_us) +
          ",\"final_state\":\"" +
          std::string(
              autoruntime::stability::NetworkHealthStateName(
                  summary.final_state)) +
          "\"}");
  return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    std::signal(SIGINT, RequestStop);
    std::signal(SIGTERM, RequestStop);
    std::ofstream output(options.output, std::ios::trunc);
    if (!output) {
      throw std::runtime_error(
          "failed to open cross-machine output file");
    }
    if (options.role == Role::MachineA) {
      return RunMachineA(options, output);
    }
    return RunMachineB(options, output);
  } catch (const std::exception& error) {
    std::cerr << "cross-machine runner failed: " << error.what() << '\n';
    return 2;
  }
}
