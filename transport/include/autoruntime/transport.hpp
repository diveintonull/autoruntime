#pragma once

#include <autoruntime/deadline.hpp>
#include <autoruntime/message.hpp>
#include <autoruntime/status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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

class TransportPublisherLoan {
 public:
  class Backend {
   public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual std::span<std::byte> Data() noexcept = 0;
    virtual Status Publish(const MessageEnvelope& envelope) noexcept = 0;
    virtual Status Abandon() noexcept = 0;
  };

  TransportPublisherLoan() = default;
  explicit TransportPublisherLoan(
      std::unique_ptr<Backend> backend) noexcept
      : backend_(std::move(backend)) {}
  ~TransportPublisherLoan() { static_cast<void>(Abandon()); }

  TransportPublisherLoan(const TransportPublisherLoan&) = delete;
  TransportPublisherLoan& operator=(const TransportPublisherLoan&) = delete;
  TransportPublisherLoan(TransportPublisherLoan&&) noexcept = default;
  TransportPublisherLoan& operator=(
      TransportPublisherLoan&& other) noexcept {
    if (this != &other) {
      static_cast<void>(Abandon());
      backend_ = std::move(other.backend_);
    }
    return *this;
  }

  [[nodiscard]] std::span<std::byte> Data() noexcept {
    return backend_ ? backend_->Data() : std::span<std::byte>{};
  }
  [[nodiscard]] std::size_t size() noexcept { return Data().size(); }
  [[nodiscard]] explicit operator bool() const noexcept {
    return backend_ != nullptr;
  }

  Status Publish(const MessageEnvelope& envelope) noexcept {
    if (!backend_) {
      return Status(StatusCode::Closed, "publisher loan is empty");
    }
    auto backend = std::move(backend_);
    return backend->Publish(envelope);
  }

  Status Abandon() noexcept {
    if (!backend_) {
      return Status::Ok();
    }
    auto backend = std::move(backend_);
    return backend->Abandon();
  }

 private:
  std::unique_ptr<Backend> backend_;
};

class TransportSubscriberSample {
 public:
  class Backend {
   public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual std::span<const std::byte> Data()
        const noexcept = 0;
    virtual Status Release() noexcept = 0;
  };

  TransportSubscriberSample() = default;
  TransportSubscriberSample(
      MessageEnvelope envelope,
      std::unique_ptr<Backend> backend) noexcept
      : envelope_(envelope), backend_(std::move(backend)) {}
  ~TransportSubscriberSample() { static_cast<void>(Release()); }

  TransportSubscriberSample(const TransportSubscriberSample&) = delete;
  TransportSubscriberSample& operator=(
      const TransportSubscriberSample&) = delete;
  TransportSubscriberSample(TransportSubscriberSample&&) noexcept = default;
  TransportSubscriberSample& operator=(
      TransportSubscriberSample&& other) noexcept {
    if (this != &other) {
      static_cast<void>(Release());
      envelope_ = other.envelope_;
      backend_ = std::move(other.backend_);
    }
    return *this;
  }

  [[nodiscard]] std::span<const std::byte> Data() const noexcept {
    return backend_ ? backend_->Data() : std::span<const std::byte>{};
  }
  [[nodiscard]] std::size_t size() const noexcept { return Data().size(); }
  [[nodiscard]] const MessageEnvelope& envelope() const noexcept {
    return envelope_;
  }
  [[nodiscard]] TraceContext ContinueTrace() const noexcept {
    return TraceContext{
        envelope_.trace_id,
        envelope_.span_id,
        envelope_.source_timestamp_ns};
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return backend_ != nullptr;
  }

  Status Release() noexcept {
    if (!backend_) {
      return Status::Ok();
    }
    auto backend = std::move(backend_);
    return backend->Release();
  }

 private:
  MessageEnvelope envelope_;
  std::unique_ptr<Backend> backend_;
};

using TransportLoanedMessageCallback =
    std::function<void(TransportSubscriberSample)>;

class Transport {
 public:
  virtual ~Transport() = default;

  virtual Status Publish(std::string_view topic, Message message,
                         const QosProfile& qos) = 0;
  [[nodiscard]] virtual Result<SubscriptionId> Subscribe(
      std::string_view topic, const QosProfile& qos,
      TransportMessageCallback callback) = 0;
  virtual Status Unsubscribe(SubscriptionId subscription_id) = 0;

  [[nodiscard]] virtual Result<TransportPublisherLoan> Loan(
      std::string_view topic, std::size_t payload_size,
      const QosProfile& qos) {
    static_cast<void>(topic);
    static_cast<void>(payload_size);
    static_cast<void>(qos);
    return Status(
        StatusCode::Unsupported,
        "transport does not support loaned messages");
  }
  [[nodiscard]] virtual Result<SubscriptionId> SubscribeLoaned(
      std::string_view topic, const QosProfile& qos,
      TransportLoanedMessageCallback callback) {
    static_cast<void>(topic);
    static_cast<void>(qos);
    static_cast<void>(callback);
    return Status(
        StatusCode::Unsupported,
        "transport does not support loaned messages");
  }

  [[nodiscard]] virtual Result<ServiceId> AdvertiseService(
      std::string_view service_name, TransportServiceCallback callback) = 0;
  virtual Status RemoveService(ServiceId service_id) = 0;
  [[nodiscard]] virtual Result<Message> Request(
      std::string_view service_name, Message request, Deadline deadline) = 0;

  [[nodiscard]] virtual TransportStats Stats() const = 0;
  virtual Status Close() = 0;
};

}  // namespace autoruntime
