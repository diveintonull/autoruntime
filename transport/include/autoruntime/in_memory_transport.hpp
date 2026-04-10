#pragma once

#include <autoruntime/transport.hpp>

#include <memory>

namespace autoruntime {

class InMemoryTransport final : public Transport {
 public:
  InMemoryTransport();
  ~InMemoryTransport() override;

  InMemoryTransport(const InMemoryTransport&) = delete;
  InMemoryTransport& operator=(const InMemoryTransport&) = delete;
  InMemoryTransport(InMemoryTransport&&) = delete;
  InMemoryTransport& operator=(InMemoryTransport&&) = delete;

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
  Status Close() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime
