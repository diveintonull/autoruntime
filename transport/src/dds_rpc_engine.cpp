#include "dds_rpc_engine.hpp"

#include "autoruntime_message.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autoruntime::detail {
namespace {

constexpr std::uint16_t kRpcProtocolVersion = 1U;
constexpr std::size_t kMaximumServiceNameBytes = 128U;
constexpr std::size_t kMaximumErrorDetailBytes = 1024U;
constexpr std::int32_t kRpcHistoryDepth = 64;
constexpr std::uint64_t kInfiniteBudget =
    std::numeric_limits<std::uint64_t>::max();

struct QosDeleter {
  void operator()(dds_qos_t* qos) const noexcept {
    if (qos != nullptr) {
      dds_delete_qos(qos);
    }
  }
};
using QosPtr = std::unique_ptr<dds_qos_t, QosDeleter>;

[[nodiscard]] Status DdsError(
    dds_return_t result, std::string_view operation) {
  if (result >= DDS_RETCODE_OK) {
    return Status::Ok();
  }

  StatusCode code = StatusCode::TransportError;
  switch (result) {
    case DDS_RETCODE_UNSUPPORTED:
      code = StatusCode::Unsupported;
      break;
    case DDS_RETCODE_BAD_PARAMETER:
    case DDS_RETCODE_PRECONDITION_NOT_MET:
    case DDS_RETCODE_IMMUTABLE_POLICY:
    case DDS_RETCODE_INCONSISTENT_POLICY:
    case DDS_RETCODE_ILLEGAL_OPERATION:
      code = StatusCode::InvalidArgument;
      break;
    case DDS_RETCODE_OUT_OF_RESOURCES:
      code = StatusCode::QueueFull;
      break;
    case DDS_RETCODE_ALREADY_DELETED:
      code = StatusCode::Closed;
      break;
    case DDS_RETCODE_TIMEOUT:
      code = StatusCode::Timeout;
      break;
    default:
      break;
  }

  std::string detail(operation);
  detail.append(": ");
  detail.append(dds_strretcode(-result));
  return Status(code, std::move(detail));
}

[[nodiscard]] QosPtr RpcEndpointQos(
    std::chrono::milliseconds reliability_max_blocking_time) {
  QosPtr qos(dds_create_qos());
  if (!qos) {
    return qos;
  }
  dds_qset_reliability(
      qos.get(), DDS_RELIABILITY_RELIABLE,
      static_cast<dds_duration_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              reliability_max_blocking_time)
              .count()));
  dds_qset_history(
      qos.get(), DDS_HISTORY_KEEP_LAST, kRpcHistoryDepth);
  return qos;
}

[[nodiscard]] bool ValidServiceName(
    std::string_view service_name) {
  return !service_name.empty() &&
         service_name.size() <= kMaximumServiceNameBytes &&
         service_name.find('\0') == std::string_view::npos;
}

template <std::size_t Size>
[[nodiscard]] bool CopyBoundedString(
    char (&destination)[Size], std::string_view source) {
  if (source.size() >= Size) {
    return false;
  }
  std::fill(std::begin(destination), std::end(destination), '\0');
  std::memcpy(destination, source.data(), source.size());
  return true;
}

template <std::size_t Size>
[[nodiscard]] std::optional<std::string_view> ReadBoundedString(
    const char (&source)[Size]) {
  const auto end =
      std::find(std::begin(source), std::end(source), '\0');
  if (end == std::end(source)) {
    return std::nullopt;
  }
  return std::string_view(
      source, static_cast<std::size_t>(end - std::begin(source)));
}

[[nodiscard]] std::uint64_t HashServiceName(
    std::string_view service_name) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char character : service_name) {
    hash ^= static_cast<std::uint64_t>(
        static_cast<unsigned char>(character));
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] std::string Hex(std::uint64_t value) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::array<char, 16U> encoded{};
  for (std::size_t index = 0U; index < encoded.size(); ++index) {
    const auto shift =
        static_cast<unsigned int>((encoded.size() - index - 1U) * 4U);
    encoded[index] = digits[
        static_cast<std::size_t>((value >> shift) & 0xFU)];
  }
  return std::string(encoded.data(), encoded.size());
}

[[nodiscard]] std::string RequestTopicName(
    std::string_view service_name) {
  return "autoruntime_rpc_request_v1_" +
         Hex(HashServiceName(service_name));
}

[[nodiscard]] std::string ResponseTopicName(
    std::string_view service_name) {
  return "autoruntime_rpc_response_v1_" +
         Hex(HashServiceName(service_name));
}

[[nodiscard]] std::uint64_t GuidHalf(
    const dds_guid_t& guid, std::size_t offset) noexcept {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value = (value << 8U) |
            static_cast<std::uint64_t>(guid.v[offset + index]);
  }
  return value;
}

void EncodeMessage(
    const Message& message, autoruntime_dds_Message* sample) {
  sample->envelope_version = message.envelope.version;
  sample->trace_id = message.envelope.trace_id;
  sample->span_id = message.envelope.span_id;
  sample->parent_span_id = message.envelope.parent_span_id;
  sample->message_sequence = message.envelope.sequence;
  sample->source_timestamp_ns =
      message.envelope.source_timestamp_ns;
  sample->publish_timestamp_ns =
      message.envelope.publish_timestamp_ns;
  sample->source_generation =
      message.envelope.source_generation;
  sample->priority = message.envelope.priority;
  sample->payload._maximum =
      static_cast<std::uint32_t>(message.payload.size());
  sample->payload._length =
      static_cast<std::uint32_t>(message.payload.size());
  sample->payload._buffer =
      const_cast<std::uint8_t*>(
          reinterpret_cast<const std::uint8_t*>(
              message.payload.data()));
  sample->payload._release = false;
}

[[nodiscard]] bool ValidWireMessage(
    const autoruntime_dds_Message& sample,
    std::size_t max_payload_size) {
  return sample.envelope_version == kMessageEnvelopeVersion &&
         sample.payload._length <= max_payload_size &&
         (sample.payload._length == 0U ||
          sample.payload._buffer != nullptr);
}

[[nodiscard]] Message DecodeMessage(
    const autoruntime_dds_Message& sample) {
  Message message;
  message.envelope.version = sample.envelope_version;
  message.envelope.trace_id = sample.trace_id;
  message.envelope.span_id = sample.span_id;
  message.envelope.parent_span_id = sample.parent_span_id;
  message.envelope.sequence = sample.message_sequence;
  message.envelope.source_timestamp_ns =
      sample.source_timestamp_ns;
  message.envelope.publish_timestamp_ns =
      sample.publish_timestamp_ns;
  message.envelope.source_generation =
      sample.source_generation;
  message.envelope.priority = sample.priority;
  if (sample.payload._length != 0U) {
    const auto* begin =
        reinterpret_cast<const std::byte*>(
            sample.payload._buffer);
    message.payload.assign(
        begin, begin + sample.payload._length);
  }
  return message;
}

[[nodiscard]] std::uint64_t RemainingBudget(
    Deadline deadline) noexcept {
  if (deadline.infinite()) {
    return kInfiniteBudget;
  }
  const auto remaining =
      deadline.time_point() - Deadline::Clock::now();
  if (remaining <= Deadline::Clock::duration::zero()) {
    return 0U;
  }
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          remaining);
  if (nanoseconds < remaining) {
    nanoseconds += std::chrono::nanoseconds(1);
  }
  return static_cast<std::uint64_t>(
      std::max<std::int64_t>(1, nanoseconds.count()));
}

[[nodiscard]] Deadline DeadlineFromBudget(
    std::uint64_t budget) noexcept {
  if (budget == kInfiniteBudget) {
    return Deadline::Infinite();
  }
  const auto bounded = std::min<std::uint64_t>(
      budget,
      static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max()));
  return Deadline::After(
      std::chrono::nanoseconds(
          static_cast<std::int64_t>(bounded)));
}

[[nodiscard]] bool ValidStatusCode(
    std::uint16_t raw) noexcept {
  return raw <= static_cast<std::uint16_t>(
                    StatusCode::Internal);
}

void MergeStatus(Status candidate, Status* aggregate) {
  if (*aggregate && !candidate) {
    *aggregate = std::move(candidate);
  }
}

}  // namespace

struct DdsRpcEngine::Impl {
  struct PendingRequest {
    bool completed{false};
    std::optional<Result<Message>> result;
  };

  struct ClientChannel {
    std::string service_name;
    dds_entity_t request_writer{0};
    dds_entity_t response_reader{0};
    std::atomic<bool> closed{false};
    std::mutex write_mutex;
    std::mutex state_mutex;
    std::condition_variable state_changed;
    std::unordered_map<
        std::uint64_t, std::shared_ptr<PendingRequest>>
        pending;
    std::jthread receiver;
  };

  struct ServiceEndpoint {
    ServiceId id{0U};
    std::string service_name;
    dds_entity_t request_reader{0};
    dds_entity_t response_writer{0};
    TransportServiceCallback callback;
    std::atomic<bool> closed{false};
    std::jthread receiver;
  };

  dds_entity_t participant{0};
  DdsTransportConfig config;
  DdsRpcStatsHooks hooks;
  std::uint64_t client_guid_high{0U};
  std::uint64_t client_guid_low{0U};
  std::atomic<std::uint64_t> next_request_id{1U};
  ServiceId next_service_id{1U};
  std::mutex mutex;
  bool closed{false};
  std::unordered_map<std::string, dds_entity_t> request_topics;
  std::unordered_map<std::string, dds_entity_t> response_topics;
  std::unordered_map<
      std::string, std::shared_ptr<ClientChannel>>
      clients;
  std::unordered_map<
      ServiceId, std::shared_ptr<ServiceEndpoint>>
      services;
  std::unordered_map<std::string, ServiceId> service_names;

  void CountRequest() const {
    if (hooks.count_request) {
      hooks.count_request();
    }
  }

  void CountFailure() const {
    if (hooks.count_failure) {
      hooks.count_failure();
    }
  }

  void CountDropped() const {
    if (hooks.count_dropped) {
      hooks.count_dropped();
    }
  }

  [[nodiscard]] Result<dds_entity_t> TopicLocked(
      std::unordered_map<std::string, dds_entity_t>* topics,
      std::string topic_name,
      const dds_topic_descriptor_t* descriptor) {
    const auto existing = topics->find(topic_name);
    if (existing != topics->end()) {
      return existing->second;
    }
    const auto topic = dds_create_topic(
        participant, descriptor, topic_name.c_str(),
        nullptr, nullptr);
    if (topic < 0) {
      return DdsError(topic, "dds_create_topic(RPC)");
    }
    topics->emplace(std::move(topic_name), topic);
    return topic;
  }

  [[nodiscard]] Result<
      std::shared_ptr<ClientChannel>>
  ClientLocked(std::string_view service_name) {
    const std::string key(service_name);
    const auto existing = clients.find(key);
    if (existing != clients.end()) {
      return existing->second;
    }

    auto request_topic = TopicLocked(
        &request_topics, RequestTopicName(service_name),
        &autoruntime_dds_RpcRequest_desc);
    if (!request_topic) {
      return request_topic.status();
    }
    auto response_topic = TopicLocked(
        &response_topics, ResponseTopicName(service_name),
        &autoruntime_dds_RpcResponse_desc);
    if (!response_topic) {
      return response_topic.status();
    }
    auto qos = RpcEndpointQos(
        config.reliability_max_blocking_time);
    if (!qos) {
      return Status(
          StatusCode::Internal,
          "dds_create_qos failed for RPC client");
    }
    const auto writer = dds_create_writer(
        participant, request_topic.value(), qos.get(), nullptr);
    if (writer < 0) {
      return DdsError(writer, "dds_create_writer(RPC request)");
    }
    const auto reader = dds_create_reader(
        participant, response_topic.value(), qos.get(), nullptr);
    if (reader < 0) {
      static_cast<void>(dds_delete(writer));
      return DdsError(reader, "dds_create_reader(RPC response)");
    }

    auto channel = std::make_shared<ClientChannel>();
    channel->service_name = key;
    channel->request_writer = writer;
    channel->response_reader = reader;
    try {
      channel->receiver = std::jthread(
          [this, channel](std::stop_token stop_token) {
            ReceiveResponses(channel, stop_token);
          });
    } catch (...) {
      static_cast<void>(dds_delete(reader));
      static_cast<void>(dds_delete(writer));
      return Status(
          StatusCode::Internal,
          "failed to start DDS RPC response thread");
    }
    clients.emplace(key, channel);
    return channel;
  }

  [[nodiscard]] Result<
      std::shared_ptr<ServiceEndpoint>>
  CreateServiceLocked(
      std::string service_name, ServiceId id,
      TransportServiceCallback callback) {
    auto request_topic = TopicLocked(
        &request_topics, RequestTopicName(service_name),
        &autoruntime_dds_RpcRequest_desc);
    if (!request_topic) {
      return request_topic.status();
    }
    auto response_topic = TopicLocked(
        &response_topics, ResponseTopicName(service_name),
        &autoruntime_dds_RpcResponse_desc);
    if (!response_topic) {
      return response_topic.status();
    }
    auto qos = RpcEndpointQos(
        config.reliability_max_blocking_time);
    if (!qos) {
      return Status(
          StatusCode::Internal,
          "dds_create_qos failed for RPC service");
    }
    const auto reader = dds_create_reader(
        participant, request_topic.value(), qos.get(), nullptr);
    if (reader < 0) {
      return DdsError(reader, "dds_create_reader(RPC request)");
    }
    const auto writer = dds_create_writer(
        participant, response_topic.value(), qos.get(), nullptr);
    if (writer < 0) {
      static_cast<void>(dds_delete(reader));
      return DdsError(writer, "dds_create_writer(RPC response)");
    }

    auto service = std::make_shared<ServiceEndpoint>();
    service->id = id;
    service->service_name = std::move(service_name);
    service->request_reader = reader;
    service->response_writer = writer;
    service->callback = std::move(callback);
    try {
      service->receiver = std::jthread(
          [this, service](std::stop_token stop_token) {
            ReceiveRequests(service, stop_token);
          });
    } catch (...) {
      static_cast<void>(dds_delete(writer));
      static_cast<void>(dds_delete(reader));
      return Status(
          StatusCode::Internal,
          "failed to start DDS RPC service thread");
    }
    return service;
  }

  void ReceiveResponses(
      const std::shared_ptr<ClientChannel>& channel,
      std::stop_token stop_token) {
    auto* sample = autoruntime_dds_RpcResponse__alloc();
    if (sample == nullptr) {
      CountDropped();
      return;
    }
    void* samples[1]{sample};
    dds_sample_info_t information[1]{};

    while (!stop_token.stop_requested()) {
      const auto count = dds_take(
          channel->response_reader, samples, information,
          1U, 1U);
      if (count < 0) {
        if (count == DDS_RETCODE_ALREADY_DELETED) {
          break;
        }
        CountDropped();
        std::this_thread::sleep_for(
            config.receive_poll_interval);
        continue;
      }
      if (count == 0) {
        std::this_thread::sleep_for(
            config.receive_poll_interval);
        continue;
      }
      if (!information[0].valid_data) {
        CountDropped();
        continue;
      }

      const auto service_name =
          ReadBoundedString(sample->service_name);
      const auto error_detail =
          ReadBoundedString(sample->error_detail);
      if (!service_name || !error_detail ||
          *service_name != channel->service_name) {
        continue;
      }
      if (sample->protocol_version !=
              kRpcProtocolVersion ||
          sample->client_guid_high != client_guid_high ||
          sample->client_guid_low != client_guid_low ||
          sample->request_id == 0U ||
          !ValidStatusCode(sample->status_code)) {
        CountDropped();
        continue;
      }

      std::optional<Result<Message>> completion;
      const auto code =
          static_cast<StatusCode>(sample->status_code);
      if (code == StatusCode::Ok) {
        if (!ValidWireMessage(
                sample->message, config.max_payload_size)) {
          CountDropped();
          continue;
        }
        completion.emplace(
            DecodeMessage(sample->message));
      } else {
        completion.emplace(
            Status(code, std::string(*error_detail)));
      }

      bool matched = false;
      {
        std::lock_guard lock(channel->state_mutex);
        const auto iterator =
            channel->pending.find(sample->request_id);
        if (iterator != channel->pending.end()) {
          iterator->second->result.emplace(
              std::move(completion.value()));
          iterator->second->completed = true;
          channel->pending.erase(iterator);
          matched = true;
        }
      }
      if (matched) {
        channel->state_changed.notify_all();
      } else {
        CountDropped();
      }
    }

    autoruntime_dds_RpcResponse_free(
        sample, DDS_FREE_ALL);
  }

  void ReceiveRequests(
      const std::shared_ptr<ServiceEndpoint>& service,
      std::stop_token stop_token) {
    auto* sample = autoruntime_dds_RpcRequest__alloc();
    if (sample == nullptr) {
      CountDropped();
      return;
    }
    void* samples[1]{sample};
    dds_sample_info_t information[1]{};

    while (!stop_token.stop_requested()) {
      const auto count = dds_take(
          service->request_reader, samples, information,
          1U, 1U);
      if (count < 0) {
        if (count == DDS_RETCODE_ALREADY_DELETED) {
          break;
        }
        CountDropped();
        std::this_thread::sleep_for(
            config.receive_poll_interval);
        continue;
      }
      if (count == 0) {
        std::this_thread::sleep_for(
            config.receive_poll_interval);
        continue;
      }
      if (!information[0].valid_data) {
        CountDropped();
        continue;
      }

      const auto request_service =
          ReadBoundedString(sample->service_name);
      if (!request_service ||
          *request_service != service->service_name) {
        continue;
      }
      if (sample->protocol_version !=
              kRpcProtocolVersion ||
          (sample->client_guid_high == 0U &&
           sample->client_guid_low == 0U) ||
          sample->request_id == 0U ||
          sample->timeout_budget_ns == 0U ||
          !ValidWireMessage(
              sample->message, config.max_payload_size)) {
        CountDropped();
        continue;
      }

      auto request = DecodeMessage(sample->message);
      const auto handler_deadline =
          DeadlineFromBudget(sample->timeout_budget_ns);
      Result<Message> response =
          Status(StatusCode::Timeout,
                 "DDS RPC request expired before dispatch");
      if (!handler_deadline.expired()) {
        try {
          response = service->callback(
              std::move(request), handler_deadline);
        } catch (...) {
          response = Status(
              StatusCode::Internal,
              "DDS RPC service callback threw");
        }
      }

      autoruntime_dds_RpcResponse wire{};
      wire.protocol_version = kRpcProtocolVersion;
      static_cast<void>(CopyBoundedString(
          wire.service_name, service->service_name));
      wire.client_guid_high = sample->client_guid_high;
      wire.client_guid_low = sample->client_guid_low;
      wire.request_id = sample->request_id;

      StatusCode response_code = StatusCode::Ok;
      std::string detail;
      std::optional<Message> response_message;
      if (response) {
        auto decoded = std::move(response).take_value();
        if (decoded.envelope.version !=
                kMessageEnvelopeVersion ||
            decoded.payload.size() >
                config.max_payload_size) {
          response_code = StatusCode::Internal;
          detail =
              "DDS RPC service returned an invalid response";
        } else {
          response_message.emplace(std::move(decoded));
        }
      } else {
        response_code = response.status().code();
        detail = response.status().detail();
      }
      if (detail.size() > kMaximumErrorDetailBytes) {
        detail.resize(kMaximumErrorDetailBytes);
      }
      wire.status_code =
          static_cast<std::uint16_t>(response_code);
      static_cast<void>(CopyBoundedString(
          wire.error_detail, detail));
      if (response_message) {
        EncodeMessage(*response_message, &wire.message);
      } else {
        CountFailure();
      }

      const auto status =
          dds_write(service->response_writer, &wire);
      if (status < 0) {
        CountFailure();
      }
    }

    autoruntime_dds_RpcRequest_free(
        sample, DDS_FREE_ALL);
  }

  [[nodiscard]] Status CloseClient(
      const std::shared_ptr<ClientChannel>& channel) {
    if (channel->closed.exchange(
            true, std::memory_order_acq_rel)) {
      return Status::Ok();
    }
    channel->receiver.request_stop();
    {
      std::lock_guard lock(channel->state_mutex);
      for (auto& [request_id, pending] :
           channel->pending) {
        static_cast<void>(request_id);
        if (!pending->completed) {
          pending->result.emplace(
              Status(StatusCode::Closed,
                     "DDS RPC client channel closed"));
          pending->completed = true;
        }
      }
      channel->pending.clear();
    }
    channel->state_changed.notify_all();
    {
      std::lock_guard lock(channel->write_mutex);
    }
    if (channel->receiver.joinable()) {
      channel->receiver.join();
    }

    Status result = Status::Ok();
    MergeStatus(
        DdsError(
            dds_delete(channel->response_reader),
            "dds_delete(RPC response reader)"),
        &result);
    MergeStatus(
        DdsError(
            dds_delete(channel->request_writer),
            "dds_delete(RPC request writer)"),
        &result);
    return result;
  }

  [[nodiscard]] Status CloseService(
      const std::shared_ptr<ServiceEndpoint>& service) {
    if (service->closed.exchange(
            true, std::memory_order_acq_rel)) {
      return Status::Ok();
    }
    service->receiver.request_stop();
    if (service->receiver.joinable()) {
      service->receiver.join();
    }

    Status result = Status::Ok();
    MergeStatus(
        DdsError(
            dds_delete(service->request_reader),
            "dds_delete(RPC request reader)"),
        &result);
    MergeStatus(
        DdsError(
            dds_delete(service->response_writer),
            "dds_delete(RPC response writer)"),
        &result);
    return result;
  }
};

DdsRpcEngine::DdsRpcEngine(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DdsRpcEngine::~DdsRpcEngine() {
  static_cast<void>(Close());
}

Result<std::unique_ptr<DdsRpcEngine>>
DdsRpcEngine::Create(
    dds_entity_t participant,
    const DdsTransportConfig& config,
    DdsRpcStatsHooks hooks) {
  if (participant <= 0) {
    return Status(
        StatusCode::InvalidArgument,
        "DDS RPC requires a participant");
  }
  dds_guid_t guid{};
  const auto guid_status =
      dds_get_guid(participant, &guid);
  if (guid_status < 0) {
    return DdsError(
        guid_status, "dds_get_guid(RPC client)");
  }

  auto impl = std::make_unique<Impl>();
  impl->participant = participant;
  impl->config = config;
  impl->hooks = std::move(hooks);
  impl->client_guid_high = GuidHalf(guid, 0U);
  impl->client_guid_low = GuidHalf(guid, 8U);
  return std::unique_ptr<DdsRpcEngine>(
      new DdsRpcEngine(std::move(impl)));
}

Result<ServiceId> DdsRpcEngine::AdvertiseService(
    std::string_view service_name,
    TransportServiceCallback callback) {
  if (!ValidServiceName(service_name) || !callback) {
    return Status(
        StatusCode::InvalidArgument,
        "DDS RPC service requires a 1-128 byte name and callback");
  }

  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    return Status(
        StatusCode::Closed, "DDS RPC engine is closed");
  }
  const std::string key(service_name);
  if (impl_->service_names.contains(key)) {
    return Status(
        StatusCode::AlreadyExists,
        "DDS RPC service name is already advertised");
  }

  const auto id = impl_->next_service_id++;
  auto service = impl_->CreateServiceLocked(
      key, id, std::move(callback));
  if (!service) {
    return service.status();
  }
  impl_->service_names.emplace(key, id);
  impl_->services.emplace(id, service.value());
  return id;
}

Status DdsRpcEngine::RemoveService(
    ServiceId service_id) {
  std::shared_ptr<Impl::ServiceEndpoint> service;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      return Status(
          StatusCode::Closed, "DDS RPC engine is closed");
    }
    const auto iterator =
        impl_->services.find(service_id);
    if (iterator == impl_->services.end()) {
      return Status(
          StatusCode::NotFound,
          "DDS RPC service was not found");
    }
    service = iterator->second;
    impl_->service_names.erase(service->service_name);
    impl_->services.erase(iterator);
  }
  return impl_->CloseService(service);
}

Result<Message> DdsRpcEngine::Request(
    std::string_view service_name, Message request,
    Deadline deadline) {
  if (!ValidServiceName(service_name) ||
      request.envelope.version !=
          kMessageEnvelopeVersion ||
      request.payload.size() >
          impl_->config.max_payload_size) {
    impl_->CountFailure();
    return Status(
        StatusCode::InvalidArgument,
        "invalid DDS RPC service, envelope, or payload");
  }
  if (deadline.expired()) {
    impl_->CountFailure();
    return Status(
        StatusCode::Timeout,
        "DDS RPC deadline already expired");
  }

  std::shared_ptr<Impl::ClientChannel> channel;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      impl_->CountFailure();
      return Status(
          StatusCode::Closed, "DDS RPC engine is closed");
    }
    auto channel_result =
        impl_->ClientLocked(service_name);
    if (!channel_result) {
      impl_->CountFailure();
      return channel_result.status();
    }
    channel = std::move(channel_result).take_value();
  }

  auto request_id = impl_->next_request_id.fetch_add(
      1U, std::memory_order_relaxed);
  if (request_id == 0U) {
    request_id = impl_->next_request_id.fetch_add(
        1U, std::memory_order_relaxed);
  }
  auto pending =
      std::make_shared<Impl::PendingRequest>();
  {
    std::lock_guard lock(channel->state_mutex);
    if (channel->closed.load(std::memory_order_acquire)) {
      impl_->CountFailure();
      return Status(
          StatusCode::Closed,
          "DDS RPC client channel is closed");
    }
    const auto [iterator, inserted] =
        channel->pending.emplace(request_id, pending);
    static_cast<void>(iterator);
    if (!inserted) {
      impl_->CountFailure();
      return Status(StatusCode::Internal,
                    "DDS RPC request id collided");
    }
  }

  const auto remove_pending = [&] {
    std::lock_guard lock(channel->state_mutex);
    const auto iterator =
        channel->pending.find(request_id);
    if (iterator != channel->pending.end() &&
        iterator->second == pending) {
      channel->pending.erase(iterator);
    }
  };

  const auto budget = RemainingBudget(deadline);
  if (budget == 0U) {
    remove_pending();
    impl_->CountFailure();
    return Status(
        StatusCode::Timeout,
        "DDS RPC deadline expired before write");
  }

  autoruntime_dds_RpcRequest wire{};
  wire.protocol_version = kRpcProtocolVersion;
  static_cast<void>(CopyBoundedString(
      wire.service_name, service_name));
  wire.client_guid_high = impl_->client_guid_high;
  wire.client_guid_low = impl_->client_guid_low;
  wire.request_id = request_id;
  wire.timeout_budget_ns = budget;
  EncodeMessage(request, &wire.message);

  dds_return_t write_status = DDS_RETCODE_OK;
  {
    std::lock_guard lock(channel->write_mutex);
    if (channel->closed.load(std::memory_order_acquire)) {
      remove_pending();
      impl_->CountFailure();
      return Status(
          StatusCode::Closed,
          "DDS RPC client channel is closed");
    }
    write_status =
        dds_write(channel->request_writer, &wire);
  }
  if (write_status < 0) {
    remove_pending();
    impl_->CountFailure();
    return DdsError(
        write_status, "dds_write(RPC request)");
  }
  impl_->CountRequest();

  std::unique_lock lock(channel->state_mutex);
  const auto completed = [&] {
    return pending->completed;
  };
  if (deadline.infinite()) {
    channel->state_changed.wait(lock, completed);
  } else if (!channel->state_changed.wait_until(
                 lock, deadline.time_point(), completed)) {
    const auto iterator =
        channel->pending.find(request_id);
    if (iterator != channel->pending.end() &&
        iterator->second == pending) {
      channel->pending.erase(iterator);
    }
    lock.unlock();
    impl_->CountFailure();
    return Status(
        StatusCode::Timeout,
        "DDS RPC response deadline expired");
  }

  auto result = std::move(pending->result.value());
  lock.unlock();
  if (!result) {
    impl_->CountFailure();
  }
  return result;
}

Status DdsRpcEngine::Close() {
  std::vector<std::shared_ptr<Impl::ClientChannel>>
      clients;
  std::vector<std::shared_ptr<Impl::ServiceEndpoint>>
      services;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      return Status::Ok();
    }
    impl_->closed = true;
    clients.reserve(impl_->clients.size());
    for (auto& [name, client] : impl_->clients) {
      static_cast<void>(name);
      clients.push_back(std::move(client));
    }
    services.reserve(impl_->services.size());
    for (auto& [id, service] : impl_->services) {
      static_cast<void>(id);
      services.push_back(std::move(service));
    }
    impl_->clients.clear();
    impl_->services.clear();
    impl_->service_names.clear();
    impl_->request_topics.clear();
    impl_->response_topics.clear();
  }

  Status result = Status::Ok();
  for (const auto& client : clients) {
    MergeStatus(
        impl_->CloseClient(client), &result);
  }
  for (const auto& service : services) {
    MergeStatus(
        impl_->CloseService(service), &result);
  }
  return result;
}

}  // namespace autoruntime::detail
