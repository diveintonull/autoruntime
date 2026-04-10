#pragma once

#include <autoruntime/deadline.hpp>
#include <autoruntime/message.hpp>
#include <autoruntime/status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace autoruntime {

using SubscriptionId = std::uint64_t;
using ServiceId = std::uint64_t;

enum class Reliability {
  Reliable,
  BestEffort,
};

enum class HistoryKind {
  KeepLast,
  KeepAll,
};

enum class LivelinessKind {
  Automatic,
  ManualByTopic,
};

struct QosProfile {
  Reliability reliability{Reliability::Reliable};
  HistoryKind history{HistoryKind::KeepLast};
  std::size_t depth{16U};
  std::chrono::nanoseconds deadline{0};
  LivelinessKind liveliness{LivelinessKind::Automatic};
  std::chrono::nanoseconds liveliness_lease{0};
};

struct TransportStats {
  std::uint64_t published_messages{0U};
  std::uint64_t delivered_messages{0U};
  std::uint64_t dropped_messages{0U};
  std::uint64_t publish_failures{0U};
  std::uint64_t rpc_requests{0U};
  std::uint64_t rpc_failures{0U};
};

using TransportMessageCallback = std::function<void(Message)>;
using TransportServiceCallback =
    std::function<Result<Message>(Message, Deadline)>;

class Transport {
 public:
  virtual ~Transport() = default;

  virtual Status Publish(std::string_view topic, Message message,
                         const QosProfile& qos) = 0;
  [[nodiscard]] virtual Result<SubscriptionId> Subscribe(
      std::string_view topic, const QosProfile& qos,
      TransportMessageCallback callback) = 0;
  virtual Status Unsubscribe(SubscriptionId subscription_id) = 0;

  [[nodiscard]] virtual Result<ServiceId> AdvertiseService(
      std::string_view service_name, TransportServiceCallback callback) = 0;
  virtual Status RemoveService(ServiceId service_id) = 0;
  [[nodiscard]] virtual Result<Message> Request(
      std::string_view service_name, Message request, Deadline deadline) = 0;

  [[nodiscard]] virtual TransportStats Stats() const = 0;
  virtual Status Close() = 0;
};

}  // namespace autoruntime
