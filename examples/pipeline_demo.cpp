#include <autoruntime/health_monitor.hpp>
#include <autoruntime/in_memory_transport.hpp>
#include <autoruntime/node.hpp>
#include <autoruntime/observability.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

template <typename T>
T Take(autoruntime::Result<T> result, std::string_view operation) {
  if (!result) {
    throw std::runtime_error(
        std::string(operation) + ": " + result.status().detail());
  }
  return std::move(result).take_value();
}

void Require(const autoruntime::Status& status,
             std::string_view operation) {
  if (!status) {
    throw std::runtime_error(
        std::string(operation) + ": " + status.detail());
  }
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

std::vector<std::byte> Bytes(std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text));
  return {bytes.begin(), bytes.end()};
}

std::string Text(std::span<const std::byte> bytes) {
  return {
      reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

int main() {
  try {
    auto executor = std::make_shared<autoruntime::Executor>();
    auto transport =
        std::make_shared<autoruntime::InMemoryTransport>();
    const auto pipeline_group = Take(
        executor->CreateCallbackGroup({"pipeline", 1U, 32U}),
        "create pipeline group");
    const auto service_group = Take(
        executor->CreateCallbackGroup({"services", 1U, 16U}),
        "create service group");

    autoruntime::Node sensor(
        {"sensor", 1U}, executor, transport);
    autoruntime::Node control(
        {"control", 1U}, executor, transport);

    std::atomic<std::uint64_t> received{0U};
    std::atomic<std::uint64_t> timer_ticks{0U};
    auto subscription = Take(
        control.CreateSubscriber(
            "sensor/frame",
            autoruntime::SubscriptionOptions{
                pipeline_group,
                8U,
                autoruntime::OverflowPolicy::DropOldest},
            [&](const autoruntime::Message& message) {
              std::cout << "frame sequence="
                        << message.envelope.sequence
                        << " trace=" << message.envelope.trace_id
                        << " payload=" << Text(message.payload) << '\n';
              received.fetch_add(1U, std::memory_order_release);
            }),
        "create subscription");
    auto publisher = Take(
        sensor.CreatePublisher("sensor/frame"),
        "create publisher");

    auto arm_service = Take(
        control.CreateService(
            "control/arm", service_group,
            [](const autoruntime::Message& request)
                -> autoruntime::Result<std::vector<std::byte>> {
              return Bytes("armed:" + Text(request.payload));
            }),
        "create service");
    auto arm_client = Take(
        sensor.CreateClient("control/arm"), "create client");
    auto timer = Take(
        control.CreateTimer(
            "heartbeat", pipeline_group, 20ms, 10ms,
            [&](std::stop_token) {
              timer_ticks.fetch_add(1U, std::memory_order_release);
            },
            50),
        "create timer");

    autoruntime::MetricsRegistry metrics;
    autoruntime::StructuredLogger logger(
        [](std::string record) { std::cout << record << '\n'; });
    autoruntime::HealthMonitor health;
    const auto now = autoruntime::HealthMonitor::Clock::now();
    auto component = health.Register(
        {"control",
         static_cast<std::int64_t>(::getpid()),
         1U,
         autoruntime::HealthPolicy{}},
        now);
    if (!component) {
      throw std::runtime_error(component.status().detail());
    }
    Require(health.Heartbeat("control", 1U, 1U, now),
            "record heartbeat");
    Require(health.Progress("control", 1U, 1U, now),
            "record progress");
    Require(logger.Log(
                {autoruntime::LogLevel::Info,
                 "pipeline_demo",
                 "started",
                 1U,
                 0U,
                 0U,
                 autoruntime::StatusCode::Ok,
                 "demo runtime started",
                 {{"transport", "in_memory"}}}),
            "write structured log");

    Require(executor->Start(), "start executor");
    const auto payload = Bytes("frame-001");
    Require(publisher.Publish(payload), "publish frame");

    const auto request = Bytes("vehicle-7");
    auto response = arm_client.Call(
        request, autoruntime::Deadline::After(1s));
    if (!response) {
      throw std::runtime_error(response.status().detail());
    }
    std::cout << "service response=" << Text(response.value().payload)
              << '\n';

    if (!WaitUntil(
            [&] {
              return received.load(std::memory_order_acquire) == 1U &&
                     timer_ticks.load(std::memory_order_acquire) >= 2U;
            },
            1s)) {
      throw std::runtime_error("demo callbacks did not complete");
    }

    const auto subscription_stats = subscription.Stats();
    static_cast<void>(metrics.SetGauge(
        "demo.node.control.running", 1.0));
    static_cast<void>(metrics.IncrementCounter(
        "demo.messages.received",
        static_cast<double>(subscription_stats.delivered_messages)));
    static_cast<void>(metrics.SetGauge(
        "demo.timer.ticks",
        static_cast<double>(
            timer_ticks.load(std::memory_order_acquire))));
    std::cout << metrics.RenderJson() << '\n';

    Require(timer.Cancel(), "cancel timer");
    Require(subscription.Close(), "close subscription");
    Require(arm_service.Close(), "close service");
    Require(executor->Stop(autoruntime::Deadline::After(1s)),
            "stop executor");
    Require(transport->Close(), "close transport");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pipeline_demo: " << error.what() << '\n';
    return 1;
  }
}
