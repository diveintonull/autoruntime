#pragma once

#include <autoruntime/deadline.hpp>
#include <autoruntime/message.hpp>
#include <autoruntime/status.hpp>
#include <autoruntime/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace autoruntime {

inline constexpr std::uint16_t kTraceFormatVersion = 1U;

enum class RecorderOverflowPolicy {
  Block,
  DropNewest,
};

struct TraceLimits {
  std::size_t max_channel_bytes{1024U};
  std::size_t max_message_type_bytes{1024U};
  std::size_t max_payload_bytes{64U * 1024U * 1024U};
};

struct RecorderConfig {
  std::string path;
  std::size_t queue_capacity{1024U};
  TraceLimits limits;
  RecorderOverflowPolicy overflow_policy{
      RecorderOverflowPolicy::Block};
  std::chrono::nanoseconds enqueue_timeout{
      std::chrono::milliseconds(100)};
  bool synchronize_on_stop{true};
};

struct RecorderStats {
  std::uint64_t accepted_records{0U};
  std::uint64_t written_records{0U};
  std::uint64_t dropped_records{0U};
  std::uint64_t enqueue_timeouts{0U};
  std::uint64_t io_errors{0U};
  std::uint64_t bytes_written{0U};
  std::size_t queue_depth{0U};
  std::size_t queue_high_watermark{0U};
  bool stopping{false};
  bool stopped{false};
};

struct TraceFileInfo {
  std::uint16_t format_version{0U};
  std::uint64_t wall_clock_start_ns{0U};
  std::uint64_t monotonic_start_ns{0U};
};

struct TraceRecord {
  std::uint64_t record_sequence{0U};
  std::uint64_t monotonic_offset_ns{0U};
  std::string channel;
  std::string message_type;
  Message message;
  std::uint32_t checksum{0U};
};

class Recorder {
 public:
  [[nodiscard]] static Result<std::shared_ptr<Recorder>> Create(
      RecorderConfig config);
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  Recorder(Recorder&&) = delete;
  Recorder& operator=(Recorder&&) = delete;

  Status Record(std::string_view channel,
                std::string_view message_type,
                const Message& message);
  Status Stop(Deadline deadline = Deadline::Infinite());
  [[nodiscard]] RecorderStats Stats() const;

 private:
  struct Impl;
  explicit Recorder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

enum class RecordingFailurePolicy {
  RequireRecord,
  ContinueWithoutRecord,
};

using MessageTypeResolver =
    std::function<std::string(std::string_view)>;

struct RecordingTransportConfig {
  std::shared_ptr<Transport> transport;
  std::shared_ptr<Recorder> recorder;
  std::string default_message_type{"opaque/bytes"};
  MessageTypeResolver message_type_resolver;
  RecordingFailurePolicy failure_policy{
      RecordingFailurePolicy::RequireRecord};
  bool stop_recorder_on_close{true};
};

struct RecordingTransportStats {
  std::uint64_t record_failures{0U};
  Status last_record_status;
};

class RecordingTransport final : public Transport {
 public:
  explicit RecordingTransport(RecordingTransportConfig config);
  ~RecordingTransport() override;

  RecordingTransport(const RecordingTransport&) = delete;
  RecordingTransport& operator=(const RecordingTransport&) = delete;

  Status Publish(std::string_view topic, Message message,
                 const QosProfile& qos) override;
  [[nodiscard]] Result<SubscriptionId> Subscribe(
      std::string_view topic, const QosProfile& qos,
      TransportMessageCallback callback) override;
  Status Unsubscribe(SubscriptionId subscription_id) override;

  [[nodiscard]] Result<ServiceId> AdvertiseService(
      std::string_view service_name,
      TransportServiceCallback callback) override;
  Status RemoveService(ServiceId service_id) override;
  [[nodiscard]] Result<Message> Request(
      std::string_view service_name, Message request,
      Deadline deadline) override;

  [[nodiscard]] TransportStats Stats() const override;
  [[nodiscard]] RecordingTransportStats CaptureStats() const;
  Status Close() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class ReplayTiming {
  Original,
  Accelerated,
  AsFastAsPossible,
};

struct ReplayOptions {
  ReplayTiming timing{ReplayTiming::Original};
  double speed{1.0};
};

struct ReplayStats {
  std::uint64_t records_read{0U};
  std::uint64_t records_delivered{0U};
  std::uint64_t payload_bytes{0U};
  std::uint64_t checksum_mismatches{0U};
  std::uint64_t sequence_mismatches{0U};
  std::uint64_t truncated_records{0U};
  std::uint64_t format_errors{0U};
  std::uint64_t timing_observations{0U};
  std::chrono::nanoseconds total_timing_drift{0};
  std::chrono::nanoseconds maximum_timing_drift{0};
};

using ReplayCallback =
    std::function<Status(const TraceRecord&)>;

class Replayer {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Replayer>> Open(
      std::string path, TraceLimits limits = {});
  ~Replayer();

  Replayer(const Replayer&) = delete;
  Replayer& operator=(const Replayer&) = delete;
  Replayer(Replayer&&) = delete;
  Replayer& operator=(Replayer&&) = delete;

  [[nodiscard]] const TraceFileInfo& Info() const noexcept;
  [[nodiscard]] Result<std::optional<TraceRecord>> Next();
  Status Reset();
  Status Replay(const ReplayOptions& options,
                ReplayCallback callback,
                std::stop_token stop_token = {});
  [[nodiscard]] ReplayStats Stats() const;

 private:
  struct Impl;
  explicit Replayer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime
