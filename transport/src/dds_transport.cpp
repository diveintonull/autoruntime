#include <autoruntime/dds_transport.hpp>

#include "autoruntime_message.h"
#include "dds_rpc_engine.hpp"

#include <dds/dds.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autoruntime {
namespace {

constexpr std::size_t kIdlMaximumPayload = 4U * 1024U * 1024U;

struct QosDeleter {
  void operator()(dds_qos_t* qos) const noexcept {
    if (qos != nullptr) {
      dds_delete_qos(qos);
    }
  }
};
using QosPtr = std::unique_ptr<dds_qos_t, QosDeleter>;

[[nodiscard]] Status DdsError(dds_return_t result,
                              std::string_view operation) {
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

[[nodiscard]] bool ValidQos(const QosProfile& qos) {
  return qos.depth != 0U &&
         qos.depth <=
             static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) &&
         qos.deadline >= std::chrono::nanoseconds::zero() &&
         qos.liveliness_lease >= std::chrono::nanoseconds::zero();
}

[[nodiscard]] bool SameQos(const QosProfile& left,
                           const QosProfile& right) {
  return left.reliability == right.reliability &&
         left.history == right.history && left.depth == right.depth &&
         left.deadline == right.deadline &&
         left.liveliness == right.liveliness &&
         left.liveliness_lease == right.liveliness_lease;
}

[[nodiscard]] dds_duration_t Duration(
    std::chrono::nanoseconds value) noexcept {
  return static_cast<dds_duration_t>(value.count());
}

[[nodiscard]] QosPtr EndpointQos(
    const QosProfile& profile,
    std::chrono::milliseconds reliability_max_blocking_time) {
  QosPtr qos(dds_create_qos());
  if (!qos) {
    return qos;
  }

  const auto reliability =
      profile.reliability == Reliability::Reliable
          ? DDS_RELIABILITY_RELIABLE
          : DDS_RELIABILITY_BEST_EFFORT;
  const auto blocking_time =
      profile.reliability == Reliability::Reliable
          ? Duration(reliability_max_blocking_time)
          : static_cast<dds_duration_t>(0);
  dds_qset_reliability(qos.get(), reliability, blocking_time);

  const auto history =
      profile.history == HistoryKind::KeepLast
          ? DDS_HISTORY_KEEP_LAST
          : DDS_HISTORY_KEEP_ALL;
  const auto depth =
      profile.history == HistoryKind::KeepLast
          ? static_cast<std::int32_t>(profile.depth)
          : 0;
  dds_qset_history(qos.get(), history, depth);

  if (profile.deadline > std::chrono::nanoseconds::zero()) {
    dds_qset_deadline(qos.get(), Duration(profile.deadline));
  }

  const auto liveliness =
      profile.liveliness == LivelinessKind::Automatic
          ? DDS_LIVELINESS_AUTOMATIC
          : DDS_LIVELINESS_MANUAL_BY_TOPIC;
  const auto lease =
      profile.liveliness_lease > std::chrono::nanoseconds::zero()
          ? Duration(profile.liveliness_lease)
          : static_cast<dds_duration_t>(DDS_INFINITY);
  dds_qset_liveliness(qos.get(), liveliness, lease);
  return qos;
}

[[nodiscard]] Message Decode(const autoruntime_dds_Message& sample) {
  Message message;
  message.envelope.version = sample.envelope_version;
  message.envelope.trace_id = sample.trace_id;
  message.envelope.span_id = sample.span_id;
  message.envelope.parent_span_id = sample.parent_span_id;
  message.envelope.sequence = sample.message_sequence;
  message.envelope.source_timestamp_ns = sample.source_timestamp_ns;
  message.envelope.publish_timestamp_ns = sample.publish_timestamp_ns;
  message.envelope.source_generation = sample.source_generation;
  message.envelope.priority = sample.priority;
  if (sample.payload._length != 0U) {
    const auto* begin =
        reinterpret_cast<const std::byte*>(sample.payload._buffer);
    message.payload.assign(begin, begin + sample.payload._length);
  }
  return message;
}

}  // namespace

struct DdsTransport::Impl {
  struct Writer {
    dds_entity_t entity{0};
    QosProfile qos;
  };

  struct Subscription {
    SubscriptionId id{0U};
    dds_entity_t reader{0};
    TransportMessageCallback callback;
    std::jthread receiver;
  };

  DdsTransportConfig config;
  dds_entity_t participant{0};
  mutable std::mutex mutex;
  bool closed{false};
  SubscriptionId next_subscription_id{1U};
  std::unordered_map<std::string, dds_entity_t> topics;
  std::unordered_map<std::string, Writer> writers;
  std::unordered_map<SubscriptionId, std::shared_ptr<Subscription>>
      subscriptions;
  mutable std::mutex stats_mutex;
  TransportStats stats;
  std::unique_ptr<detail::DdsRpcEngine> rpc;

  void CountPublished() {
    std::lock_guard lock(stats_mutex);
    ++stats.published_messages;
  }

  void CountPublishFailure() {
    std::lock_guard lock(stats_mutex);
    ++stats.publish_failures;
  }

  void CountDelivered() {
    std::lock_guard lock(stats_mutex);
    ++stats.delivered_messages;
  }

  void CountDropped() {
    std::lock_guard lock(stats_mutex);
    ++stats.dropped_messages;
  }

  void CountRpcRequest() {
    std::lock_guard lock(stats_mutex);
    ++stats.rpc_requests;
  }

  void CountRpcFailure() {
    std::lock_guard lock(stats_mutex);
    ++stats.rpc_failures;
  }

  [[nodiscard]] Result<dds_entity_t> TopicLocked(
      std::string_view topic_name) {
    const std::string key(topic_name);
    const auto existing = topics.find(key);
    if (existing != topics.end()) {
      return existing->second;
    }

    const auto topic = dds_create_topic(
        participant, &autoruntime_dds_Message_desc, key.c_str(), nullptr,
        nullptr);
    if (topic < 0) {
      return DdsError(topic, "dds_create_topic");
    }
    topics.emplace(key, topic);
    return topic;
  }

  [[nodiscard]] Result<Writer*> WriterLocked(
      std::string_view topic_name, const QosProfile& qos) {
    const std::string key(topic_name);
    const auto existing = writers.find(key);
    if (existing != writers.end()) {
      if (!SameQos(existing->second.qos, qos)) {
        return Status(
            StatusCode::InvalidArgument,
            "a DDS writer already exists with different immutable QoS");
      }
      return &existing->second;
    }

    auto topic = TopicLocked(topic_name);
    if (!topic) {
      return topic.status();
    }
    auto endpoint_qos =
        EndpointQos(qos, config.reliability_max_blocking_time);
    if (!endpoint_qos) {
      return Status(StatusCode::Internal,
                    "dds_create_qos failed for writer");
    }
    const auto writer = dds_create_writer(
        participant, topic.value(), endpoint_qos.get(), nullptr);
    if (writer < 0) {
      return DdsError(writer, "dds_create_writer");
    }

    auto [iterator, inserted] =
        writers.emplace(key, Writer{writer, qos});
    if (!inserted) {
      static_cast<void>(dds_delete(writer));
      return Status(StatusCode::Internal,
                    "DDS writer insertion raced unexpectedly");
    }
    return &iterator->second;
  }

  void ReceiveLoop(const std::shared_ptr<Subscription>& subscription,
                   std::stop_token stop_token) {
    auto* sample = autoruntime_dds_Message__alloc();
    if (sample == nullptr) {
      CountDropped();
      return;
    }
    void* samples[1]{sample};
    dds_sample_info_t information[1]{};

    while (!stop_token.stop_requested()) {
      const auto count =
          dds_take(subscription->reader, samples, information, 1U, 1U);
      if (count < 0) {
        if (count == DDS_RETCODE_ALREADY_DELETED) {
          break;
        }
        CountDropped();
        std::this_thread::sleep_for(config.receive_poll_interval);
        continue;
      }
      if (count == 0) {
        std::this_thread::sleep_for(config.receive_poll_interval);
        continue;
      }
      if (!information[0].valid_data ||
          sample->envelope_version != kMessageEnvelopeVersion ||
          sample->payload._length > config.max_payload_size ||
          (sample->payload._length != 0U &&
           sample->payload._buffer == nullptr)) {
        CountDropped();
        continue;
      }

      try {
        subscription->callback(Decode(*sample));
        CountDelivered();
      } catch (...) {
        CountDropped();
      }
    }

    autoruntime_dds_Message_free(sample, DDS_FREE_ALL);
  }
};

DdsTransport::DdsTransport(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DdsTransport::~DdsTransport() {
  static_cast<void>(Close());
}

Result<std::shared_ptr<DdsTransport>> DdsTransport::Create(
    DdsTransportConfig config) {
  if (config.participant_name.empty() ||
      config.receive_poll_interval <= std::chrono::milliseconds::zero() ||
      config.reliability_max_blocking_time <=
          std::chrono::milliseconds::zero() ||
      config.max_payload_size == 0U ||
      config.max_payload_size > kIdlMaximumPayload) {
    return Status(StatusCode::InvalidArgument,
                  "invalid DDS transport configuration");
  }

  QosPtr qos(dds_create_qos());
  if (!qos) {
    return Status(StatusCode::Internal,
                  "dds_create_qos failed for participant");
  }
  dds_qset_entity_name(qos.get(), config.participant_name.c_str());
  const auto participant = dds_create_participant(
      static_cast<dds_domainid_t>(config.domain_id), qos.get(), nullptr);
  if (participant < 0) {
    return DdsError(participant, "dds_create_participant");
  }

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->participant = participant;
  auto rpc_result = detail::DdsRpcEngine::Create(
      participant, impl->config,
      detail::DdsRpcStatsHooks{
          [state = impl.get()] { state->CountRpcRequest(); },
          [state = impl.get()] { state->CountRpcFailure(); },
          [state = impl.get()] { state->CountDropped(); }});
  if (!rpc_result) {
    static_cast<void>(dds_delete(participant));
    return rpc_result.status();
  }
  impl->rpc = std::move(rpc_result).take_value();
  return std::shared_ptr<DdsTransport>(
      new DdsTransport(std::move(impl)));
}

Status DdsTransport::Publish(std::string_view topic, Message message,
                             const QosProfile& qos) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    impl_->CountPublishFailure();
    return Status(StatusCode::Closed, "DDS transport is closed");
  }
  if (topic.empty() || !ValidQos(qos) ||
      message.envelope.version != kMessageEnvelopeVersion ||
      message.payload.size() > impl_->config.max_payload_size) {
    impl_->CountPublishFailure();
    return Status(StatusCode::InvalidArgument,
                  "invalid DDS topic, QoS, envelope, or payload");
  }

  auto writer = impl_->WriterLocked(topic, qos);
  if (!writer) {
    impl_->CountPublishFailure();
    return writer.status();
  }

  autoruntime_dds_Message sample{};
  sample.envelope_version = message.envelope.version;
  sample.trace_id = message.envelope.trace_id;
  sample.span_id = message.envelope.span_id;
  sample.parent_span_id = message.envelope.parent_span_id;
  sample.message_sequence = message.envelope.sequence;
  sample.source_timestamp_ns = message.envelope.source_timestamp_ns;
  sample.publish_timestamp_ns = message.envelope.publish_timestamp_ns;
  sample.source_generation = message.envelope.source_generation;
  sample.priority = message.envelope.priority;
  sample.payload._maximum =
      static_cast<std::uint32_t>(message.payload.size());
  sample.payload._length =
      static_cast<std::uint32_t>(message.payload.size());
  sample.payload._buffer =
      reinterpret_cast<std::uint8_t*>(message.payload.data());
  sample.payload._release = false;

  if (qos.liveliness == LivelinessKind::ManualByTopic) {
    const auto status = dds_assert_liveliness(writer.value()->entity);
    if (status < 0) {
      impl_->CountPublishFailure();
      return DdsError(status, "dds_assert_liveliness");
    }
  }
  const auto status = dds_write(writer.value()->entity, &sample);
  if (status < 0) {
    impl_->CountPublishFailure();
    return DdsError(status, "dds_write");
  }
  impl_->CountPublished();
  return Status::Ok();
}

Result<SubscriptionId> DdsTransport::Subscribe(
    std::string_view topic, const QosProfile& qos,
    TransportMessageCallback callback) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    return Status(StatusCode::Closed, "DDS transport is closed");
  }
  if (topic.empty() || !ValidQos(qos) || !callback) {
    return Status(StatusCode::InvalidArgument,
                  "DDS subscription requires topic, QoS, and callback");
  }

  auto topic_entity = impl_->TopicLocked(topic);
  if (!topic_entity) {
    return topic_entity.status();
  }
  auto endpoint_qos =
      EndpointQos(qos, impl_->config.reliability_max_blocking_time);
  if (!endpoint_qos) {
    return Status(StatusCode::Internal,
                  "dds_create_qos failed for reader");
  }
  const auto reader = dds_create_reader(
      impl_->participant, topic_entity.value(), endpoint_qos.get(), nullptr);
  if (reader < 0) {
    return DdsError(reader, "dds_create_reader");
  }

  auto subscription = std::make_shared<Impl::Subscription>();
  subscription->id = impl_->next_subscription_id++;
  subscription->reader = reader;
  subscription->callback = std::move(callback);
  impl_->subscriptions.emplace(subscription->id, subscription);
  try {
    subscription->receiver = std::jthread(
        [implementation = impl_.get(), subscription](
            std::stop_token stop_token) {
          implementation->ReceiveLoop(subscription, stop_token);
        });
  } catch (...) {
    impl_->subscriptions.erase(subscription->id);
    static_cast<void>(dds_delete(reader));
    return Status(StatusCode::Internal,
                  "failed to start DDS receive thread");
  }
  return subscription->id;
}

Status DdsTransport::Unsubscribe(SubscriptionId subscription_id) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    return Status(StatusCode::Closed, "DDS transport is closed");
  }
  const auto iterator = impl_->subscriptions.find(subscription_id);
  if (iterator == impl_->subscriptions.end()) {
    return Status(StatusCode::NotFound,
                  "DDS subscription was not found");
  }
  auto subscription = iterator->second;
  impl_->subscriptions.erase(iterator);
  subscription->receiver.request_stop();
  if (subscription->receiver.joinable()) {
    subscription->receiver.join();
  }
  return DdsError(dds_delete(subscription->reader), "dds_delete(reader)");
}

Result<ServiceId> DdsTransport::AdvertiseService(
    std::string_view service_name,
    TransportServiceCallback callback) {
  return impl_->rpc->AdvertiseService(
      service_name, std::move(callback));
}

Status DdsTransport::RemoveService(ServiceId service_id) {
  return impl_->rpc->RemoveService(service_id);
}

Result<Message> DdsTransport::Request(
    std::string_view service_name, Message request, Deadline deadline) {
  return impl_->rpc->Request(
      service_name, std::move(request), deadline);
}

TransportStats DdsTransport::Stats() const {
  std::lock_guard lock(impl_->stats_mutex);
  return impl_->stats;
}

Status DdsTransport::Close() {
  std::vector<std::shared_ptr<Impl::Subscription>> subscriptions;
  dds_entity_t participant = 0;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      return Status::Ok();
    }
    impl_->closed = true;

    subscriptions.reserve(impl_->subscriptions.size());
    for (auto& [id, subscription] : impl_->subscriptions) {
      static_cast<void>(id);
      subscriptions.push_back(std::move(subscription));
    }
    impl_->subscriptions.clear();
    impl_->writers.clear();
    impl_->topics.clear();
    participant = impl_->participant;
    impl_->participant = 0;
  }

  const auto rpc_status = impl_->rpc->Close();

  for (auto& subscription : subscriptions) {
    subscription->receiver.request_stop();
  }
  for (auto& subscription : subscriptions) {
    if (subscription->receiver.joinable()) {
      subscription->receiver.join();
    }
  }
  const auto participant_status =
      DdsError(dds_delete(participant), "dds_delete(participant)");
  if (!rpc_status) {
    return rpc_status;
  }
  return participant_status;
}

}  // namespace autoruntime
