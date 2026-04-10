#pragma once

#include <autoruntime/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace autoruntime {

enum class FastIpcDirection {
  Publish,
  Subscribe,
};

struct FastIpcEndpointConfig {
  std::string topic;
  std::string channel_name;
  FastIpcDirection direction{FastIpcDirection::Publish};
  std::uint32_t slot_count{64U};
  std::uint32_t max_payload_size{4096U};
  std::chrono::milliseconds peer_timeout{1000};
  bool unlink_on_owner_close{false};
};

struct FastIpcTransportConfig {
  std::vector<FastIpcEndpointConfig> endpoints;
  std::chrono::milliseconds open_timeout{1000};
};

class FastIpcTransport final : public Transport {
 public:
  [[nodiscard]] static Result<std::shared_ptr<FastIpcTransport>> Create(
      FastIpcTransportConfig config);

  ~FastIpcTransport() override;

  FastIpcTransport(const FastIpcTransport&) = delete;
  FastIpcTransport& operator=(const FastIpcTransport&) = delete;
  FastIpcTransport(FastIpcTransport&&) = delete;
  FastIpcTransport& operator=(FastIpcTransport&&) = delete;

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
  explicit FastIpcTransport(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime
