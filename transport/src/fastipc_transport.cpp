#include <autoruntime/fastipc_transport.hpp>

#include <fastipc/shared_memory_transport.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autoruntime {
namespace {

constexpr std::uint32_t kWireMagic = 0x49524641U;
constexpr std::uint16_t kWireVersion = 1U;
constexpr auto kDefaultSendDeadline = std::chrono::milliseconds(100);
constexpr auto kReceivePollInterval = std::chrono::milliseconds(50);

struct WireHeader {
  std::uint32_t magic{kWireMagic};
  std::uint16_t wire_version{kWireVersion};
  std::uint16_t header_bytes{sizeof(WireHeader)};
  std::uint16_t envelope_version{kMessageEnvelopeVersion};
  std::uint16_t reserved16{0U};
  std::uint32_t reserved32{0U};
  std::uint64_t trace_id{0U};
  std::uint64_t span_id{0U};
  std::uint64_t parent_span_id{0U};
  std::uint64_t sequence{0U};
  std::uint64_t source_timestamp_ns{0U};
  std::uint64_t publish_timestamp_ns{0U};
  std::uint64_t source_generation{0U};
  std::int32_t priority{0};
  std::uint32_t payload_bytes{0U};
};
static_assert(sizeof(WireHeader) == 80U);

[[nodiscard]] Status MapStatus(const fastipc::Status& status) {
  using FastCode = fastipc::StatusCode;
  switch (status.code()) {
    case FastCode::Ok:
      return Status::Ok();
    case FastCode::InvalidArgument:
    case FastCode::MessageTooLarge:
    case FastCode::BufferTooSmall:
      return Status(StatusCode::InvalidArgument, status.detail());
    case FastCode::AlreadyExists:
    case FastCode::RoleConflict:
      return Status(StatusCode::AlreadyExists, status.detail());
    case FastCode::NotFound:
    case FastCode::PeerUnavailable:
      return Status(StatusCode::NotFound, status.detail());
    case FastCode::WouldBlock:
      return Status(StatusCode::QueueFull, status.detail());
    case FastCode::Timeout:
      return Status(StatusCode::Timeout, status.detail());
    case FastCode::Dropped:
      return Status(StatusCode::Dropped, status.detail());
    case FastCode::Closed:
      return Status(StatusCode::Closed, status.detail());
    case FastCode::LayoutMismatch:
    case FastCode::PeerDead:
    case FastCode::StaleGeneration:
    case FastCode::CorruptData:
    case FastCode::PermissionDenied:
    case FastCode::IoError:
      return Status(StatusCode::TransportError, status.detail());
    case FastCode::Unsupported:
      return Status(StatusCode::Unsupported, status.detail());
  }
  return Status(StatusCode::TransportError, "unknown FastIPC status");
}

[[nodiscard]] Result<std::vector<std::byte>> Encode(
    const Message& message, std::uint32_t maximum_payload) {
  if (message.payload.size() > maximum_payload ||
      message.payload.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return Status(StatusCode::InvalidArgument,
                  "message exceeds configured FastIPC payload limit");
  }

  WireHeader header;
  header.envelope_version = message.envelope.version;
  header.trace_id = message.envelope.trace_id;
  header.span_id = message.envelope.span_id;
  header.parent_span_id = message.envelope.parent_span_id;
  header.sequence = message.envelope.sequence;
  header.source_timestamp_ns = message.envelope.source_timestamp_ns;
  header.publish_timestamp_ns = message.envelope.publish_timestamp_ns;
  header.source_generation = message.envelope.source_generation;
  header.priority = message.envelope.priority;
  header.payload_bytes =
      static_cast<std::uint32_t>(message.payload.size());

  std::vector<std::byte> wire(sizeof(header) + message.payload.size());
  std::memcpy(wire.data(), &header, sizeof(header));
  if (!message.payload.empty()) {
    std::memcpy(wire.data() + sizeof(header), message.payload.data(),
                message.payload.size());
  }
  return wire;
}

[[nodiscard]] Result<Message> Decode(std::span<const std::byte> wire,
                                     std::uint32_t maximum_payload) {
  if (wire.size() < sizeof(WireHeader)) {
    return Status(StatusCode::TransportError,
                  "FastIPC frame is smaller than its header");
  }
  WireHeader header;
  std::memcpy(&header, wire.data(), sizeof(header));
  if (header.magic != kWireMagic ||
      header.wire_version != kWireVersion ||
      header.header_bytes != sizeof(WireHeader) ||
      header.envelope_version != kMessageEnvelopeVersion ||
      header.reserved16 != 0U || header.reserved32 != 0U) {
    return Status(StatusCode::TransportError,
                  "FastIPC frame header is incompatible");
  }
  const auto payload_bytes =
      static_cast<std::size_t>(header.payload_bytes);
  if (payload_bytes > maximum_payload ||
      sizeof(WireHeader) + payload_bytes != wire.size()) {
    return Status(StatusCode::TransportError,
                  "FastIPC frame payload length is invalid");
  }

  Message message;
  message.envelope.version = header.envelope_version;
  message.envelope.trace_id = header.trace_id;
  message.envelope.span_id = header.span_id;
  message.envelope.parent_span_id = header.parent_span_id;
  message.envelope.sequence = header.sequence;
  message.envelope.source_timestamp_ns = header.source_timestamp_ns;
  message.envelope.publish_timestamp_ns = header.publish_timestamp_ns;
  message.envelope.source_generation = header.source_generation;
  message.envelope.priority = header.priority;
  message.payload.resize(payload_bytes);
  if (payload_bytes != 0U) {
    std::memcpy(message.payload.data(), wire.data() + sizeof(WireHeader),
                payload_bytes);
  }
  return message;
}

[[nodiscard]] fastipc::ChannelConfig ChannelConfigFor(
    const FastIpcEndpointConfig& endpoint) {
  fastipc::ChannelConfig config;
  config.name = endpoint.channel_name;
  config.slot_count = endpoint.slot_count;
  config.max_message_size =
      static_cast<std::uint32_t>(sizeof(WireHeader)) +
      endpoint.max_payload_size;
  config.unlink_on_owner_close = endpoint.unlink_on_owner_close;
  config.peer_timeout = endpoint.peer_timeout;
  return config;
}

}  // namespace

struct FastIpcTransport::Impl {
  struct Endpoint {
    FastIpcEndpointConfig config;
    std::unique_ptr<fastipc::SharedMemoryTransport> channel;
    std::mutex send_mutex;
    std::mutex callback_mutex;
    TransportMessageCallback callback;
    QosProfile qos;
    SubscriptionId subscription_id{0U};
    std::jthread receiver;
  };

  mutable std::mutex mutex;
  bool closed{false};
  SubscriptionId next_subscription_id{1U};
  std::unordered_map<std::string, std::shared_ptr<Endpoint>> endpoints;
  std::unordered_map<SubscriptionId, std::shared_ptr<Endpoint>> subscriptions;
  mutable std::mutex stats_mutex;
  TransportStats stats;

  void CountPublishFailure() {
    std::lock_guard lock(stats_mutex);
    ++stats.publish_failures;
  }

  void ReceiveLoop(const std::shared_ptr<Endpoint>& endpoint,
                   std::stop_token stop_token) {
    const auto wire_capacity =
        sizeof(WireHeader) +
        static_cast<std::size_t>(endpoint->config.max_payload_size);
    std::vector<std::byte> buffer(wire_capacity);
    while (!stop_token.stop_requested()) {
      auto result = endpoint->channel->Receive(
          buffer, fastipc::Deadline::After(kReceivePollInterval));
      if (!result) {
        const auto code = result.status().code();
        if (code == fastipc::StatusCode::Timeout ||
            code == fastipc::StatusCode::PeerUnavailable ||
            code == fastipc::StatusCode::PeerDead) {
          if (code == fastipc::StatusCode::PeerDead) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
          continue;
        }
        if (code == fastipc::StatusCode::Closed) {
          return;
        }
        std::lock_guard lock(stats_mutex);
        ++stats.dropped_messages;
        continue;
      }

      auto decoded = Decode(
          std::span<const std::byte>(buffer.data(), result.value()),
          endpoint->config.max_payload_size);
      if (!decoded) {
        std::lock_guard lock(stats_mutex);
        ++stats.dropped_messages;
        continue;
      }

      TransportMessageCallback callback;
      {
        std::lock_guard lock(endpoint->callback_mutex);
        callback = endpoint->callback;
      }
      if (!callback) {
        std::lock_guard lock(stats_mutex);
        ++stats.dropped_messages;
        continue;
      }
      try {
        callback(std::move(decoded).take_value());
        std::lock_guard lock(stats_mutex);
        ++stats.delivered_messages;
      } catch (...) {
        std::lock_guard lock(stats_mutex);
        ++stats.dropped_messages;
      }
    }
  }
};

FastIpcTransport::FastIpcTransport(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

FastIpcTransport::~FastIpcTransport() {
  static_cast<void>(Close());
}

Result<std::shared_ptr<FastIpcTransport>> FastIpcTransport::Create(
    FastIpcTransportConfig config) {
  if (config.endpoints.empty() ||
      config.open_timeout <= std::chrono::milliseconds::zero()) {
    return Status(StatusCode::InvalidArgument,
                  "FastIPC transport requires endpoints and open timeout");
  }

  auto impl = std::make_unique<Impl>();
  for (auto& endpoint_config : config.endpoints) {
    if (endpoint_config.topic.empty() ||
        endpoint_config.channel_name.empty() ||
        endpoint_config.slot_count == 0U ||
        endpoint_config.max_payload_size == 0U ||
        endpoint_config.max_payload_size >
            std::numeric_limits<std::uint32_t>::max() -
                sizeof(WireHeader) ||
        impl->endpoints.contains(endpoint_config.topic)) {
      return Status(StatusCode::InvalidArgument,
                    "invalid or duplicate FastIPC endpoint");
    }

    auto endpoint = std::make_shared<Impl::Endpoint>();
    endpoint->config = endpoint_config;
    const auto channel_config = ChannelConfigFor(endpoint_config);
    if (endpoint_config.direction == FastIpcDirection::Publish) {
      auto result =
          fastipc::SharedMemoryTransport::CreateProducer(channel_config);
      if (!result) {
        return MapStatus(result.status());
      }
      endpoint->channel = std::move(result).take_value();
    } else {
      const auto open_deadline =
          std::chrono::steady_clock::now() + config.open_timeout;
      for (;;) {
        auto result =
            fastipc::SharedMemoryTransport::OpenConsumer(channel_config);
        if (result) {
          endpoint->channel = std::move(result).take_value();
          break;
        }
        if (std::chrono::steady_clock::now() >= open_deadline) {
          return MapStatus(result.status());
        }
        if (result.status().code() != fastipc::StatusCode::NotFound &&
            result.status().code() !=
                fastipc::StatusCode::PeerUnavailable) {
          return MapStatus(result.status());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    impl->endpoints.emplace(endpoint_config.topic, std::move(endpoint));
  }

  return std::shared_ptr<FastIpcTransport>(
      new FastIpcTransport(std::move(impl)));
}

Status FastIpcTransport::Publish(std::string_view topic, Message message,
                                 const QosProfile& qos) {
  std::shared_ptr<Impl::Endpoint> endpoint;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      impl_->CountPublishFailure();
      return Status(StatusCode::Closed, "FastIPC transport is closed");
    }
    const auto iterator = impl_->endpoints.find(std::string(topic));
    if (iterator == impl_->endpoints.end() ||
        iterator->second->config.direction != FastIpcDirection::Publish) {
      impl_->CountPublishFailure();
      return Status(StatusCode::NotFound,
                    "FastIPC publish endpoint is not configured");
    }
    endpoint = iterator->second;
  }

  auto encoded = Encode(message, endpoint->config.max_payload_size);
  if (!encoded) {
    impl_->CountPublishFailure();
    return encoded.status();
  }

  fastipc::SendOptions options;
  if (qos.reliability == Reliability::BestEffort) {
    options.policy = fastipc::BackpressurePolicy::Drop;
    options.deadline = fastipc::Deadline::Immediate();
  } else {
    options.policy = fastipc::BackpressurePolicy::Timeout;
    const auto timeout =
        qos.deadline > std::chrono::nanoseconds::zero()
            ? qos.deadline
            : std::chrono::duration_cast<std::chrono::nanoseconds>(
                  kDefaultSendDeadline);
    options.deadline = fastipc::Deadline::After(timeout);
  }

  fastipc::Status send_status;
  {
    std::lock_guard lock(endpoint->send_mutex);
    send_status = endpoint->channel->Send(encoded.value(), options);
  }
  if (!send_status) {
    impl_->CountPublishFailure();
    const auto mapped = MapStatus(send_status);
    if (mapped.code() == StatusCode::Dropped) {
      std::lock_guard lock(impl_->stats_mutex);
      ++impl_->stats.dropped_messages;
    }
    return mapped;
  }
  {
    std::lock_guard lock(impl_->stats_mutex);
    ++impl_->stats.published_messages;
  }
  return Status::Ok();
}

Result<SubscriptionId> FastIpcTransport::Subscribe(
    std::string_view topic, const QosProfile& qos,
    TransportMessageCallback callback) {
  if (!callback || qos.depth == 0U) {
    return Status(StatusCode::InvalidArgument,
                  "FastIPC subscription requires callback and depth");
  }

  std::shared_ptr<Impl::Endpoint> endpoint;
  SubscriptionId id = 0U;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      return Status(StatusCode::Closed, "FastIPC transport is closed");
    }
    const auto iterator = impl_->endpoints.find(std::string(topic));
    if (iterator == impl_->endpoints.end() ||
        iterator->second->config.direction !=
            FastIpcDirection::Subscribe) {
      return Status(StatusCode::NotFound,
                    "FastIPC subscribe endpoint is not configured");
    }
    endpoint = iterator->second;
    if (endpoint->subscription_id != 0U) {
      return Status(StatusCode::AlreadyExists,
                    "FastIPC SPSC endpoint already has a subscriber");
    }
    id = impl_->next_subscription_id++;
    endpoint->subscription_id = id;
    impl_->subscriptions.emplace(id, endpoint);
  }
  {
    std::lock_guard lock(endpoint->callback_mutex);
    endpoint->qos = qos;
    endpoint->callback = std::move(callback);
  }
  try {
    endpoint->receiver = std::jthread(
        [this, endpoint](std::stop_token stop_token) {
          impl_->ReceiveLoop(endpoint, stop_token);
        });
  } catch (const std::system_error& error) {
    {
      std::lock_guard lock(impl_->mutex);
      impl_->subscriptions.erase(id);
      endpoint->subscription_id = 0U;
    }
    return Status(StatusCode::Internal,
                  std::string("failed to start FastIPC receiver: ") +
                      error.what());
  }
  return id;
}

Status FastIpcTransport::Unsubscribe(SubscriptionId subscription_id) {
  std::shared_ptr<Impl::Endpoint> endpoint;
  {
    std::lock_guard lock(impl_->mutex);
    const auto iterator = impl_->subscriptions.find(subscription_id);
    if (iterator == impl_->subscriptions.end()) {
      return Status(StatusCode::NotFound,
                    "FastIPC subscription does not exist");
    }
    endpoint = iterator->second;
    impl_->subscriptions.erase(iterator);
    endpoint->subscription_id = 0U;
  }
  {
    std::lock_guard lock(endpoint->callback_mutex);
    endpoint->callback = {};
  }
  endpoint->receiver.request_stop();
  endpoint->channel->Close();
  if (endpoint->receiver.joinable()) {
    endpoint->receiver.join();
  }
  return Status::Ok();
}

Result<ServiceId> FastIpcTransport::AdvertiseService(
    std::string_view service_name, TransportServiceCallback callback) {
  static_cast<void>(service_name);
  static_cast<void>(callback);
  return Status(StatusCode::Unsupported,
                "FastIPC adapter currently supports pub/sub only");
}

Status FastIpcTransport::RemoveService(ServiceId service_id) {
  static_cast<void>(service_id);
  return Status(StatusCode::Unsupported,
                "FastIPC adapter currently supports pub/sub only");
}

Result<Message> FastIpcTransport::Request(
    std::string_view service_name, Message request, Deadline deadline) {
  static_cast<void>(service_name);
  static_cast<void>(request);
  static_cast<void>(deadline);
  return Status(StatusCode::Unsupported,
                "FastIPC adapter currently supports pub/sub only");
}

TransportStats FastIpcTransport::Stats() const {
  std::lock_guard lock(impl_->stats_mutex);
  return impl_->stats;
}

Status FastIpcTransport::Close() {
  std::vector<std::shared_ptr<Impl::Endpoint>> endpoints;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      return Status::Ok();
    }
    impl_->closed = true;
    for (const auto& [unused_topic, endpoint] : impl_->endpoints) {
      static_cast<void>(unused_topic);
      endpoints.push_back(endpoint);
    }
    impl_->subscriptions.clear();
  }
  for (const auto& endpoint : endpoints) {
    endpoint->receiver.request_stop();
    endpoint->channel->Close();
  }
  for (const auto& endpoint : endpoints) {
    if (endpoint->receiver.joinable()) {
      endpoint->receiver.join();
    }
  }
  return Status::Ok();
}

}  // namespace autoruntime
