#pragma once

#include <autoruntime/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace autoruntime {

struct DdsTransportConfig {
  std::uint32_t domain_id{0U};
  std::string participant_name{"autoruntime"};
  std::chrono::milliseconds receive_poll_interval{2};
  std::chrono::milliseconds reliability_max_blocking_time{100};
  std::size_t max_payload_size{1024U * 1024U};
};

class DdsTransport final : public Transport {
 public:
  [[nodiscard]] static Result<std::shared_ptr<DdsTransport>> Create(
      DdsTransportConfig config);

  ~DdsTransport() override;

  DdsTransport(const DdsTransport&) = delete;
  DdsTransport& operator=(const DdsTransport&) = delete;
  DdsTransport(DdsTransport&&) = delete;
  DdsTransport& operator=(DdsTransport&&) = delete;

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
  explicit DdsTransport(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace autoruntime
