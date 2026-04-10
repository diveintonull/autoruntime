#include <autoruntime/in_memory_transport.hpp>

#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autoruntime {
namespace {

struct SubscriptionEntry {
  SubscriptionId id{0U};
  std::string topic;
  QosProfile qos;
  TransportMessageCallback callback;
};

struct ServiceEntry {
  ServiceId id{0U};
  std::string name;
  TransportServiceCallback callback;
};

}  // namespace

struct InMemoryTransport::Impl {
  mutable std::mutex mutex;
  bool closed{false};
  SubscriptionId next_subscription_id{1U};
  ServiceId next_service_id{1U};
  std::unordered_map<SubscriptionId, SubscriptionEntry> subscriptions;
  std::unordered_map<ServiceId, ServiceEntry> services;
  std::unordered_map<std::string, ServiceId> service_names;
  TransportStats stats;
};

InMemoryTransport::InMemoryTransport() : impl_(std::make_unique<Impl>()) {}

InMemoryTransport::~InMemoryTransport() {
  static_cast<void>(Close());
}

Status InMemoryTransport::Publish(std::string_view topic, Message message,
                                  const QosProfile& qos) {
  static_cast<void>(qos);
  if (topic.empty()) {
    return Status(StatusCode::InvalidArgument, "topic must not be empty");
  }

  std::vector<TransportMessageCallback> callbacks;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      ++impl_->stats.publish_failures;
      return Status(StatusCode::Closed, "transport is closed");
    }
    ++impl_->stats.published_messages;
    for (const auto& [unused_id, entry] : impl_->subscriptions) {
      static_cast<void>(unused_id);
      if (entry.topic == topic) {
        callbacks.push_back(entry.callback);
      }
    }
  }

  std::uint64_t delivered = 0U;
  std::uint64_t failed = 0U;
  for (const auto& callback : callbacks) {
    try {
      callback(message);
      ++delivered;
    } catch (...) {
      ++failed;
    }
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stats.delivered_messages += delivered;
    impl_->stats.dropped_messages += failed;
  }
  if (failed != 0U) {
    return Status(StatusCode::Internal,
                  "one or more in-memory subscribers threw");
  }
  return Status::Ok();
}

Result<SubscriptionId> InMemoryTransport::Subscribe(
    std::string_view topic, const QosProfile& qos,
    TransportMessageCallback callback) {
  if (topic.empty() || !callback || qos.depth == 0U) {
    return Status(StatusCode::InvalidArgument,
                  "subscription requires topic, callback, and depth");
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    return Status(StatusCode::Closed, "transport is closed");
  }
  const auto id = impl_->next_subscription_id++;
  impl_->subscriptions.emplace(
      id, SubscriptionEntry{id, std::string(topic), qos, std::move(callback)});
  return id;
}

Status InMemoryTransport::Unsubscribe(SubscriptionId subscription_id) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->subscriptions.erase(subscription_id) == 0U) {
    return Status(StatusCode::NotFound, "subscription does not exist");
  }
  return Status::Ok();
}

Result<ServiceId> InMemoryTransport::AdvertiseService(
    std::string_view service_name, TransportServiceCallback callback) {
  if (service_name.empty() || !callback) {
    return Status(StatusCode::InvalidArgument,
                  "service requires name and callback");
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    return Status(StatusCode::Closed, "transport is closed");
  }
  if (impl_->service_names.contains(std::string(service_name))) {
    return Status(StatusCode::AlreadyExists,
                  "service name is already advertised");
  }
  const auto id = impl_->next_service_id++;
  auto name = std::string(service_name);
  impl_->service_names.emplace(name, id);
  impl_->services.emplace(
      id, ServiceEntry{id, std::move(name), std::move(callback)});
  return id;
}

Status InMemoryTransport::RemoveService(ServiceId service_id) {
  std::lock_guard lock(impl_->mutex);
  const auto iterator = impl_->services.find(service_id);
  if (iterator == impl_->services.end()) {
    return Status(StatusCode::NotFound, "service does not exist");
  }
  impl_->service_names.erase(iterator->second.name);
  impl_->services.erase(iterator);
  return Status::Ok();
}

Result<Message> InMemoryTransport::Request(
    std::string_view service_name, Message request, Deadline deadline) {
  if (deadline.expired()) {
    return Status(StatusCode::Timeout, "request deadline already expired");
  }

  TransportServiceCallback callback;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) {
      ++impl_->stats.rpc_failures;
      return Status(StatusCode::Closed, "transport is closed");
    }
    const auto name_iterator =
        impl_->service_names.find(std::string(service_name));
    if (name_iterator == impl_->service_names.end()) {
      ++impl_->stats.rpc_failures;
      return Status(StatusCode::NotFound, "service is not advertised");
    }
    callback = impl_->services.at(name_iterator->second).callback;
    ++impl_->stats.rpc_requests;
  }

  try {
    auto result = callback(std::move(request), deadline);
    if (!result) {
      std::lock_guard lock(impl_->mutex);
      ++impl_->stats.rpc_failures;
    }
    return result;
  } catch (const std::exception&) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->stats.rpc_failures;
    return Status(StatusCode::Internal, "service callback threw");
  } catch (...) {
    std::lock_guard lock(impl_->mutex);
    ++impl_->stats.rpc_failures;
    return Status(StatusCode::Internal, "service callback threw");
  }
}

TransportStats InMemoryTransport::Stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}

Status InMemoryTransport::Close() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) {
    return Status::Ok();
  }
  impl_->closed = true;
  impl_->subscriptions.clear();
  impl_->services.clear();
  impl_->service_names.clear();
  return Status::Ok();
}

}  // namespace autoruntime
