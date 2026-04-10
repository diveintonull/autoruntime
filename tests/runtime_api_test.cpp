#include "test_support.hpp"
#include <autoruntime/in_memory_transport.hpp>
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
#include <vector>

using namespace std::chrono_literals;

namespace {
std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}
std::string Text(std::span<const std::byte> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

int PubSubPreservesTraceAcrossSensorPlanningControl() {
  auto executor = std::make_shared<autoruntime::Executor>();
  auto transport = std::make_shared<autoruntime::InMemoryTransport>();
  auto sensor_group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"sensor", 1U, 32U});
  auto planning_group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"planning", 1U, 32U});
  auto control_group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"control", 1U, 32U});
  CHECK(sensor_group && planning_group && control_group);

  autoruntime::Node sensor({"sensor", 1U}, executor, transport);
  autoruntime::Node planning({"planning", 4U}, executor, transport);
  autoruntime::Node control({"control", 2U}, executor, transport);
  auto frame_publisher = sensor.CreatePublisher("sensor/frame");
  auto command_result = planning.CreatePublisher("control/command");
  CHECK(frame_publisher && command_result);

  std::atomic<std::uint64_t> received_trace{0U};
  std::atomic<std::uint64_t> received_sequence{0U};
  auto control_subscription = control.CreateSubscriber(
      "control/command",
      autoruntime::SubscriptionOptions{control_group.value(), 8U,
                                       autoruntime::OverflowPolicy::DropNewest},
      [&](const autoruntime::Message& message) {
        received_trace.store(message.envelope.trace_id,
                             std::memory_order_release);
        received_sequence.store(message.envelope.sequence,
                                std::memory_order_release);
      });
  CHECK(control_subscription);

  auto command_publisher = command_result.value();
  auto planning_subscription = planning.CreateSubscriber(
      "sensor/frame",
      autoruntime::SubscriptionOptions{planning_group.value(), 8U,
                                       autoruntime::OverflowPolicy::DropNewest},
      [command_publisher](const autoruntime::Message& message) mutable {
        const auto command = Bytes("steer");
        if (!command_publisher.Publish(command, message.ContinueTrace())) {
          std::terminate();
        }
      });
  CHECK(planning_subscription);
  CHECK(executor->Start());

  const auto frame = Bytes("frame");
  CHECK(frame_publisher.value().Publish(frame));
  CHECK(WaitUntil(
      [&] { return received_trace.load(std::memory_order_acquire) != 0U; },
      1s));
  CHECK(received_sequence.load(std::memory_order_acquire) == 1U);
  const auto stats = transport->Stats();
  CHECK(stats.published_messages == 2U);
  CHECK(stats.delivered_messages == 2U);
  CHECK(stats.dropped_messages == 0U);
  CHECK(executor->Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int ServiceClientAndTimerUseUnifiedApi() {
  auto executor = std::make_shared<autoruntime::Executor>();
  auto transport = std::make_shared<autoruntime::InMemoryTransport>();
  auto services = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"services", 1U, 32U});
  auto timers = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"timers", 1U, 32U});
  CHECK(services && timers);
  autoruntime::Node server({"planner", 7U}, executor, transport);
  autoruntime::Node caller({"coordinator", 3U}, executor, transport);

  auto service = server.CreateService(
      "planner/echo", services.value(),
      [](const autoruntime::Message& request)
          -> autoruntime::Result<std::vector<std::byte>> {
        return request.payload;
      });
  auto client = caller.CreateClient("planner/echo");
  CHECK(service && client);
  std::atomic<std::uint64_t> calls{0U};
  auto timer = caller.CreateTimer(
      "heartbeat", timers.value(), 10ms, 8ms,
      [&](std::stop_token) {
        calls.fetch_add(1U, std::memory_order_relaxed);
      });
  CHECK(timer);
  CHECK(executor->Start());

  const auto request = Bytes("route");
  auto response = client.value().Call(
      request, autoruntime::Deadline::After(500ms));
  CHECK(response);
  CHECK(Text(response.value().payload) == "route");
  CHECK(response.value().envelope.trace_id != 0U);
  CHECK(WaitUntil(
      [&] { return calls.load(std::memory_order_relaxed) >= 3U; }, 300ms));
  CHECK(timer.value().Cancel());
  CHECK(service.value().Close());
  CHECK(executor->Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int SubscriberQueueIsBoundedAndReportsDrops() {
  auto executor = std::make_shared<autoruntime::Executor>();
  auto transport = std::make_shared<autoruntime::InMemoryTransport>();
  auto group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"slow", 1U, 8U});
  CHECK(group);
  autoruntime::Node camera({"camera", 1U}, executor, transport);
  autoruntime::Node perception({"perception", 1U}, executor, transport);
  auto publisher = camera.CreatePublisher("camera/image");
  CHECK(publisher);
  std::atomic<bool> entered{false};
  auto subscriber = perception.CreateSubscriber(
      "camera/image",
      autoruntime::SubscriptionOptions{group.value(), 2U,
                                       autoruntime::OverflowPolicy::DropNewest},
      [&](const autoruntime::Message&) {
        entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(80ms);
      });
  CHECK(subscriber);
  CHECK(executor->Start());
  const auto payload = Bytes("image");
  CHECK(publisher.value().Publish(payload));
  CHECK(WaitUntil(
      [&] { return entered.load(std::memory_order_acquire); }, 200ms));
  for (int i = 0; i < 10; ++i) CHECK(publisher.value().Publish(payload));
  CHECK(WaitUntil(
      [&] { return subscriber.value().Stats().dropped_messages > 0U; }, 300ms));
  CHECK(subscriber.value().Stats().queue_high_watermark <= 2U);
  CHECK(executor->Stop(autoruntime::Deadline::After(1s)));
  return 0;
}
}  // namespace

int main() {
  if (PubSubPreservesTraceAcrossSensorPlanningControl() != 0) return 1;
  if (ServiceClientAndTimerUseUnifiedApi() != 0) return 1;
  return SubscriberQueueIsBoundedAndReportsDrops();
}
