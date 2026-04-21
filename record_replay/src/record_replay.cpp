#include <autoruntime/record_replay.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace autoruntime {
namespace {

using Bytes = std::vector<std::byte>;
using Clock = Deadline::Clock;

constexpr std::array<std::byte, 8U> kFileMagic{
    std::byte{'A'}, std::byte{'R'}, std::byte{'T'}, std::byte{'R'},
    std::byte{'A'}, std::byte{'C'}, std::byte{'E'}, std::byte{'1'}};
constexpr std::uint32_t kRecordMagic = 0x31524341U;
constexpr std::uint16_t kRecordVersion = 1U;
constexpr std::size_t kFileHeaderBytes = 40U;
constexpr std::size_t kRecordFixedBytes = 112U;
constexpr std::size_t kChecksumBytes = 4U;
constexpr std::size_t kMinimumRecordBytes =
    kRecordFixedBytes + kChecksumBytes;

[[nodiscard]] std::uint64_t WallClockNanoseconds() noexcept {
  const auto now =
      std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now)
          .count());
}

void AppendU16(Bytes& output, std::uint16_t value) {
  for (unsigned int shift = 0U; shift < 16U; shift += 8U) {
    output.push_back(static_cast<std::byte>(
        (static_cast<std::uint32_t>(value) >> shift) &
            0xffU));
  }
}

void AppendU32(Bytes& output, std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    output.push_back(static_cast<std::byte>(
        (value >> shift) & 0xffU));
  }
}

void AppendU64(Bytes& output, std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    output.push_back(static_cast<std::byte>(
        (value >> shift) & 0xffU));
  }
}

void AppendString(Bytes& output, std::string_view value) {
  const auto* begin =
      reinterpret_cast<const std::byte*>(value.data());
  output.insert(output.end(), begin, begin + value.size());
}

[[nodiscard]] bool ReadU16(
    std::span<const std::byte> input, std::size_t& offset,
    std::uint16_t& value) {
  if (input.size() - std::min(input.size(), offset) < 2U) {
    return false;
  }
  value = 0U;
  for (unsigned int index = 0U; index < 2U; ++index) {
    value |= static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(input[offset + index])
        << (index * 8U));
  }
  offset += 2U;
  return true;
}

[[nodiscard]] bool ReadU32(
    std::span<const std::byte> input, std::size_t& offset,
    std::uint32_t& value) {
  if (input.size() - std::min(input.size(), offset) < 4U) {
    return false;
  }
  value = 0U;
  for (unsigned int index = 0U; index < 4U; ++index) {
    value |= std::to_integer<std::uint32_t>(
                 input[offset + index])
             << (index * 8U);
  }
  offset += 4U;
  return true;
}

[[nodiscard]] bool ReadU64(
    std::span<const std::byte> input, std::size_t& offset,
    std::uint64_t& value) {
  if (input.size() - std::min(input.size(), offset) < 8U) {
    return false;
  }
  value = 0U;
  for (unsigned int index = 0U; index < 8U; ++index) {
    value |= std::to_integer<std::uint64_t>(
                 input[offset + index])
             << (index * 8U);
  }
  offset += 8U;
  return true;
}

[[nodiscard]] std::uint32_t Crc32(
    std::span<const std::byte> input) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const auto value : input) {
    crc ^= std::to_integer<std::uint32_t>(value);
    for (unsigned int bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask =
          0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

[[nodiscard]] bool CheckedAdd(
    std::size_t left, std::size_t right,
    std::size_t& result) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] Bytes BuildFileHeader(
    std::uint64_t wall_clock_start_ns,
    std::uint64_t monotonic_start_ns) {
  Bytes header;
  header.reserve(kFileHeaderBytes);
  header.insert(header.end(), kFileMagic.begin(), kFileMagic.end());
  AppendU16(header, kTraceFormatVersion);
  AppendU16(header, static_cast<std::uint16_t>(kFileHeaderBytes));
  AppendU64(header, wall_clock_start_ns);
  AppendU64(header, monotonic_start_ns);
  AppendU64(header, 0U);
  AppendU32(header, Crc32(header));
  return header;
}

struct PendingRecord {
  std::uint64_t record_sequence{0U};
  std::uint64_t monotonic_offset_ns{0U};
  std::string channel;
  std::string message_type;
  Message message;
};

[[nodiscard]] Bytes SerializeRecord(
    const PendingRecord& record) {
  std::size_t total = kMinimumRecordBytes;
  if (!CheckedAdd(total, record.channel.size(), total) ||
      !CheckedAdd(total, record.message_type.size(), total) ||
      !CheckedAdd(total, record.message.payload.size(), total)) {
    throw std::length_error("trace record size overflow");
  }

  Bytes output;
  output.reserve(total);
  AppendU32(output, kRecordMagic);
  AppendU16(output, kRecordVersion);
  AppendU16(output, 0U);
  AppendU64(output, static_cast<std::uint64_t>(total));
  AppendU64(output, record.record_sequence);
  AppendU64(output, record.monotonic_offset_ns);
  AppendU16(output, record.message.envelope.version);
  AppendU16(output, 0U);
  AppendU32(
      output,
      std::bit_cast<std::uint32_t>(
          record.message.envelope.priority));
  AppendU64(output, record.message.envelope.trace_id);
  AppendU64(output, record.message.envelope.span_id);
  AppendU64(output, record.message.envelope.parent_span_id);
  AppendU64(output, record.message.envelope.sequence);
  AppendU64(output, record.message.envelope.source_timestamp_ns);
  AppendU64(output, record.message.envelope.publish_timestamp_ns);
  AppendU64(output, record.message.envelope.source_generation);
  AppendU32(
      output,
      static_cast<std::uint32_t>(record.channel.size()));
  AppendU32(
      output,
      static_cast<std::uint32_t>(record.message_type.size()));
  AppendU64(
      output,
      static_cast<std::uint64_t>(record.message.payload.size()));
  AppendString(output, record.channel);
  AppendString(output, record.message_type);
  output.insert(
      output.end(), record.message.payload.begin(),
      record.message.payload.end());
  AppendU32(output, Crc32(output));
  if (output.size() != total) {
    throw std::logic_error("serialized trace record size mismatch");
  }
  return output;
}

[[nodiscard]] Status ValidateLimits(
    const TraceLimits& limits) {
  if (limits.max_channel_bytes == 0U ||
      limits.max_message_type_bytes == 0U ||
      limits.max_payload_bytes == 0U ||
      limits.max_channel_bytes >
          std::numeric_limits<std::uint32_t>::max() ||
      limits.max_message_type_bytes >
          std::numeric_limits<std::uint32_t>::max()) {
    return Status(
        StatusCode::InvalidArgument,
        "trace limits must be positive and encodable");
  }
  std::size_t maximum = kMinimumRecordBytes;
  if (!CheckedAdd(maximum, limits.max_channel_bytes, maximum) ||
      !CheckedAdd(
          maximum, limits.max_message_type_bytes, maximum) ||
      !CheckedAdd(maximum, limits.max_payload_bytes, maximum) ||
      maximum >
          static_cast<std::size_t>(
              std::numeric_limits<std::streamsize>::max())) {
    return Status(
        StatusCode::InvalidArgument,
        "trace limits exceed stream or size_t bounds");
  }
  return Status::Ok();
}

enum class StreamRead {
  Ok,
  Eof,
  Truncated,
};

[[nodiscard]] StreamRead ReadExact(
    std::ifstream& input, std::span<std::byte> destination,
    bool allow_clean_eof) {
  if (destination.empty()) {
    return StreamRead::Ok;
  }
  input.read(
      reinterpret_cast<char*>(destination.data()),
      static_cast<std::streamsize>(destination.size()));
  const auto count = input.gcount();
  if (count == static_cast<std::streamsize>(destination.size())) {
    return StreamRead::Ok;
  }
  if (allow_clean_eof && count == 0 && input.eof()) {
    return StreamRead::Eof;
  }
  return StreamRead::Truncated;
}

[[nodiscard]] Status TraceError(std::string detail) {
  return Status(StatusCode::TransportError, std::move(detail));
}

[[nodiscard]] Status WriteAll(
    int descriptor, std::span<const std::byte> bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto written = ::write(
        descriptor, bytes.data() + offset,
        bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status(
          StatusCode::TransportError,
          std::string("write trace failed: ") +
              std::strerror(errno));
    }
    if (written == 0) {
      return Status(
          StatusCode::TransportError,
          "write trace made no progress");
    }
    offset += static_cast<std::size_t>(written);
  }
  return Status::Ok();
}

}  // namespace

struct Recorder::Impl {
  explicit Impl(RecorderConfig configured)
      : config(std::move(configured)),
        wall_clock_start_ns(WallClockNanoseconds()),
        monotonic_start_ns(MonotonicNanoseconds()) {
    const int descriptor = ::open(
        config.path.c_str(),
        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      throw std::system_error(
          errno, std::generic_category(),
          "create trace output");
    }
    const auto header =
        BuildFileHeader(wall_clock_start_ns, monotonic_start_ns);
    const auto write_status = WriteAll(descriptor, header);
    if (!write_status) {
      const int saved_error = errno;
      static_cast<void>(::close(descriptor));
      static_cast<void>(::unlink(config.path.c_str()));
      throw std::system_error(
          saved_error == 0 ? EIO : saved_error,
          std::generic_category(),
          write_status.detail());
    }
    output_descriptor = descriptor;
    stats.bytes_written =
        static_cast<std::uint64_t>(header.size());
  }

  ~Impl() {
    if (output_descriptor >= 0) {
      static_cast<void>(::close(output_descriptor));
    }
  }

  void StartWriter() {
    {
      std::lock_guard lock(mutex);
      writer_finished = false;
    }
    try {
      writer = std::thread([this] { WriterLoop(); });
    } catch (...) {
      {
        std::lock_guard lock(mutex);
        writer_finished = true;
        stopping = true;
        stats.stopping = true;
        stats.stopped = true;
      }
      if (output_descriptor >= 0) {
        static_cast<void>(::close(output_descriptor));
        output_descriptor = -1;
      }
      static_cast<void>(::unlink(config.path.c_str()));
      throw;
    }
  }

  void FailWriter(std::string detail) noexcept {
    std::lock_guard lock(mutex);
    ++stats.io_errors;
    stats.dropped_records +=
        static_cast<std::uint64_t>(queue.size()) + 1U;
    queue.clear();
    stats.queue_depth = 0U;
    writer_status =
        Status(StatusCode::TransportError, std::move(detail));
    stopping = true;
    stats.stopping = true;
    not_full.notify_all();
  }

  void WriterLoop() noexcept {
    for (;;) {
      PendingRecord record;
      {
        std::unique_lock lock(mutex);
        not_empty.wait(lock, [&] {
          return stopping || !queue.empty();
        });
        if (queue.empty()) {
          if (stopping) {
            break;
          }
          continue;
        }
        record = std::move(queue.front());
        queue.pop_front();
        stats.queue_depth = queue.size();
        not_full.notify_all();
      }

      try {
        const auto serialized = SerializeRecord(record);
        const auto write_status =
            WriteAll(output_descriptor, serialized);
        if (!write_status) {
          FailWriter(write_status.detail());
          break;
        }
        std::lock_guard lock(mutex);
        ++stats.written_records;
        stats.bytes_written +=
            static_cast<std::uint64_t>(serialized.size());
      } catch (const std::exception& error) {
        FailWriter(
            std::string("serialize trace record failed: ") +
            error.what());
        break;
      } catch (...) {
        FailWriter("serialize trace record failed");
        break;
      }
    }

    int synchronization_error = 0;
    if (config.synchronize_on_stop &&
        output_descriptor >= 0 &&
        ::fsync(output_descriptor) != 0) {
      synchronization_error = errno;
    }
    int close_error = 0;
    if (output_descriptor >= 0 &&
        ::close(output_descriptor) != 0) {
      close_error = errno;
    }
    output_descriptor = -1;
    {
      std::lock_guard lock(mutex);
      if (writer_status &&
          (synchronization_error != 0 || close_error != 0)) {
        ++stats.io_errors;
        const int error =
            synchronization_error != 0
                ? synchronization_error
                : close_error;
        writer_status = Status(
            StatusCode::TransportError,
            std::string("synchronize trace output failed: ") +
                std::strerror(error));
      }
      writer_finished = true;
      stats.stopped = true;
      stats.stopping = true;
    }
    finished.notify_all();
    not_full.notify_all();
  }

  RecorderConfig config;
  std::uint64_t wall_clock_start_ns{0U};
  std::uint64_t monotonic_start_ns{0U};
  mutable std::mutex mutex;
  std::mutex join_mutex;
  std::condition_variable not_empty;
  std::condition_variable not_full;
  std::condition_variable finished;
  std::deque<PendingRecord> queue;
  int output_descriptor{-1};
  std::thread writer;
  std::uint64_t next_record_sequence{1U};
  std::uint64_t last_record_offset_ns{0U};
  bool stopping{false};
  bool writer_finished{true};
  Status writer_status;
  RecorderStats stats;
};

Result<std::shared_ptr<Recorder>> Recorder::Create(
    RecorderConfig config) {
  if (config.path.empty() || config.queue_capacity == 0U) {
    return Status(
        StatusCode::InvalidArgument,
        "recorder requires a path and nonzero queue capacity");
  }
  const auto limits_status = ValidateLimits(config.limits);
  if (!limits_status) {
    return limits_status;
  }
  if (config.overflow_policy == RecorderOverflowPolicy::Block &&
      config.enqueue_timeout <=
          std::chrono::nanoseconds::zero()) {
    return Status(
        StatusCode::InvalidArgument,
        "blocking recorder requires a positive enqueue timeout");
  }

  try {
    auto recorder = std::shared_ptr<Recorder>(
        new Recorder(std::make_unique<Impl>(std::move(config))));
    recorder->impl_->StartWriter();
    return recorder;
  } catch (const std::system_error& error) {
    if (error.code().value() == EEXIST) {
      return Status(
          StatusCode::AlreadyExists,
          "trace output already exists");
    }
    return Status(
        StatusCode::TransportError, error.what());
  } catch (const std::exception& error) {
    return Status(
        StatusCode::TransportError,
        std::string("create recorder failed: ") + error.what());
  }
}

Recorder::Recorder(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

Recorder::~Recorder() {
  if (impl_) {
    static_cast<void>(Stop(Deadline::Infinite()));
  }
}

Status Recorder::Record(
    std::string_view channel, std::string_view message_type,
    const Message& message) {
  if (channel.empty() || message_type.empty()) {
    return Status(
        StatusCode::InvalidArgument,
        "trace record requires channel and message type");
  }
  if (channel.size() > impl_->config.limits.max_channel_bytes ||
      message_type.size() >
          impl_->config.limits.max_message_type_bytes ||
      message.payload.size() >
          impl_->config.limits.max_payload_bytes) {
    return Status(
        StatusCode::InvalidArgument,
        "trace record exceeds configured limits");
  }

  PendingRecord record;
  try {
    record.channel = std::string(channel);
    record.message_type = std::string(message_type);
    record.message = message;
  } catch (const std::exception& error) {
    return Status(
        StatusCode::Internal,
        std::string("copy trace record failed: ") + error.what());
  }

  const auto capture_now = MonotonicNanoseconds();
  std::unique_lock lock(impl_->mutex);
  if (impl_->stopping) {
    return Status(StatusCode::Closed, "recorder is stopping");
  }
  if (!impl_->writer_status) {
    return impl_->writer_status;
  }

  if (impl_->queue.size() >= impl_->config.queue_capacity) {
    if (impl_->config.overflow_policy ==
        RecorderOverflowPolicy::DropNewest) {
      ++impl_->stats.dropped_records;
      return Status(
          StatusCode::Dropped, "recorder queue is full");
    }
    const auto deadline =
        Clock::now() + impl_->config.enqueue_timeout;
    if (!impl_->not_full.wait_until(
            lock, deadline,
            [&] {
              return impl_->stopping ||
                     !impl_->writer_status ||
                     impl_->queue.size() <
                         impl_->config.queue_capacity;
            })) {
      ++impl_->stats.enqueue_timeouts;
      return Status(
          StatusCode::Timeout,
          "recorder queue wait timed out");
    }
    if (impl_->stopping) {
      return Status(StatusCode::Closed, "recorder is stopping");
    }
    if (!impl_->writer_status) {
      return impl_->writer_status;
    }
  }

  const auto captured_offset =
      capture_now >= impl_->monotonic_start_ns
          ? capture_now - impl_->monotonic_start_ns
          : 0U;
  record.monotonic_offset_ns = std::max(
      captured_offset, impl_->last_record_offset_ns);
  impl_->last_record_offset_ns = record.monotonic_offset_ns;
  record.record_sequence = impl_->next_record_sequence++;
  impl_->queue.push_back(std::move(record));
  ++impl_->stats.accepted_records;
  impl_->stats.queue_depth = impl_->queue.size();
  impl_->stats.queue_high_watermark =
      std::max(
          impl_->stats.queue_high_watermark,
          impl_->queue.size());
  lock.unlock();
  impl_->not_empty.notify_one();
  return Status::Ok();
}

Status Recorder::Stop(Deadline deadline) {
  std::unique_lock join_lock(impl_->join_mutex);
  Status writer_status;
  {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->stopping) {
      impl_->stopping = true;
      impl_->stats.stopping = true;
      impl_->not_empty.notify_all();
      impl_->not_full.notify_all();
    }
    if (!impl_->writer_finished) {
      if (deadline.infinite()) {
        impl_->finished.wait(
            lock, [&] { return impl_->writer_finished; });
      } else if (!impl_->finished.wait_until(
                     lock, deadline.time_point(),
                     [&] { return impl_->writer_finished; })) {
        return Status(
            StatusCode::Timeout,
            "recorder flush deadline expired");
      }
    }
    writer_status = impl_->writer_status;
  }
  if (impl_->writer.joinable()) {
    impl_->writer.join();
  }
  return writer_status;
}

RecorderStats Recorder::Stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

struct RecordingTransport::Impl {
  explicit Impl(RecordingTransportConfig configured)
      : config(std::move(configured)) {}

  [[nodiscard]] std::string MessageType(
      std::string_view topic) const {
    if (config.message_type_resolver) {
      return config.message_type_resolver(topic);
    }
    return config.default_message_type;
  }

  void RecordFailure(const Status& status) {
    std::lock_guard lock(mutex);
    ++capture_stats.record_failures;
    capture_stats.last_record_status = status;
  }

  RecordingTransportConfig config;
  mutable std::mutex mutex;
  bool closed{false};
  RecordingTransportStats capture_stats;
};

RecordingTransport::RecordingTransport(
    RecordingTransportConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
  if (!impl_->config.transport || !impl_->config.recorder ||
      (!impl_->config.message_type_resolver &&
       impl_->config.default_message_type.empty())) {
    throw std::invalid_argument(
        "RecordingTransport requires transport, recorder, and message type");
  }
}

RecordingTransport::~RecordingTransport() {
  if (impl_) {
    static_cast<void>(Close());
  }
}

Status RecordingTransport::Publish(
    std::string_view topic, Message message,
    const QosProfile& qos) {
  Status record_status;
  try {
    const auto message_type = impl_->MessageType(topic);
    record_status = impl_->config.recorder->Record(
        topic, message_type, message);
  } catch (const std::exception& error) {
    record_status = Status(
        StatusCode::Internal,
        std::string("message type resolver threw: ") + error.what());
  } catch (...) {
    record_status = Status(
        StatusCode::Internal,
        "message type resolver threw");
  }

  if (!record_status) {
    impl_->RecordFailure(record_status);
    if (impl_->config.failure_policy ==
        RecordingFailurePolicy::RequireRecord) {
      return record_status;
    }
  }
  return impl_->config.transport->Publish(
      topic, std::move(message), qos);
}

Result<SubscriptionId> RecordingTransport::Subscribe(
    std::string_view topic, const QosProfile& qos,
    TransportMessageCallback callback) {
  return impl_->config.transport->Subscribe(
      topic, qos, std::move(callback));
}

Status RecordingTransport::Unsubscribe(
    SubscriptionId subscription_id) {
  return impl_->config.transport->Unsubscribe(subscription_id);
}

Result<ServiceId> RecordingTransport::AdvertiseService(
    std::string_view service_name,
    TransportServiceCallback callback) {
  return impl_->config.transport->AdvertiseService(
      service_name, std::move(callback));
}

Status RecordingTransport::RemoveService(ServiceId service_id) {
  return impl_->config.transport->RemoveService(service_id);
}

Result<Message> RecordingTransport::Request(
    std::string_view service_name, Message request,
    Deadline deadline) {
  return impl_->config.transport->Request(
      service_name, std::move(request), deadline);
}

TransportStats RecordingTransport::Stats() const {
  return impl_->config.transport->Stats();
}

RecordingTransportStats RecordingTransport::CaptureStats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->capture_stats;
}

Status RecordingTransport::Close() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      return Status::Ok();
    }
    impl_->closed = true;
  }

  Status recorder_status;
  if (impl_->config.stop_recorder_on_close) {
    recorder_status =
        impl_->config.recorder->Stop(Deadline::Infinite());
  }
  const auto transport_status =
      impl_->config.transport->Close();
  if (!recorder_status) {
    return recorder_status;
  }
  return transport_status;
}

struct Replayer::Impl {
  Impl(std::string configured_path, TraceLimits configured_limits)
      : path(std::move(configured_path)),
        limits(configured_limits) {}

  [[nodiscard]] Status OpenAndReadHeader() {
    input.open(path, std::ios::binary | std::ios::in);
    if (!input) {
      return Status(
          StatusCode::NotFound, "open trace input failed");
    }
    std::array<std::byte, kFileHeaderBytes> header{};
    const auto read = ReadExact(input, header, false);
    if (read != StreamRead::Ok) {
      return TraceError("trace header is truncated");
    }
    if (!std::equal(
            kFileMagic.begin(), kFileMagic.end(),
            header.begin())) {
      return TraceError("trace file magic mismatch");
    }
    std::size_t offset = kFileMagic.size();
    std::uint16_t header_size = 0U;
    std::uint64_t reserved = 0U;
    std::uint32_t stored_checksum = 0U;
    if (!ReadU16(header, offset, info.format_version) ||
        !ReadU16(header, offset, header_size) ||
        !ReadU64(header, offset, info.wall_clock_start_ns) ||
        !ReadU64(header, offset, info.monotonic_start_ns) ||
        !ReadU64(header, offset, reserved) ||
        !ReadU32(header, offset, stored_checksum)) {
      return TraceError("trace header decode failed");
    }
    if (info.format_version != kTraceFormatVersion ||
        header_size != kFileHeaderBytes || reserved != 0U) {
      return TraceError("trace header version or size is unsupported");
    }
    const auto calculated = Crc32(
        std::span<const std::byte>(
            header.data(), header.size() - kChecksumBytes));
    if (calculated != stored_checksum) {
      return TraceError("trace header checksum mismatch");
    }
    return Status::Ok();
  }

  [[nodiscard]] Result<std::optional<TraceRecord>> NextLocked() {
    if (terminal_error.has_value()) {
      return *terminal_error;
    }
    if (eof) {
      return std::optional<TraceRecord>{};
    }

    std::array<std::byte, kRecordFixedBytes> fixed{};
    const auto fixed_read = ReadExact(input, fixed, true);
    if (fixed_read == StreamRead::Eof) {
      eof = true;
      return std::optional<TraceRecord>{};
    }
    if (fixed_read == StreamRead::Truncated) {
      ++stats.truncated_records;
      terminal_error =
          TraceError("trace record header is truncated");
      return *terminal_error;
    }

    std::size_t offset = 0U;
    std::uint32_t magic = 0U;
    std::uint16_t version = 0U;
    std::uint16_t flags = 0U;
    std::uint64_t total_u64 = 0U;
    TraceRecord record;
    std::uint16_t envelope_reserved = 0U;
    std::uint32_t priority_bits = 0U;
    std::uint32_t channel_size_u32 = 0U;
    std::uint32_t type_size_u32 = 0U;
    std::uint64_t payload_size_u64 = 0U;
    if (!ReadU32(fixed, offset, magic) ||
        !ReadU16(fixed, offset, version) ||
        !ReadU16(fixed, offset, flags) ||
        !ReadU64(fixed, offset, total_u64) ||
        !ReadU64(fixed, offset, record.record_sequence) ||
        !ReadU64(fixed, offset, record.monotonic_offset_ns) ||
        !ReadU16(
            fixed, offset, record.message.envelope.version) ||
        !ReadU16(fixed, offset, envelope_reserved) ||
        !ReadU32(fixed, offset, priority_bits) ||
        !ReadU64(
            fixed, offset, record.message.envelope.trace_id) ||
        !ReadU64(
            fixed, offset, record.message.envelope.span_id) ||
        !ReadU64(
            fixed, offset,
            record.message.envelope.parent_span_id) ||
        !ReadU64(
            fixed, offset, record.message.envelope.sequence) ||
        !ReadU64(
            fixed, offset,
            record.message.envelope.source_timestamp_ns) ||
        !ReadU64(
            fixed, offset,
            record.message.envelope.publish_timestamp_ns) ||
        !ReadU64(
            fixed, offset,
            record.message.envelope.source_generation) ||
        !ReadU32(fixed, offset, channel_size_u32) ||
        !ReadU32(fixed, offset, type_size_u32) ||
        !ReadU64(fixed, offset, payload_size_u64) ||
        offset != kRecordFixedBytes) {
      ++stats.format_errors;
      terminal_error = TraceError("trace record decode failed");
      return *terminal_error;
    }

    if (magic != kRecordMagic ||
        version != kRecordVersion ||
        flags != 0U || envelope_reserved != 0U ||
        total_u64 < kMinimumRecordBytes ||
        total_u64 >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      ++stats.format_errors;
      terminal_error =
          TraceError("trace record framing is invalid");
      return *terminal_error;
    }

    const auto channel_size =
        static_cast<std::size_t>(channel_size_u32);
    const auto type_size =
        static_cast<std::size_t>(type_size_u32);
    if (payload_size_u64 >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
      ++stats.format_errors;
      terminal_error =
          TraceError("trace payload size exceeds size_t");
      return *terminal_error;
    }
    const auto payload_size =
        static_cast<std::size_t>(payload_size_u64);
    if (channel_size == 0U || type_size == 0U ||
        channel_size > limits.max_channel_bytes ||
        type_size > limits.max_message_type_bytes ||
        payload_size > limits.max_payload_bytes) {
      ++stats.format_errors;
      terminal_error =
          TraceError("trace record exceeds configured limits");
      return *terminal_error;
    }

    std::size_t expected_total = kMinimumRecordBytes;
    if (!CheckedAdd(expected_total, channel_size, expected_total) ||
        !CheckedAdd(expected_total, type_size, expected_total) ||
        !CheckedAdd(expected_total, payload_size, expected_total) ||
        total_u64 != static_cast<std::uint64_t>(expected_total)) {
      ++stats.format_errors;
      terminal_error =
          TraceError("trace record length fields disagree");
      return *terminal_error;
    }

    const auto total = static_cast<std::size_t>(total_u64);
    Bytes bytes;
    try {
      bytes.resize(total);
    } catch (const std::exception& error) {
      terminal_error = Status(
          StatusCode::Internal,
          std::string("allocate trace record failed: ") +
              error.what());
      return *terminal_error;
    }
    std::copy(fixed.begin(), fixed.end(), bytes.begin());
    const auto remainder = std::span<std::byte>(
        bytes.data() + kRecordFixedBytes,
        total - kRecordFixedBytes);
    if (ReadExact(input, remainder, false) != StreamRead::Ok) {
      ++stats.truncated_records;
      terminal_error =
          TraceError("trace record body is truncated");
      return *terminal_error;
    }

    std::size_t checksum_offset = total - kChecksumBytes;
    std::uint32_t stored_checksum = 0U;
    if (!ReadU32(bytes, checksum_offset, stored_checksum) ||
        checksum_offset != total) {
      ++stats.format_errors;
      terminal_error =
          TraceError("trace checksum field is invalid");
      return *terminal_error;
    }
    const auto calculated = Crc32(
        std::span<const std::byte>(
            bytes.data(), total - kChecksumBytes));
    if (calculated != stored_checksum) {
      ++stats.checksum_mismatches;
      terminal_error =
          TraceError("trace record checksum mismatch");
      return *terminal_error;
    }

    if (record.record_sequence != expected_sequence ||
        (record.record_sequence > 1U &&
         record.monotonic_offset_ns < last_offset_ns)) {
      ++stats.sequence_mismatches;
      terminal_error =
          TraceError("trace record sequence or timestamp regressed");
      return *terminal_error;
    }

    std::size_t variable_offset = kRecordFixedBytes;
    try {
      record.channel.assign(
          reinterpret_cast<const char*>(
              bytes.data() + variable_offset),
          channel_size);
      variable_offset += channel_size;
      record.message_type.assign(
          reinterpret_cast<const char*>(
              bytes.data() + variable_offset),
          type_size);
      variable_offset += type_size;
      record.message.payload.assign(
          bytes.begin() +
              static_cast<std::ptrdiff_t>(variable_offset),
          bytes.begin() +
              static_cast<std::ptrdiff_t>(
                  variable_offset + payload_size));
    } catch (const std::exception& error) {
      terminal_error = Status(
          StatusCode::Internal,
          std::string("materialize trace record failed: ") +
              error.what());
      return *terminal_error;
    }
    record.message.envelope.priority =
        std::bit_cast<std::int32_t>(priority_bits);
    record.checksum = stored_checksum;

    ++expected_sequence;
    last_offset_ns = record.monotonic_offset_ns;
    ++stats.records_read;
    stats.payload_bytes +=
        static_cast<std::uint64_t>(payload_size);
    return std::optional<TraceRecord>(std::move(record));
  }

  std::string path;
  TraceLimits limits;
  std::ifstream input;
  TraceFileInfo info;
  mutable std::mutex mutex;
  std::uint64_t expected_sequence{1U};
  std::uint64_t last_offset_ns{0U};
  bool eof{false};
  std::optional<Status> terminal_error;
  ReplayStats stats;
};

Result<std::unique_ptr<Replayer>> Replayer::Open(
    std::string path, TraceLimits limits) {
  if (path.empty()) {
    return Status(
        StatusCode::InvalidArgument,
        "replayer requires a trace path");
  }
  const auto limits_status = ValidateLimits(limits);
  if (!limits_status) {
    return limits_status;
  }
  try {
    auto impl =
        std::make_unique<Impl>(std::move(path), limits);
    const auto open_status = impl->OpenAndReadHeader();
    if (!open_status) {
      return open_status;
    }
    return std::unique_ptr<Replayer>(
        new Replayer(std::move(impl)));
  } catch (const std::exception& error) {
    return Status(
        StatusCode::TransportError,
        std::string("open replayer failed: ") + error.what());
  }
}

Replayer::Replayer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

Replayer::~Replayer() = default;

const TraceFileInfo& Replayer::Info() const noexcept {
  return impl_->info;
}

Result<std::optional<TraceRecord>> Replayer::Next() {
  std::lock_guard lock(impl_->mutex);
  return impl_->NextLocked();
}

Status Replayer::Reset() {
  std::lock_guard lock(impl_->mutex);
  impl_->input.clear();
  impl_->input.seekg(
      static_cast<std::streamoff>(kFileHeaderBytes),
      std::ios::beg);
  if (!impl_->input) {
    return TraceError("seek trace input failed");
  }
  impl_->expected_sequence = 1U;
  impl_->last_offset_ns = 0U;
  impl_->eof = false;
  impl_->terminal_error.reset();
  impl_->stats = ReplayStats{};
  return Status::Ok();
}

Status Replayer::Replay(
    const ReplayOptions& options,
    ReplayCallback callback,
    std::stop_token stop_token) {
  if (!callback) {
    return Status(
        StatusCode::InvalidArgument,
        "replay requires a callback");
  }
  if (!std::isfinite(options.speed) ||
      options.speed <= 0.0 ||
      (options.timing == ReplayTiming::Accelerated &&
       options.speed <= 1.0)) {
    return Status(
        StatusCode::InvalidArgument,
        "replay speed is invalid for the selected mode");
  }

  std::optional<std::uint64_t> first_offset;
  const auto replay_start = Clock::now();
  for (;;) {
    if (stop_token.stop_requested()) {
      return Status(
          StatusCode::Cancelled, "replay was cancelled");
    }
    auto next = Next();
    if (!next) {
      return next.status();
    }
    if (!next.value().has_value()) {
      return Status::Ok();
    }
    auto record = std::move(*next.value());

    if (options.timing != ReplayTiming::AsFastAsPossible) {
      if (!first_offset.has_value()) {
        first_offset = record.monotonic_offset_ns;
      }
      const auto relative_ns =
          record.monotonic_offset_ns - *first_offset;
      const long double divisor =
          options.timing == ReplayTiming::Accelerated
              ? static_cast<long double>(options.speed)
              : 1.0L;
      const long double scaled =
          static_cast<long double>(relative_ns) / divisor;
      if (scaled >
          static_cast<long double>(
              std::numeric_limits<std::int64_t>::max())) {
        return TraceError(
            "replay timestamp exceeds nanosecond duration range");
      }
      const auto scaled_duration = std::chrono::nanoseconds(
          static_cast<std::int64_t>(scaled));
      if (scaled_duration >
          Clock::time_point::max() - replay_start) {
        return TraceError(
            "replay target time point would overflow");
      }
      const auto target = replay_start + scaled_duration;
      while (Clock::now() < target) {
        if (stop_token.stop_requested()) {
          return Status(
              StatusCode::Cancelled, "replay was cancelled");
        }
        const auto remaining = target - Clock::now();
        const auto slice = std::min(
            remaining,
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::milliseconds(5)));
        if (slice > Clock::duration::zero()) {
          std::this_thread::sleep_for(slice);
        }
      }
      const auto delivered_at = Clock::now();
      const auto drift =
          delivered_at > target
              ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                    delivered_at - target)
              : std::chrono::nanoseconds::zero();
      {
        std::lock_guard lock(impl_->mutex);
        ++impl_->stats.timing_observations;
        impl_->stats.total_timing_drift += drift;
        impl_->stats.maximum_timing_drift =
            std::max(
                impl_->stats.maximum_timing_drift, drift);
      }
    }

    Status callback_status;
    try {
      callback_status = callback(record);
    } catch (const std::exception& error) {
      callback_status = Status(
          StatusCode::Internal,
          std::string("replay callback threw: ") + error.what());
    } catch (...) {
      callback_status = Status(
          StatusCode::Internal, "replay callback threw");
    }
    if (!callback_status) {
      return callback_status;
    }
    {
      std::lock_guard lock(impl_->mutex);
      ++impl_->stats.records_delivered;
    }
  }
}

ReplayStats Replayer::Stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

}  // namespace autoruntime
