#include "test_support.hpp"

#include <autoruntime/dds_transport.hpp>
#include <autoruntime/node.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

std::string Text(std::span<const std::byte> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

autoruntime::DdsTransportConfig Config(std::string name) {
  autoruntime::DdsTransportConfig config;
  config.domain_id = static_cast<std::uint32_t>(20 + (::getpid() % 180));
  config.participant_name = std::move(name);
  config.receive_poll_interval = 2ms;
  config.reliability_max_blocking_time = 250ms;
  return config;
}

int RealDdsParticipantsPreserveEnvelopeAndQos() {
  auto subscriber_transport =
      autoruntime::DdsTransport::Create(Config("dds-subscriber"));
  auto publisher_transport =
      autoruntime::DdsTransport::Create(Config("dds-publisher"));
  CHECK(subscriber_transport && publisher_transport);

  auto executor = std::make_shared<autoruntime::Executor>();
  auto group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"dds-receiver", 1U, 32U});
  CHECK(group);

  autoruntime::QosProfile qos;
  qos.reliability = autoruntime::Reliability::Reliable;
  qos.history = autoruntime::HistoryKind::KeepLast;
  qos.depth = 8U;
  qos.deadline = 1s;
  qos.liveliness_lease = 2s;

  autoruntime::Node subscriber_node(
      {"dds-subscriber", 3U}, executor, subscriber_transport.value());
  autoruntime::Node publisher_node(
      {"dds-publisher", 17U}, executor, publisher_transport.value());

  std::atomic<bool> received{false};
  std::atomic<std::uint64_t> trace_id{0U};
  std::atomic<std::uint64_t> parent_span_id{0U};
  std::atomic<std::uint64_t> source_timestamp{0U};
  std::atomic<std::uint64_t> source_generation{0U};
  std::string received_payload;
  auto subscriber = subscriber_node.CreateSubscriber(
      "autoruntime/dds/integration",
      autoruntime::SubscriptionOptions{
          group.value(), 16U, autoruntime::OverflowPolicy::DropNewest, qos},
      [&](const autoruntime::Message& message) {
        trace_id.store(message.envelope.trace_id, std::memory_order_relaxed);
        parent_span_id.store(message.envelope.parent_span_id,
                             std::memory_order_relaxed);
        source_timestamp.store(message.envelope.source_timestamp_ns,
                               std::memory_order_relaxed);
        source_generation.store(message.envelope.source_generation,
                                std::memory_order_relaxed);
        received_payload = Text(message.payload);
        received.store(true, std::memory_order_release);
      });
  auto publisher =
      publisher_node.CreatePublisher("autoruntime/dds/integration", qos);
  CHECK(subscriber && publisher);
  CHECK(executor->Start());

  const std::string_view payload = "cyclone-dds-frame";
  const auto bytes = std::as_bytes(std::span(payload));
  for (int attempt = 0;
       attempt < 60 && !received.load(std::memory_order_acquire);
       ++attempt) {
    CHECK(publisher.value().Publish(
        bytes, autoruntime::TraceContext{7001U, 8002U, 9003U}));
    std::this_thread::sleep_for(50ms);
  }
  CHECK(WaitUntil(
      [&] { return received.load(std::memory_order_acquire); }, 2s));
  CHECK(trace_id.load(std::memory_order_relaxed) == 7001U);
  CHECK(parent_span_id.load(std::memory_order_relaxed) == 8002U);
  CHECK(source_timestamp.load(std::memory_order_relaxed) == 9003U);
  CHECK(source_generation.load(std::memory_order_relaxed) == 17U);
  CHECK(received_payload == payload);
  CHECK(publisher_transport.value()->Stats().published_messages >= 1U);
  CHECK(subscriber_transport.value()->Stats().delivered_messages >= 1U);
  CHECK(executor->Stop(autoruntime::Deadline::After(1s)));
  CHECK(subscriber.value().Close());
  CHECK(subscriber_transport.value()->Close());
  CHECK(publisher_transport.value()->Close());
  return 0;
}

int UnsupportedRpcIsTyped() {
  auto result = autoruntime::DdsTransport::Create(Config("dds-errors"));
  CHECK(result);
  auto advertised = result.value()->AdvertiseService("route", {});
  CHECK(!advertised);
  CHECK(advertised.status().code() == autoruntime::StatusCode::Unsupported);
  CHECK(result.value()->Close());
  return 0;
}

}  // namespace

int main() {
  if (const int result = RealDdsParticipantsPreserveEnvelopeAndQos();
      result != 0) {
    return result;
  }
  return UnsupportedRpcIsTyped();
}
