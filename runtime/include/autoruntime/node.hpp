#pragma once

#include <autoruntime/executor.hpp>
#include <autoruntime/message.hpp>
#include <autoruntime/transport.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace autoruntime {

struct NodeConfig {
  std::string name;
  std::uint64_t generation{1U};
};

enum class OverflowPolicy {
  DropNewest,
  DropOldest,
};

struct SubscriptionOptions {
  CallbackGroupId callback_group{0U};
  std::size_t queue_capacity{16U};
  OverflowPolicy overflow_policy{OverflowPolicy::DropNewest};
  QosProfile qos{};
};

struct SubscriptionStats {
  std::uint64_t received_messages{0U};
  std::uint64_t delivered_messages{0U};
  std::uint64_t dropped_messages{0U};
  std::size_t queue_depth{0U};
  std::size_t queue_high_watermark{0U};
};

using MessageCallback = std::function<void(const Message&)>;
using ServiceHandler =
    std::function<Result<std::vector<std::byte>>(const Message&)>;

class PublisherLoan {
 public:
  PublisherLoan() = default;
  PublisherLoan(const PublisherLoan&) = delete;
  PublisherLoan& operator=(const PublisherLoan&) = delete;
  PublisherLoan(PublisherLoan&&) noexcept = default;
  PublisherLoan& operator=(PublisherLoan&&) noexcept = default;

  [[nodiscard]] std::span<std::byte> Data() noexcept;
  [[nodiscard]] std::size_t size() noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;
  Status Publish() noexcept;
  Status Abandon() noexcept;

 private:
  PublisherLoan(
      TransportPublisherLoan loan, MessageEnvelope envelope) noexcept;
  TransportPublisherLoan loan_;
  MessageEnvelope envelope_;
  friend class Publisher;
};

class Publisher {
 public:
  Publisher() = default;
  Status Publish(std::span<const std::byte> payload,
                 TraceContext trace = {}) const;
  [[nodiscard]] Result<PublisherLoan> Loan(
      std::size_t payload_size, TraceContext trace = {}) const;
  [[nodiscard]] explicit operator bool() const noexcept;

 private:
  struct Impl;
  explicit Publisher(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Node;
};

class Subscriber {
 public:
  Subscriber() = default;
  [[nodiscard]] SubscriptionStats Stats() const;
  Status Close();
  [[nodiscard]] explicit operator bool() const noexcept;

 private:
  struct Impl;
  explicit Subscriber(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Node;
};

class LoanedMessage {
 public:
  explicit LoanedMessage(TransportSubscriberSample sample) noexcept
      : sample_(std::move(sample)) {}
  LoanedMessage(const LoanedMessage&) = delete;
  LoanedMessage& operator=(const LoanedMessage&) = delete;
  LoanedMessage(LoanedMessage&&) noexcept = default;
  LoanedMessage& operator=(LoanedMessage&&) noexcept = default;

  [[nodiscard]] std::span<const std::byte> Data() const noexcept {
    return sample_.Data();
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return sample_.size();
  }
  [[nodiscard]] const MessageEnvelope& envelope() const noexcept {
    return sample_.envelope();
  }
  [[nodiscard]] TraceContext ContinueTrace() const noexcept {
    return sample_.ContinueTrace();
  }

 private:
  TransportSubscriberSample sample_;
};

using LoanedMessageCallback =
    std::function<void(const LoanedMessage&)>;

class LoanedSubscriber {
 public:
  LoanedSubscriber() = default;
  [[nodiscard]] SubscriptionStats Stats() const;
  Status Close();
  [[nodiscard]] explicit operator bool() const noexcept;

 private:
  struct Impl;
  explicit LoanedSubscriber(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Node;
};

class Service {
 public:
  Service() = default;
  Status Close();
  [[nodiscard]] explicit operator bool() const noexcept;

 private:
  struct Impl;
  explicit Service(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Node;
};

class Client {
 public:
  Client() = default;
  [[nodiscard]] Result<Message> Call(
      std::span<const std::byte> payload, Deadline deadline,
      TraceContext trace = {}) const;
  [[nodiscard]] explicit operator bool() const noexcept;

 private:
  struct Impl;
  explicit Client(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Node;
};

class Timer {
 public:
  Timer() = default;
  Status Cancel();
  [[nodiscard]] Result<TaskStats> Stats() const;
  [[nodiscard]] explicit operator bool() const noexcept;

 private:
  struct Impl;
  explicit Timer(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Node;
};

class Node {
 public:
  Node(NodeConfig config, std::shared_ptr<Executor> executor,
       std::shared_ptr<Transport> transport);

  [[nodiscard]] const NodeConfig& config() const noexcept;
  [[nodiscard]] Result<Publisher> CreatePublisher(
      std::string topic, QosProfile qos = {}) const;
  [[nodiscard]] Result<Subscriber> CreateSubscriber(
      std::string topic, SubscriptionOptions options,
      MessageCallback callback) const;
  [[nodiscard]] Result<LoanedSubscriber> CreateLoanedSubscriber(
      std::string topic, SubscriptionOptions options,
      LoanedMessageCallback callback) const;
  [[nodiscard]] Result<Service> CreateService(
      std::string service_name, CallbackGroupId callback_group,
      ServiceHandler handler) const;
  [[nodiscard]] Result<Client> CreateClient(
      std::string service_name) const;
  [[nodiscard]] Result<Timer> CreateTimer(
      std::string name, CallbackGroupId callback_group,
      std::chrono::nanoseconds period,
      std::chrono::nanoseconds deadline,
      TaskCallback callback, int priority = 0) const;

 private:
  NodeConfig config_;
  std::shared_ptr<Executor> executor_;
  std::shared_ptr<Transport> transport_;
};

}  // namespace autoruntime
