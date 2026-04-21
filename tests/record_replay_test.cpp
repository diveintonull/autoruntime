#include "test_support.hpp"
#include <autoruntime/in_memory_transport.hpp>
#include <autoruntime/node.hpp>
#include <autoruntime/record_replay.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

class TempTrace {
 public:
  explicit TempTrace(std::string_view label) {
    static std::atomic<std::uint64_t> next{1U};
    const auto id = next.fetch_add(1U, std::memory_order_relaxed);
    path_ = (std::filesystem::temp_directory_path() /
             ("autoruntime-" + std::string(label) + "-" +
              std::to_string(autoruntime::MonotonicNanoseconds()) + "-" +
              std::to_string(id) + ".artrace"))
                .string();
  }

  ~TempTrace() {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(path_, error));
  }

  TempTrace(const TempTrace&) = delete;
  TempTrace& operator=(const TempTrace&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

template <typename T>
T Take(autoruntime::Result<T> result) {
  if (!result) {
    throw std::runtime_error(result.status().detail());
  }
  return std::move(result).take_value();
}

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

autoruntime::Message MakeMessage(
    std::uint64_t sequence, std::vector<std::byte> payload) {
  autoruntime::Message message;
  message.envelope.version = autoruntime::kMessageEnvelopeVersion;
  message.envelope.trace_id = 1000U + sequence;
  message.envelope.span_id = 2000U + sequence;
  message.envelope.parent_span_id = 3000U + sequence;
  message.envelope.sequence = sequence;
  message.envelope.source_timestamp_ns = 4000U + sequence;
  message.envelope.publish_timestamp_ns = 5000U + sequence;
  message.envelope.source_generation = 7U;
  message.envelope.priority = static_cast<std::int32_t>(sequence % 17U);
  message.payload = std::move(payload);
  return message;
}

bool EqualMessage(const autoruntime::Message& left,
                  const autoruntime::Message& right) {
  return left.envelope.version == right.envelope.version &&
         left.envelope.trace_id == right.envelope.trace_id &&
         left.envelope.span_id == right.envelope.span_id &&
         left.envelope.parent_span_id == right.envelope.parent_span_id &&
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

std::vector<std::byte> InputPayload(std::uint64_t sequence) {
  std::vector<std::byte> payload(64U);
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    payload[index] = static_cast<std::byte>(
        (sequence * 131U +
         static_cast<std::uint64_t>(index) * 17U) &
        0xffU);
  }
  return payload;
}

std::vector<std::byte> PlanningTransform(
    std::span<const std::byte> input) {
  std::vector<std::byte> output(input.size());
  for (std::size_t index = 0U; index < input.size(); ++index) {
    output[index] = static_cast<std::byte>(
        std::to_integer<unsigned int>(input[index]) ^ 0x5aU);
  }
  return output;
}

std::vector<std::byte> ControlTransform(
    std::span<const std::byte> input) {
  std::vector<std::byte> output(input.size());
  for (std::size_t index = 0U; index < input.size(); ++index) {
    const auto value = std::to_integer<unsigned int>(input[index]);
    output[index] = static_cast<std::byte>(
        (value + static_cast<unsigned int>(index) * 3U + 11U) &
        0xffU);
  }
  return output;
}

std::uint64_t PayloadDigest(
    const std::vector<autoruntime::Message>& messages) {
  std::uint64_t digest = 1469598103934665603ULL;
  for (const auto& message : messages) {
    for (const auto value : message.payload) {
      digest ^= std::to_integer<std::uint8_t>(value);
      digest *= 1099511628211ULL;
    }
  }
  return digest;
}

std::shared_ptr<autoruntime::Recorder> CreateRecorder(
    const std::string& path, std::size_t queue_capacity = 64U) {
  autoruntime::RecorderConfig config;
  config.path = path;
  config.queue_capacity = queue_capacity;
  config.limits.max_payload_bytes = 4U * 1024U * 1024U;
  config.overflow_policy =
      autoruntime::RecorderOverflowPolicy::Block;
  config.enqueue_timeout = 2s;
  return Take(autoruntime::Recorder::Create(std::move(config)));
}

int FormatRoundTripAndStepMode() {
  TempTrace trace("format");
  auto recorder = CreateRecorder(trace.path(), 8U);
  std::vector<autoruntime::Message> expected;
  for (std::uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    expected.push_back(
        MakeMessage(sequence, Bytes("payload-" + std::to_string(sequence))));
    CHECK(recorder->Record(
        "sensor/frame", "mock.SensorFrame", expected.back()));
    std::this_thread::sleep_for(2ms);
  }
  CHECK(recorder->Stop(autoruntime::Deadline::After(2s)));
  autoruntime::RecorderConfig duplicate_config;
  duplicate_config.path = trace.path();
  auto duplicate =
      autoruntime::Recorder::Create(std::move(duplicate_config));
  CHECK(!duplicate);
  CHECK(duplicate.status().code() ==
        autoruntime::StatusCode::AlreadyExists);
  const auto recorder_stats = recorder->Stats();
  CHECK(recorder_stats.accepted_records == 3U);
  CHECK(recorder_stats.written_records == 3U);
  CHECK(recorder_stats.dropped_records == 0U);
  CHECK(recorder_stats.queue_high_watermark <= 8U);

  auto replayer = Take(autoruntime::Replayer::Open(trace.path()));
  CHECK(replayer->Info().format_version ==
        autoruntime::kTraceFormatVersion);
  std::uint64_t last_offset = 0U;
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    auto next = replayer->Next();
    CHECK(next);
    CHECK(next.value().has_value());
    const auto& record = *next.value();
    CHECK(record.record_sequence == index + 1U);
    CHECK(record.monotonic_offset_ns >= last_offset);
    CHECK(record.channel == "sensor/frame");
    CHECK(record.message_type == "mock.SensorFrame");
    CHECK(record.checksum != 0U);
    CHECK(EqualMessage(record.message, expected[index]));
    last_offset = record.monotonic_offset_ns;
  }
  auto eof = replayer->Next();
  CHECK(eof);
  CHECK(!eof.value().has_value());
  CHECK(replayer->Reset());
  auto first_again = replayer->Next();
  CHECK(first_again && first_again.value().has_value());
  CHECK(EqualMessage(first_again.value()->message, expected.front()));
  return 0;
}

void WriteSingleRecord(const std::string& path) {
  auto recorder = CreateRecorder(path, 4U);
  const auto message = MakeMessage(1U, Bytes("checksum-payload"));
  const auto status =
      recorder->Record("sensor/frame", "mock.SensorFrame", message);
  if (!status) {
    throw std::runtime_error(status.detail());
  }
  const auto stop = recorder->Stop(autoruntime::Deadline::After(2s));
  if (!stop) {
    throw std::runtime_error(stop.detail());
  }
}

int CorruptionAndTruncationAreRejected() {
  TempTrace corrupted("corrupt");
  WriteSingleRecord(corrupted.path());
  {
    std::fstream file(
        corrupted.path(),
        std::ios::binary | std::ios::in | std::ios::out);
    CHECK(static_cast<bool>(file));
    file.seekg(-5, std::ios::end);
    char value = 0;
    file.read(&value, 1);
    CHECK(static_cast<bool>(file));
    value = static_cast<char>(
        static_cast<unsigned char>(value) ^ 0x80U);
    file.seekp(-5, std::ios::end);
    file.write(&value, 1);
    file.flush();
    CHECK(static_cast<bool>(file));
  }
  auto corrupt_replayer =
      Take(autoruntime::Replayer::Open(corrupted.path()));
  auto corrupt_record = corrupt_replayer->Next();
  CHECK(!corrupt_record);
  CHECK(corrupt_record.status().code() ==
        autoruntime::StatusCode::TransportError);
  CHECK(corrupt_replayer->Stats().checksum_mismatches == 1U);

  TempTrace truncated("truncated");
  WriteSingleRecord(truncated.path());
  const auto original_size =
      std::filesystem::file_size(truncated.path());
  CHECK(original_size > 2U);
  std::filesystem::resize_file(
      truncated.path(), original_size - 2U);
  auto truncated_replayer =
      Take(autoruntime::Replayer::Open(truncated.path()));
  auto truncated_record = truncated_replayer->Next();
  CHECK(!truncated_record);
  CHECK(truncated_record.status().code() ==
        autoruntime::StatusCode::TransportError);
  CHECK(truncated_replayer->Stats().truncated_records == 1U);
  return 0;
}

int RecordingTransportFailurePolicyIsExplicit() {
  TempTrace required_trace("required");
  auto required_recorder = CreateRecorder(required_trace.path(), 4U);
  CHECK(required_recorder->Stop(autoruntime::Deadline::After(2s)));
  auto required_inner =
      std::make_shared<autoruntime::InMemoryTransport>();
  autoruntime::RecordingTransportConfig required_config;
  required_config.transport = required_inner;
  required_config.recorder = required_recorder;
  required_config.default_message_type = "mock.Raw";
  required_config.failure_policy =
      autoruntime::RecordingFailurePolicy::RequireRecord;
  auto required = std::make_shared<autoruntime::RecordingTransport>(
      std::move(required_config));
  auto required_status = required->Publish(
      "sensor/frame", MakeMessage(1U, Bytes("required")),
      autoruntime::QosProfile{});
  CHECK(!required_status);
  CHECK(required_inner->Stats().published_messages == 0U);
  CHECK(required->CaptureStats().record_failures == 1U);
  CHECK(required->Close());

  TempTrace continue_trace("continue");
  auto continue_recorder = CreateRecorder(continue_trace.path(), 4U);
  CHECK(continue_recorder->Stop(autoruntime::Deadline::After(2s)));
  auto continue_inner =
      std::make_shared<autoruntime::InMemoryTransport>();
  autoruntime::RecordingTransportConfig continue_config;
  continue_config.transport = continue_inner;
  continue_config.recorder = continue_recorder;
  continue_config.default_message_type = "mock.Raw";
  continue_config.failure_policy =
      autoruntime::RecordingFailurePolicy::ContinueWithoutRecord;
  auto continued = std::make_shared<autoruntime::RecordingTransport>(
      std::move(continue_config));
  CHECK(continued->Publish(
      "sensor/frame", MakeMessage(1U, Bytes("continue")),
      autoruntime::QosProfile{}));
  CHECK(continue_inner->Stats().published_messages == 1U);
  CHECK(continued->CaptureStats().record_failures == 1U);
  CHECK(continued->Close());
  return 0;
}

int OriginalAcceleratedAndStepTimingWork() {
  TempTrace trace("timing");
  auto recorder = CreateRecorder(trace.path(), 8U);
  for (std::uint64_t sequence = 1U; sequence <= 3U; ++sequence) {
    CHECK(recorder->Record(
        "sensor/timing", "mock.Timing",
        MakeMessage(sequence, Bytes("tick"))));
    if (sequence != 3U) {
      std::this_thread::sleep_for(15ms);
    }
  }
  CHECK(recorder->Stop(autoruntime::Deadline::After(2s)));

  auto replayer = Take(autoruntime::Replayer::Open(trace.path()));
  std::vector<Clock::time_point> delivered_at;
  autoruntime::ReplayOptions original;
  original.timing = autoruntime::ReplayTiming::Original;
  CHECK(replayer->Replay(
      original,
      [&](const autoruntime::TraceRecord&) {
        delivered_at.push_back(Clock::now());
        return autoruntime::Status::Ok();
      }));
  CHECK(delivered_at.size() == 3U);
  CHECK(delivered_at[1] - delivered_at[0] >= 8ms);
  CHECK(delivered_at[2] - delivered_at[1] >= 8ms);
  CHECK(replayer->Stats().timing_observations == 3U);

  CHECK(replayer->Reset());
  autoruntime::ReplayOptions accelerated;
  accelerated.timing = autoruntime::ReplayTiming::Accelerated;
  accelerated.speed = 10.0;
  std::uint64_t accelerated_count = 0U;
  CHECK(replayer->Replay(
      accelerated,
      [&](const autoruntime::TraceRecord&) {
        ++accelerated_count;
        return autoruntime::Status::Ok();
      }));
  CHECK(accelerated_count == 3U);
  return 0;
}

int RestartReplay5000IsBitExactAndDeterministic() {
  constexpr std::uint64_t message_count = 5000U;
  TempTrace trace("restart-5000");
  std::vector<autoruntime::Message> sensor_messages;
  std::vector<autoruntime::Message> planning_messages;
  std::vector<autoruntime::Message> control_messages;
  sensor_messages.reserve(message_count);
  planning_messages.reserve(message_count);
  control_messages.reserve(message_count);
  std::mutex captured_mutex;
  std::atomic<std::uint64_t> final_count{0U};
  std::atomic<std::uint64_t> publish_failures{0U};
  auto recorder = CreateRecorder(trace.path(), 16384U);

  {
    auto inner = std::make_shared<autoruntime::InMemoryTransport>();
    autoruntime::RecordingTransportConfig recording_config;
    recording_config.transport = inner;
    recording_config.recorder = recorder;
    recording_config.failure_policy =
        autoruntime::RecordingFailurePolicy::RequireRecord;
    recording_config.message_type_resolver =
        [](std::string_view topic) {
          if (topic == "sensor/frame") return std::string("mock.SensorFrame");
          if (topic == "planning/trajectory") {
            return std::string("mock.Trajectory");
          }
          return std::string("mock.ControlCommand");
        };
    auto transport =
        std::make_shared<autoruntime::RecordingTransport>(
            std::move(recording_config));
    auto executor = std::make_shared<autoruntime::Executor>();
    auto planning_group = executor->CreateCallbackGroup(
        autoruntime::CallbackGroupConfig{"planning", 1U, 32768U});
    auto control_group = executor->CreateCallbackGroup(
        autoruntime::CallbackGroupConfig{"control", 1U, 32768U});
    auto monitor_group = executor->CreateCallbackGroup(
        autoruntime::CallbackGroupConfig{"monitor", 1U, 32768U});
    CHECK(planning_group && control_group && monitor_group);

    autoruntime::Node sensor({"sensor", 1U}, executor, transport);
    autoruntime::Node planning({"planning", 1U}, executor, transport);
    autoruntime::Node control({"control", 1U}, executor, transport);
    autoruntime::Node monitor({"monitor", 1U}, executor, transport);
    auto sensor_publisher = sensor.CreatePublisher("sensor/frame");
    auto planning_publisher =
        planning.CreatePublisher("planning/trajectory");
    auto control_publisher =
        control.CreatePublisher("control/command");
    CHECK(sensor_publisher && planning_publisher && control_publisher);

    auto planning_output = planning_publisher.value();
    auto sensor_subscription = planning.CreateSubscriber(
        "sensor/frame",
        autoruntime::SubscriptionOptions{
            planning_group.value(), 8192U,
            autoruntime::OverflowPolicy::DropNewest},
        [&](const autoruntime::Message& message) mutable {
          {
            std::lock_guard lock(captured_mutex);
            sensor_messages.push_back(message);
          }
          const auto payload = PlanningTransform(message.payload);
          if (!planning_output.Publish(
                  payload, message.ContinueTrace())) {
            publish_failures.fetch_add(1U, std::memory_order_relaxed);
          }
        });
    CHECK(sensor_subscription);

    auto control_output = control_publisher.value();
    auto planning_subscription = control.CreateSubscriber(
        "planning/trajectory",
        autoruntime::SubscriptionOptions{
            control_group.value(), 8192U,
            autoruntime::OverflowPolicy::DropNewest},
        [&](const autoruntime::Message& message) mutable {
          {
            std::lock_guard lock(captured_mutex);
            planning_messages.push_back(message);
          }
          const auto payload = ControlTransform(message.payload);
          if (!control_output.Publish(
                  payload, message.ContinueTrace())) {
            publish_failures.fetch_add(1U, std::memory_order_relaxed);
          }
        });
    CHECK(planning_subscription);

    auto control_subscription = monitor.CreateSubscriber(
        "control/command",
        autoruntime::SubscriptionOptions{
            monitor_group.value(), 8192U,
            autoruntime::OverflowPolicy::DropNewest},
        [&](const autoruntime::Message& message) {
          {
            std::lock_guard lock(captured_mutex);
            control_messages.push_back(message);
          }
          final_count.fetch_add(1U, std::memory_order_release);
        });
    CHECK(control_subscription);
    CHECK(executor->Start());

    for (std::uint64_t sequence = 1U;
         sequence <= message_count; ++sequence) {
      CHECK(sensor_publisher.value().Publish(InputPayload(sequence)));
    }
    CHECK(WaitUntil(
        [&] {
          return final_count.load(std::memory_order_acquire) ==
                 message_count;
        },
        10s));
    CHECK(publish_failures.load(std::memory_order_relaxed) == 0U);
    CHECK(sensor_subscription.value().Close());
    CHECK(planning_subscription.value().Close());
    CHECK(control_subscription.value().Close());
    CHECK(executor->Stop(autoruntime::Deadline::After(2s)));
    CHECK(transport->Close());
  }

  const auto recorded = recorder->Stats();
  CHECK(recorded.accepted_records == message_count * 3U);
  CHECK(recorded.written_records == message_count * 3U);
  CHECK(recorded.dropped_records == 0U);
  CHECK(recorded.enqueue_timeouts == 0U);
  {
    std::lock_guard lock(captured_mutex);
    CHECK(sensor_messages.size() == message_count);
    CHECK(planning_messages.size() == message_count);
    CHECK(control_messages.size() == message_count);
  }
  const auto original_digest = PayloadDigest(control_messages);

  auto replayer = Take(autoruntime::Replayer::Open(trace.path()));
  auto replay_transport =
      std::make_shared<autoruntime::InMemoryTransport>();
  auto replay_executor = std::make_shared<autoruntime::Executor>();
  auto planning_group = replay_executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"replay-planning", 1U, 32768U});
  auto control_group = replay_executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"replay-control", 1U, 32768U});
  auto monitor_group = replay_executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"replay-monitor", 1U, 32768U});
  CHECK(planning_group && control_group && monitor_group);

  autoruntime::Node planning(
      {"planning", 1U}, replay_executor, replay_transport);
  autoruntime::Node control(
      {"control", 1U}, replay_executor, replay_transport);
  autoruntime::Node monitor(
      {"monitor", 1U}, replay_executor, replay_transport);
  auto planning_publisher =
      planning.CreatePublisher("planning/trajectory");
  auto control_publisher =
      control.CreatePublisher("control/command");
  CHECK(planning_publisher && control_publisher);

  auto planning_output = planning_publisher.value();
  auto sensor_subscription = planning.CreateSubscriber(
      "sensor/frame",
      autoruntime::SubscriptionOptions{
          planning_group.value(), 8192U,
          autoruntime::OverflowPolicy::DropNewest},
      [&, planning_output](const autoruntime::Message& message) mutable {
        const auto payload = PlanningTransform(message.payload);
        if (!planning_output.Publish(payload, message.ContinueTrace())) {
          publish_failures.fetch_add(1U, std::memory_order_relaxed);
        }
      });
  CHECK(sensor_subscription);

  auto control_output = control_publisher.value();
  auto planning_subscription = control.CreateSubscriber(
      "planning/trajectory",
      autoruntime::SubscriptionOptions{
          control_group.value(), 8192U,
          autoruntime::OverflowPolicy::DropNewest},
      [&, control_output](const autoruntime::Message& message) mutable {
        const auto payload = ControlTransform(message.payload);
        if (!control_output.Publish(payload, message.ContinueTrace())) {
          publish_failures.fetch_add(1U, std::memory_order_relaxed);
        }
      });
  CHECK(planning_subscription);

  std::vector<autoruntime::Message> replay_control_messages;
  replay_control_messages.reserve(message_count);
  std::mutex replay_mutex;
  std::atomic<std::uint64_t> replay_final_count{0U};
  auto control_subscription = monitor.CreateSubscriber(
      "control/command",
      autoruntime::SubscriptionOptions{
          monitor_group.value(), 8192U,
          autoruntime::OverflowPolicy::DropNewest},
      [&](const autoruntime::Message& message) {
        {
          std::lock_guard lock(replay_mutex);
          replay_control_messages.push_back(message);
        }
        replay_final_count.fetch_add(1U, std::memory_order_release);
      });
  CHECK(control_subscription);
  CHECK(replay_executor->Start());

  std::size_t sensor_index = 0U;
  std::size_t planning_index = 0U;
  std::size_t control_index = 0U;
  std::uint64_t transport_mismatches = 0U;
  autoruntime::ReplayOptions options;
  options.timing = autoruntime::ReplayTiming::Original;
  CHECK(replayer->Replay(
      options,
      [&](const autoruntime::TraceRecord& record) {
        const autoruntime::Message* expected = nullptr;
        std::string_view expected_type;
        if (record.channel == "sensor/frame") {
          expected_type = "mock.SensorFrame";
          if (sensor_index < sensor_messages.size()) {
            expected = &sensor_messages[sensor_index++];
          }
        } else if (record.channel == "planning/trajectory") {
          expected_type = "mock.Trajectory";
          if (planning_index < planning_messages.size()) {
            expected = &planning_messages[planning_index++];
          }
        } else if (record.channel == "control/command") {
          expected_type = "mock.ControlCommand";
          if (control_index < control_messages.size()) {
            expected = &control_messages[control_index++];
          }
        }
        if (expected == nullptr ||
            record.message_type != expected_type ||
            !EqualMessage(record.message, *expected)) {
          ++transport_mismatches;
          return autoruntime::Status(
              autoruntime::StatusCode::Internal,
              "transport replay mismatch");
        }
        if (record.channel == "sensor/frame") {
          return replay_transport->Publish(
              record.channel, record.message,
              autoruntime::QosProfile{});
        }
        return autoruntime::Status::Ok();
      }));
  CHECK(transport_mismatches == 0U);
  CHECK(sensor_index == message_count);
  CHECK(planning_index == message_count);
  CHECK(control_index == message_count);
  CHECK(WaitUntil(
      [&] {
        return replay_final_count.load(std::memory_order_acquire) ==
               message_count;
      },
      10s));
  CHECK(publish_failures.load(std::memory_order_relaxed) == 0U);
  CHECK(sensor_subscription.value().Close());
  CHECK(planning_subscription.value().Close());
  CHECK(control_subscription.value().Close());
  CHECK(replay_executor->Stop(autoruntime::Deadline::After(2s)));
  CHECK(replay_transport->Close());

  {
    std::lock_guard lock(replay_mutex);
    CHECK(replay_control_messages.size() == control_messages.size());
    for (std::size_t index = 0U;
         index < replay_control_messages.size(); ++index) {
      CHECK(replay_control_messages[index].envelope.sequence ==
            control_messages[index].envelope.sequence);
      CHECK(replay_control_messages[index].payload ==
            control_messages[index].payload);
    }
  }
  CHECK(PayloadDigest(replay_control_messages) == original_digest);
  const auto replay_stats = replayer->Stats();
  CHECK(replay_stats.records_read == message_count * 3U);
  CHECK(replay_stats.records_delivered == message_count * 3U);
  CHECK(replay_stats.checksum_mismatches == 0U);
  CHECK(replay_stats.sequence_mismatches == 0U);
  CHECK(replay_stats.truncated_records == 0U);
  CHECK(replay_stats.timing_observations ==
        message_count * 3U);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--case") {
    const std::string_view selected(argv[2]);
    if (selected == "format_round_trip") {
      return FormatRoundTripAndStepMode();
    }
    if (selected == "corruption_detection") {
      return CorruptionAndTruncationAreRejected();
    }
    if (selected == "recording_transport_failure") {
      return RecordingTransportFailurePolicyIsExplicit();
    }
    if (selected == "timing_modes") {
      return OriginalAcceleratedAndStepTimingWork();
    }
    if (selected == "restart_replay_5000") {
      return RestartReplay5000IsBitExactAndDeterministic();
    }
    return 2;
  }
  if (argc != 1) {
    return 2;
  }
  if (FormatRoundTripAndStepMode() != 0) return 1;
  if (CorruptionAndTruncationAreRejected() != 0) return 1;
  if (RecordingTransportFailurePolicyIsExplicit() != 0) return 1;
  if (OriginalAcceleratedAndStepTimingWork() != 0) return 1;
  return RestartReplay5000IsBitExactAndDeterministic();
}
