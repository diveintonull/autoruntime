#include <autoruntime/observability.hpp>
#include <autoruntime/record_replay.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
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

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

void Require(bool condition, std::string_view detail) {
  if (!condition) {
    throw std::runtime_error(std::string(detail));
  }
}

void RequireStatus(
    const autoruntime::Status& status,
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
        std::string(operation) + ": " +
        result.status().detail());
  }
  return std::move(result).take_value();
}

struct Options {
  std::string output_path;
  std::uint64_t message_count{5000U};
  std::size_t payload_size{256U};
  std::uint64_t period_us{100U};
};

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      std::cout
          << "usage: autoruntime_record_replay_experiment "
             "[--output FILE] [--messages N] "
             "[--payload-size N] [--period-us N]\n";
      std::exit(0);
    }
    if (argument == "--output" && index + 1 < argc) {
      options.output_path = argv[++index];
      continue;
    }
    if (argument == "--messages" && index + 1 < argc) {
      options.message_count =
          std::stoull(argv[++index]);
      continue;
    }
    if (argument == "--payload-size" && index + 1 < argc) {
      const auto parsed = std::stoull(argv[++index]);
      Require(
          parsed <=
              static_cast<unsigned long long>(
                  std::numeric_limits<std::size_t>::max()),
          "payload size exceeds size_t");
      options.payload_size =
          static_cast<std::size_t>(parsed);
      continue;
    }
    if (argument == "--period-us" && index + 1 < argc) {
      options.period_us = std::stoull(argv[++index]);
      continue;
    }
    throw std::invalid_argument(
        "unknown or incomplete argument");
  }
  Require(options.message_count > 0U, "message count must be positive");
  Require(options.message_count <= 1'000'000U,
          "message count is unreasonably large");
  Require(options.payload_size > 0U, "payload size must be positive");
  Require(options.payload_size <= 64U * 1024U * 1024U,
          "payload size exceeds trace limit");
  Require(options.period_us > 0U, "period must be positive");
  Require(
      options.period_us <=
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) /
          options.message_count,
      "recording duration overflows");
  return options;
}

class TemporaryTrace {
 public:
  TemporaryTrace() {
    path_ = (std::filesystem::temp_directory_path() /
             ("autoruntime-record-replay-" +
              std::to_string(
                  autoruntime::MonotonicNanoseconds()) +
              ".artrace"))
                .string();
  }

  ~TemporaryTrace() {
    std::error_code error;
    static_cast<void>(
        std::filesystem::remove(path_, error));
  }

  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }

 private:
  std::string path_;
};

std::vector<std::byte> Payload(
    std::uint64_t sequence, std::size_t size) {
  std::vector<std::byte> payload(size);
  for (std::size_t index = 0U; index < size; ++index) {
    payload[index] = static_cast<std::byte>(
        (sequence * 131U +
         static_cast<std::uint64_t>(index) * 17U) &
        0xffU);
  }
  return payload;
}

autoruntime::Message MessageFor(
    std::uint64_t sequence, std::size_t payload_size) {
  autoruntime::Message message;
  message.envelope.version =
      autoruntime::kMessageEnvelopeVersion;
  message.envelope.trace_id = 10'000U + sequence;
  message.envelope.span_id = 20'000U + sequence;
  message.envelope.parent_span_id = 30'000U + sequence;
  message.envelope.sequence = sequence;
  message.envelope.source_timestamp_ns =
      40'000U + sequence;
  message.envelope.publish_timestamp_ns =
      50'000U + sequence;
  message.envelope.source_generation = 9U;
  message.envelope.priority =
      static_cast<std::int32_t>(sequence % 32U);
  message.payload = Payload(sequence, payload_size);
  return message;
}

bool EqualMessage(
    const autoruntime::Message& left,
    const autoruntime::Message& right) {
  return left.envelope.version == right.envelope.version &&
         left.envelope.trace_id == right.envelope.trace_id &&
         left.envelope.span_id == right.envelope.span_id &&
         left.envelope.parent_span_id ==
             right.envelope.parent_span_id &&
         left.envelope.sequence == right.envelope.sequence &&
         left.envelope.source_timestamp_ns ==
             right.envelope.source_timestamp_ns &&
         left.envelope.publish_timestamp_ns ==
             right.envelope.publish_timestamp_ns &&
         left.envelope.source_generation ==
             right.envelope.source_generation &&
         left.envelope.priority == right.envelope.priority &&
         left.payload == right.payload;
}

void UpdateDigest(
    std::uint64_t& digest,
    std::span<const std::byte> payload) {
  for (std::size_t index = 0U;
       index < payload.size(); ++index) {
    const auto value =
        std::to_integer<unsigned int>(payload[index]);
    const auto transformed =
        static_cast<std::uint8_t>(
            (value +
             static_cast<unsigned int>(index) * 3U +
             11U) &
            0xffU);
    digest ^= transformed;
    digest *= 1099511628211ULL;
  }
}

std::string Hex(std::uint64_t value) {
  std::ostringstream output;
  output << "0x" << std::hex << std::setw(16)
         << std::setfill('0') << value;
  return output.str();
}

double Milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration)
      .count();
}

double Microseconds(std::chrono::nanoseconds duration) {
  return static_cast<double>(duration.count()) / 1000.0;
}

struct ExperimentResult {
  autoruntime::RecorderStats recorder;
  autoruntime::ReplayStats replay;
  std::uint64_t transport_mismatches{0U};
  std::uint64_t message_sequence_mismatches{0U};
  std::uint64_t output_digest_mismatches{0U};
  std::uint64_t original_digest{0U};
  std::uint64_t replay_digest{0U};
  std::uint64_t trace_file_bytes{0U};
  std::uint64_t first_offset_ns{0U};
  std::uint64_t last_offset_ns{0U};
  double record_wall_ms{0.0};
  double replay_wall_ms{0.0};
  autoruntime::ProcessCpuMetrics cpu_before;
  autoruntime::ProcessCpuMetrics cpu_after;
};

bool ExperimentPassed(
    const Options& options,
    const ExperimentResult& result) {
  return result.recorder.accepted_records ==
             options.message_count &&
         result.recorder.written_records ==
             options.message_count &&
         result.recorder.dropped_records == 0U &&
         result.recorder.enqueue_timeouts == 0U &&
         result.recorder.io_errors == 0U &&
         result.recorder.queue_depth == 0U &&
         result.recorder.stopped &&
         result.recorder.bytes_written ==
             result.trace_file_bytes &&
         result.replay.records_read ==
             options.message_count &&
         result.replay.records_delivered ==
             options.message_count &&
         result.replay.payload_bytes ==
             options.message_count *
                 static_cast<std::uint64_t>(
                     options.payload_size) &&
         result.replay.checksum_mismatches == 0U &&
         result.replay.sequence_mismatches == 0U &&
         result.replay.truncated_records == 0U &&
         result.replay.format_errors == 0U &&
         result.replay.timing_observations ==
             options.message_count &&
         result.transport_mismatches == 0U &&
         result.message_sequence_mismatches == 0U &&
         result.output_digest_mismatches == 0U;
}

ExperimentResult Run(const Options& options) {
  ExperimentResult result;
  TemporaryTrace trace;
  std::vector<autoruntime::Message> expected;
  expected.reserve(
      static_cast<std::size_t>(options.message_count));

  autoruntime::RecorderConfig recorder_config;
  recorder_config.path = trace.path();
  recorder_config.queue_capacity = 4096U;
  recorder_config.limits.max_payload_bytes =
      options.payload_size;
  recorder_config.overflow_policy =
      autoruntime::RecorderOverflowPolicy::Block;
  recorder_config.enqueue_timeout = 5s;
  auto recorder = Take(
      autoruntime::Recorder::Create(
          std::move(recorder_config)),
      "create recorder");

  result.cpu_before = autoruntime::SampleProcessCpu();
  const auto record_start = Clock::now();
  for (std::uint64_t sequence = 1U;
       sequence <= options.message_count; ++sequence) {
    const auto index = sequence - 1U;
    const auto target =
        record_start +
        std::chrono::microseconds(
            static_cast<std::int64_t>(
                index * options.period_us));
    std::this_thread::sleep_until(target);
    expected.push_back(
        MessageFor(sequence, options.payload_size));
    UpdateDigest(
        result.original_digest, expected.back().payload);
    RequireStatus(
        recorder->Record(
            "sensor/frame", "mock.SensorFrame",
            expected.back()),
        "record message");
  }
  RequireStatus(
      recorder->Stop(autoruntime::Deadline::After(10s)),
      "stop recorder");
  const auto record_finish = Clock::now();
  result.record_wall_ms =
      Milliseconds(record_finish - record_start);
  result.recorder = recorder->Stats();
  recorder.reset();
  result.trace_file_bytes =
      static_cast<std::uint64_t>(
          std::filesystem::file_size(trace.path()));

  auto replayer = Take(
      autoruntime::Replayer::Open(trace.path()),
      "open replayer after recorder restart boundary");
  std::size_t index = 0U;
  bool have_offset = false;
  autoruntime::ReplayOptions replay_options;
  replay_options.timing =
      autoruntime::ReplayTiming::Original;
  const auto replay_start = Clock::now();
  RequireStatus(
      replayer->Replay(
          replay_options,
          [&](const autoruntime::TraceRecord& record) {
            if (!have_offset) {
              result.first_offset_ns =
                  record.monotonic_offset_ns;
              have_offset = true;
            }
            result.last_offset_ns =
                record.monotonic_offset_ns;
            if (record.channel != "sensor/frame" ||
                record.message_type != "mock.SensorFrame" ||
                index >= expected.size() ||
                !EqualMessage(
                    record.message, expected[index])) {
              ++result.transport_mismatches;
            }
            if (record.message.envelope.sequence !=
                static_cast<std::uint64_t>(index) + 1U) {
              ++result.message_sequence_mismatches;
            }
            UpdateDigest(
                result.replay_digest,
                record.message.payload);
            ++index;
            return autoruntime::Status::Ok();
          }),
      "replay original timing");
  const auto replay_finish = Clock::now();
  result.replay_wall_ms =
      Milliseconds(replay_finish - replay_start);
  result.replay = replayer->Stats();
  if (result.original_digest != result.replay_digest) {
    result.output_digest_mismatches = 1U;
  }
  result.cpu_after = autoruntime::SampleProcessCpu();
  return result;
}

void WriteJson(
    std::ostream& output, const Options& options,
    const ExperimentResult& result) {
  const double mean_drift_us =
      result.replay.timing_observations == 0U
          ? 0.0
          : Microseconds(
                result.replay.total_timing_drift) /
                static_cast<double>(
                    result.replay.timing_observations);
  const auto recorded_span_ns =
      result.last_offset_ns >= result.first_offset_ns
          ? result.last_offset_ns - result.first_offset_ns
          : 0U;
  double user_cpu_ms = 0.0;
  double system_cpu_ms = 0.0;
  std::uint64_t voluntary_switches = 0U;
  std::uint64_t involuntary_switches = 0U;
  std::uint64_t maximum_resident_bytes = 0U;
  if (result.cpu_before.available &&
      result.cpu_after.available) {
    user_cpu_ms =
        (result.cpu_after.user_seconds -
         result.cpu_before.user_seconds) *
        1000.0;
    system_cpu_ms =
        (result.cpu_after.system_seconds -
         result.cpu_before.system_seconds) *
        1000.0;
    voluntary_switches =
        result.cpu_after.voluntary_context_switches -
        result.cpu_before.voluntary_context_switches;
    involuntary_switches =
        result.cpu_after.involuntary_context_switches -
        result.cpu_before.involuntary_context_switches;
    maximum_resident_bytes =
        result.cpu_after.maximum_resident_bytes;
  }
  const bool passed = ExperimentPassed(options, result);

  output << std::fixed << std::setprecision(3);
  output << "{\n"
         << "  \"source_revision\": \""
         << AUTORUNTIME_SOURCE_REVISION << "\",\n"
         << "  \"passed\": "
         << (passed ? "true" : "false") << ",\n"
         << "  \"message_count\": "
         << options.message_count << ",\n"
         << "  \"payload_size\": "
         << options.payload_size << ",\n"
         << "  \"record_period_us\": "
         << options.period_us << ",\n"
         << "  \"record_wall_ms\": "
         << result.record_wall_ms << ",\n"
         << "  \"recorded_span_ms\": "
         << static_cast<double>(recorded_span_ns) /
                1'000'000.0
         << ",\n"
         << "  \"trace_file_bytes\": "
         << result.trace_file_bytes << ",\n"
         << "  \"recorder\": {"
         << "\"accepted_records\":"
         << result.recorder.accepted_records
         << ",\"written_records\":"
         << result.recorder.written_records
         << ",\"dropped_records\":"
         << result.recorder.dropped_records
         << ",\"enqueue_timeouts\":"
         << result.recorder.enqueue_timeouts
         << ",\"io_errors\":"
         << result.recorder.io_errors
         << ",\"queue_high_watermark\":"
         << result.recorder.queue_high_watermark
         << ",\"bytes_written\":"
         << result.recorder.bytes_written
         << "},\n"
         << "  \"replay_wall_ms\": "
         << result.replay_wall_ms << ",\n"
         << "  \"replay\": {"
         << "\"records_read\":"
         << result.replay.records_read
         << ",\"records_delivered\":"
         << result.replay.records_delivered
         << ",\"payload_bytes\":"
         << result.replay.payload_bytes
         << ",\"checksum_mismatches\":"
         << result.replay.checksum_mismatches
         << ",\"sequence_mismatches\":"
         << result.replay.sequence_mismatches
         << ",\"truncated_records\":"
         << result.replay.truncated_records
         << ",\"format_errors\":"
         << result.replay.format_errors
         << ",\"timing_observations\":"
         << result.replay.timing_observations
         << ",\"mean_timing_drift_us\":"
         << mean_drift_us
         << ",\"max_timing_drift_us\":"
         << Microseconds(
                result.replay.maximum_timing_drift)
         << "},\n"
         << "  \"transport_payload_mismatches\": "
         << result.transport_mismatches << ",\n"
         << "  \"message_sequence_mismatches\": "
         << result.message_sequence_mismatches << ",\n"
         << "  \"output_digest_mismatches\": "
         << result.output_digest_mismatches << ",\n"
         << "  \"original_output_digest\": \""
         << Hex(result.original_digest) << "\",\n"
         << "  \"replay_output_digest\": \""
         << Hex(result.replay_digest) << "\",\n"
         << "  \"user_cpu_ms\": "
         << user_cpu_ms << ",\n"
         << "  \"system_cpu_ms\": "
         << system_cpu_ms << ",\n"
         << "  \"voluntary_context_switches\": "
         << voluntary_switches << ",\n"
         << "  \"involuntary_context_switches\": "
         << involuntary_switches << ",\n"
         << "  \"maximum_resident_bytes\": "
         << maximum_resident_bytes << "\n"
         << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    const auto result = Run(options);
    if (options.output_path.empty()) {
      WriteJson(std::cout, options, result);
    } else {
      std::ofstream output(options.output_path);
      Require(static_cast<bool>(output), "open output file");
      WriteJson(output, options, result);
      Require(static_cast<bool>(output), "write output file");
      std::cout << options.output_path << '\n';
    }
    return ExperimentPassed(options, result) ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "record_replay_experiment: "
              << error.what() << '\n';
    return 1;
  }
}
