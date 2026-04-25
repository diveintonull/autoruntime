#include "test_support.hpp"

#include <autoruntime/fastipc_transport.hpp>
#include <autoruntime/in_memory_transport.hpp>
#include <autoruntime/node.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

using namespace std::chrono_literals;

namespace {

constexpr std::string_view kTopic = "camera/frame";

autoruntime::FastIpcTransportConfig Config(
    const std::string& channel,
    autoruntime::FastIpcDirection direction,
    std::uint32_t slot_count,
    std::uint32_t maximum_payload,
    bool unlink_on_close) {
  autoruntime::FastIpcTransportConfig config;
  config.open_timeout = 2s;
  config.endpoints.push_back(autoruntime::FastIpcEndpointConfig{
      std::string(kTopic), channel, direction, slot_count,
      maximum_payload, 500ms, unlink_on_close});
  return config;
}

autoruntime::QosProfile ReliableQos(
    std::chrono::milliseconds deadline = 500ms) {
  autoruntime::QosProfile qos;
  qos.reliability = autoruntime::Reliability::Reliable;
  qos.history = autoruntime::HistoryKind::KeepLast;
  qos.depth = 8U;
  qos.deadline = deadline;
  return qos;
}

std::byte Pattern(std::size_t index) {
  return static_cast<std::byte>((index * 131U + 17U) % 251U);
}

void Fill(std::span<std::byte> payload) {
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    payload[index] = Pattern(index);
  }
}

bool Valid(std::span<const std::byte> payload) {
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    if (payload[index] != Pattern(index)) {
      return false;
    }
  }
  return true;
}

int UnsupportedTransportRejectsLoan() {
  auto transport = std::make_shared<autoruntime::InMemoryTransport>();
  auto executor = std::make_shared<autoruntime::Executor>();
  auto group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"unsupported", 1U, 4U});
  CHECK(group);
  autoruntime::Node node({"in-memory", 1U}, executor, transport);
  auto publisher = node.CreatePublisher(std::string(kTopic));
  CHECK(publisher);

  auto loan = publisher.value().Loan(64U);
  CHECK(!loan);
  CHECK(loan.status().code() == autoruntime::StatusCode::Unsupported);

  auto subscription = node.CreateLoanedSubscriber(
      std::string(kTopic),
      autoruntime::SubscriptionOptions{group.value(), 1U},
      [](const autoruntime::LoanedMessage&) {});
  CHECK(!subscription);
  CHECK(
      subscription.status().code() ==
      autoruntime::StatusCode::Unsupported);
  return 0;
}

int LoanedPublishTakeIsEndToEnd() {
  constexpr std::size_t kPayloadBytes = 1024U * 1024U;
  const std::string channel =
      "autoruntime_loan_e2e_" + std::to_string(::getpid());
  auto producer_result = autoruntime::FastIpcTransport::Create(
      Config(channel, autoruntime::FastIpcDirection::Publish,
             4U, static_cast<std::uint32_t>(kPayloadBytes), true));
  CHECK(producer_result);
  auto consumer_result = autoruntime::FastIpcTransport::Create(
      Config(channel, autoruntime::FastIpcDirection::Subscribe,
             4U, static_cast<std::uint32_t>(kPayloadBytes), false));
  CHECK(consumer_result);
  auto producer_transport = producer_result.value();
  auto consumer_transport = consumer_result.value();

  auto executor = std::make_shared<autoruntime::Executor>();
  auto group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"loaned-e2e", 1U, 4U});
  CHECK(group);
  autoruntime::Node camera(
      {"camera", 41U}, executor, producer_transport);
  autoruntime::Node perception(
      {"perception", 7U}, executor, consumer_transport);

  auto publisher = camera.CreatePublisher(
      std::string(kTopic), ReliableQos());
  CHECK(publisher);
  std::atomic<std::uint64_t> received{0U};
  std::atomic<bool> valid{true};
  auto subscriber = perception.CreateLoanedSubscriber(
      std::string(kTopic),
      autoruntime::SubscriptionOptions{
          group.value(), 2U, autoruntime::OverflowPolicy::DropNewest,
          ReliableQos()},
      [&](const autoruntime::LoanedMessage& message) {
        if (message.size() != kPayloadBytes ||
            message.envelope().sequence != 1U ||
            message.envelope().source_generation != 41U ||
            !Valid(message.Data())) {
          valid.store(false, std::memory_order_release);
        }
        received.fetch_add(1U, std::memory_order_release);
      });
  CHECK(subscriber);
  CHECK(executor->Start());

  auto loan_result = publisher.value().Loan(kPayloadBytes);
  CHECK(loan_result);
  auto loan = std::move(loan_result).take_value();
  CHECK(loan.size() == kPayloadBytes);
  CHECK(loan.Data().data() != nullptr);
  Fill(loan.Data());
  CHECK(loan.Publish());
  CHECK(WaitUntil(
      [&] { return received.load(std::memory_order_acquire) == 1U; },
      4s));
  CHECK(valid.load(std::memory_order_acquire));
  CHECK(producer_transport->Stats().published_messages == 1U);
  CHECK(consumer_transport->Stats().delivered_messages == 1U);
  CHECK(subscriber.value().Stats().delivered_messages == 1U);

  CHECK(subscriber.value().Close());
  CHECK(executor->Stop(autoruntime::Deadline::After(1s)));
  CHECK(consumer_transport->Close());
  CHECK(producer_transport->Close());
  return 0;
}

int HeldSampleAppliesBackpressureAndAbandonReclaims() {
  constexpr std::size_t kPayloadBytes = 64U * 1024U;
  const std::string channel =
      "autoruntime_loan_pressure_" + std::to_string(::getpid());
  auto producer_result = autoruntime::FastIpcTransport::Create(
      Config(channel, autoruntime::FastIpcDirection::Publish,
             2U, static_cast<std::uint32_t>(kPayloadBytes), true));
  CHECK(producer_result);
  auto consumer_result = autoruntime::FastIpcTransport::Create(
      Config(channel, autoruntime::FastIpcDirection::Subscribe,
             2U, static_cast<std::uint32_t>(kPayloadBytes), false));
  CHECK(consumer_result);
  auto producer_transport = producer_result.value();
  auto consumer_transport = consumer_result.value();

  auto executor = std::make_shared<autoruntime::Executor>();
  auto group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"loaned-pressure", 1U, 2U});
  CHECK(group);
  autoruntime::Node camera(
      {"camera", 51U}, executor, producer_transport);
  autoruntime::Node perception(
      {"perception", 9U}, executor, consumer_transport);
  const auto qos = ReliableQos(25ms);
  auto publisher = camera.CreatePublisher(std::string(kTopic), qos);
  CHECK(publisher);

  std::atomic<bool> first_entered{false};
  std::atomic<bool> release_first{false};
  std::atomic<bool> valid{true};
  std::atomic<std::uint64_t> received{0U};
  auto subscriber = perception.CreateLoanedSubscriber(
      std::string(kTopic),
      autoruntime::SubscriptionOptions{
          group.value(), 1U, autoruntime::OverflowPolicy::DropNewest,
          qos},
      [&](const autoruntime::LoanedMessage& message) {
        if (message.size() != kPayloadBytes || !Valid(message.Data())) {
          valid.store(false, std::memory_order_release);
        }
        const auto ordinal =
            received.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        if (ordinal == 1U) {
          first_entered.store(true, std::memory_order_release);
          while (!release_first.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
          }
        }
      });
  CHECK(subscriber);
  CHECK(executor->Start());

  auto first_result = publisher.value().Loan(kPayloadBytes);
  CHECK(first_result);
  auto first = std::move(first_result).take_value();
  Fill(first.Data());
  CHECK(first.Publish());
  CHECK(WaitUntil(
      [&] { return first_entered.load(std::memory_order_acquire); },
      2s));
  auto queued_result = publisher.value().Loan(kPayloadBytes);
  CHECK(queued_result);
  auto queued = std::move(queued_result).take_value();
  Fill(queued.Data());
  CHECK(queued.Publish());


  const auto timeout_start = std::chrono::steady_clock::now();
  auto blocked = publisher.value().Loan(kPayloadBytes);
  const auto timeout_elapsed =
      std::chrono::steady_clock::now() - timeout_start;
  CHECK(!blocked);
  CHECK(blocked.status().code() == autoruntime::StatusCode::Timeout);
  CHECK(timeout_elapsed >= 20ms);

  release_first.store(true, std::memory_order_release);
  CHECK(WaitUntil(
      [&] {
        return subscriber.value().Stats().delivered_messages == 2U;
      },
      2s));

  auto abandoned_result = publisher.value().Loan(kPayloadBytes);
  CHECK(abandoned_result);
  auto abandoned = std::move(abandoned_result).take_value();
  Fill(abandoned.Data());
  auto reservation_blocked = publisher.value().Loan(kPayloadBytes);
  CHECK(!reservation_blocked);
  CHECK(reservation_blocked.status().code() ==
        autoruntime::StatusCode::QueueFull);
  CHECK(abandoned.Abandon());


  auto reclaimed_result = publisher.value().Loan(kPayloadBytes);
  CHECK(reclaimed_result);
  auto reclaimed = std::move(reclaimed_result).take_value();
  Fill(reclaimed.Data());
  CHECK(reclaimed.Publish());
  CHECK(WaitUntil(
      [&] { return received.load(std::memory_order_acquire) == 3U; },
      2s));
  CHECK(valid.load(std::memory_order_acquire));
  CHECK(producer_transport->Stats().published_messages == 3U);
  CHECK(producer_transport->Stats().publish_failures == 2U);

  CHECK(subscriber.value().Close());
  CHECK(executor->Stop(autoruntime::Deadline::After(1s)));
  CHECK(consumer_transport->Close());
  CHECK(producer_transport->Close());
  return 0;
}

int RunCase(std::string_view name) {
  if (name == "unsupported") {
    return UnsupportedTransportRejectsLoan();
  }
  if (name == "end_to_end") {
    return LoanedPublishTakeIsEndToEnd();
  }
  if (name == "backpressure") {
    return HeldSampleAppliesBackpressureAndAbandonReclaims();
  }
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--case") {
    return RunCase(argv[2]);
  }
  if (argc != 1) {
    return 2;
  }
  if (UnsupportedTransportRejectsLoan() != 0 ||
      LoanedPublishTakeIsEndToEnd() != 0 ||
      HeldSampleAppliesBackpressureAndAbandonReclaims() != 0) {
    return 1;
  }
  return 0;
}
