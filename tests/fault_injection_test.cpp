#include "test_support.hpp"

#include <autoruntime/distributed.hpp>
#include <autoruntime/executor.hpp>
#include <autoruntime/health_monitor.hpp>
#include <autoruntime/in_memory_transport.hpp>
#include <autoruntime/node.hpp>

#if defined(AUTORUNTIME_HAS_DDS)
#include <autoruntime/dds_transport.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

template <typename T>
T Take(autoruntime::Result<T> result) {
  if (!result) {
    throw std::runtime_error(result.status().detail());
  }
  return std::move(result).take_value();
}

autoruntime::TaskConfig EventConfig(
    std::string name, autoruntime::CallbackGroupId group,
    std::size_t capacity = 2U,
    std::chrono::nanoseconds deadline = 100ms) {
  autoruntime::TaskConfig config;
  config.name = std::move(name);
  config.kind = autoruntime::TaskKind::Event;
  config.queue_capacity = capacity;
  config.deadline = deadline;
  config.callback_group = group;
  return config;
}

autoruntime::HealthPolicy HealthPolicy() {
  autoruntime::HealthPolicy policy;
  policy.heartbeat_timeout = 20ms;
  policy.no_progress_timeout = 20ms;
  policy.max_backlog = 2U;
  policy.max_deadline_misses = 1U;
  policy.restart_backoff = 1ms;
  policy.recovery_timeout = 500ms;
  policy.max_restart_attempts = 2U;
  return policy;
}

int TransportClosed() {
  autoruntime::InMemoryTransport transport;
  CHECK(transport.Close());
  autoruntime::Message message;
  message.envelope.trace_id = 1U;
  const auto status =
      transport.Publish("sensor", std::move(message), {});
  CHECK(!status);
  CHECK(status.code() == autoruntime::StatusCode::Closed);
  CHECK(transport.Stats().publish_failures == 1U);
  return 0;
}

int QueueOverflow() {
  autoruntime::Executor executor;
  const auto group = Take(executor.CreateCallbackGroup(
      {"overflow", 1U, 4U}));
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  const auto task = Take(executor.AddTask(
      EventConfig("bounded", group, 1U),
      [&](std::stop_token) {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
      }));
  CHECK(executor.Start());
  CHECK(executor.Notify(task));
  {
    std::unique_lock lock(mutex);
    CHECK(condition.wait_for(lock, 1s, [&] { return entered; }));
  }
  CHECK(executor.Notify(task));
  const auto overflow = executor.Notify(task);
  CHECK(!overflow);
  CHECK(overflow.code() == autoruntime::StatusCode::QueueFull);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  CHECK(executor.Stats(task).value().queue_overflows == 1U);
  return 0;
}

int SlowCallbackIsolation() {
  autoruntime::Executor executor;
  const auto planning = Take(executor.CreateCallbackGroup(
      {"planning", 1U, 4U}));
  const auto control = Take(executor.CreateCallbackGroup(
      {"control", 1U, 4U}));
  std::atomic<bool> slow_entered{false};
  std::atomic<bool> control_finished{false};
  const auto slow = Take(executor.AddTask(
      EventConfig("slow", planning),
      [&](std::stop_token) {
        slow_entered.store(true, std::memory_order_release);
        std::this_thread::sleep_for(300ms);
      }));
  const auto critical = Take(executor.AddTask(
      EventConfig("critical", control),
      [&](std::stop_token) {
        control_finished.store(true, std::memory_order_release);
      }));
  CHECK(executor.Start());
  CHECK(executor.Notify(slow));
  CHECK(WaitUntil(
      [&] { return slow_entered.load(std::memory_order_acquire); }, 100ms));
  CHECK(executor.Notify(critical));
  CHECK(WaitUntil(
      [&] {
        return control_finished.load(std::memory_order_acquire);
      },
      50ms));
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int DeadlineMiss() {
  autoruntime::Executor executor;
  const auto group = Take(executor.CreateCallbackGroup(
      {"deadline", 1U, 4U}));
  const auto task = Take(executor.AddTask(
      EventConfig("late", group, 1U, 1ms),
      [](std::stop_token) { std::this_thread::sleep_for(10ms); }));
  CHECK(executor.Start());
  CHECK(executor.Notify(task));
  CHECK(WaitUntil(
      [&] {
        auto stats = executor.Stats(task);
        return stats && stats.value().finished == 1U;
      },
      1s));
  CHECK(executor.Stats(task).value().deadline_misses == 1U);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int CancelledTask() {
  autoruntime::Executor executor;
  const auto group = Take(executor.CreateCallbackGroup(
      {"cancel", 1U, 4U}));
  const auto task = Take(executor.AddTask(
      EventConfig("cancelled", group),
      [](std::stop_token) {}));
  CHECK(executor.Start());
  CHECK(executor.Cancel(task));
  const auto status = executor.Notify(task);
  CHECK(!status);
  CHECK(status.code() == autoruntime::StatusCode::Cancelled);
  CHECK(executor.Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

int HeartbeatLoss() {
  autoruntime::HealthMonitor monitor([](std::int64_t) { return true; });
  const auto base = autoruntime::HealthMonitor::Clock::now();
  CHECK(monitor.Register(
      {"planning", 10, 1U, HealthPolicy()}, base));
  CHECK(monitor.Heartbeat("planning", 1U, 1U, base));
  CHECK(monitor.Progress("planning", 1U, 1U, base));
  static_cast<void>(monitor.Evaluate(base + 21ms));
  const auto state = monitor.Snapshot("planning");
  CHECK(state);
  CHECK(state.value().state == autoruntime::HealthState::Failed);
  CHECK(state.value().reason ==
        autoruntime::HealthReason::HeartbeatLost);
  return 0;
}

int NoProgress() {
  auto policy = HealthPolicy();
  policy.heartbeat_timeout = 1s;
  autoruntime::HealthMonitor monitor([](std::int64_t) { return true; });
  const auto base = autoruntime::HealthMonitor::Clock::now();
  CHECK(monitor.Register({"planning", 10, 1U, policy}, base));
  CHECK(monitor.Heartbeat("planning", 1U, 1U, base));
  CHECK(monitor.Progress("planning", 1U, 1U, base));
  CHECK(monitor.Heartbeat("planning", 1U, 2U, base + 21ms));
  static_cast<void>(monitor.Evaluate(base + 21ms));
  const auto state = monitor.Snapshot("planning");
  CHECK(state);
  CHECK(state.value().state == autoruntime::HealthState::Failed);
  CHECK(state.value().reason ==
        autoruntime::HealthReason::NoProgress);
  return 0;
}

int BacklogDegraded() {
  autoruntime::HealthMonitor monitor([](std::int64_t) { return true; });
  const auto base = autoruntime::HealthMonitor::Clock::now();
  CHECK(monitor.Register(
      {"planning", 10, 1U, HealthPolicy()}, base));
  CHECK(monitor.Heartbeat("planning", 1U, 1U, base));
  CHECK(monitor.Progress("planning", 1U, 1U, base));
  CHECK(monitor.ReportBacklog("planning", 1U, 3U));
  static_cast<void>(monitor.Evaluate(base + 1ms));
  const auto state = monitor.Snapshot("planning");
  CHECK(state);
  CHECK(state.value().state == autoruntime::HealthState::Degraded);
  CHECK(state.value().reason ==
        autoruntime::HealthReason::BacklogExceeded);
  return 0;
}

int ProcessExit() {
  bool alive = true;
  autoruntime::HealthMonitor monitor(
      [&](std::int64_t) { return alive; });
  const auto base = autoruntime::HealthMonitor::Clock::now();
  CHECK(monitor.Register(
      {"planning", 10, 1U, HealthPolicy()}, base));
  CHECK(monitor.Heartbeat("planning", 1U, 1U, base));
  CHECK(monitor.Progress("planning", 1U, 1U, base));
  alive = false;
  static_cast<void>(monitor.Evaluate(base + 1ms));
  const auto state = monitor.Snapshot("planning");
  CHECK(state);
  CHECK(state.value().state == autoruntime::HealthState::Failed);
  CHECK(state.value().reason ==
        autoruntime::HealthReason::ProcessExited);
  return 0;
}

int StaleGeneration() {
  autoruntime::HealthMonitor monitor([](std::int64_t) { return true; });
  CHECK(monitor.Register(
      {"planning", 10, 2U, HealthPolicy()}));
  const auto status = monitor.Heartbeat("planning", 1U, 1U);
  CHECK(!status);
  CHECK(status.code() == autoruntime::StatusCode::StaleGeneration);
  return 0;
}

int RpcDisconnect() {
  const std::vector<std::byte> payload{std::byte{0x01}};
  const auto response = autoruntime::RpcClient::Call(
      {"127.0.0.1", 1U}, "unavailable", payload,
      autoruntime::Deadline::After(200ms));
  CHECK(!response);
  CHECK(response.status().code() ==
        autoruntime::StatusCode::TransportError);
  return 0;
}

int RpcCorruptFrame() {
  autoruntime::RpcServerConfig config;
  config.bind_endpoint = {"127.0.0.1", 0U};
  config.max_request_bytes = 1024U;
  auto server = Take(autoruntime::RpcServer::Create(config));
  CHECK(server->Start());

  const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
  CHECK(descriptor >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server->LocalEndpoint().port);
  CHECK(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  CHECK(::connect(
            descriptor, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0);
  const std::array<unsigned char, 28U> corrupt{};
  CHECK(::send(descriptor, corrupt.data(), corrupt.size(), 0) ==
        static_cast<ssize_t>(corrupt.size()));
  static_cast<void>(::close(descriptor));
  CHECK(WaitUntil(
      [&] { return server->Stats().malformed_requests == 1U; }, 1s));
  CHECK(server->Stop());
  return 0;
}

autoruntime::DiscoveryConfig DiscoveryConfig(std::string node) {
  autoruntime::DiscoveryConfig config;
  config.node_id = std::move(node);
  config.generation = 1U;
  config.bind_endpoint = {"127.0.0.1", 0U};
  config.advertised_rpc_endpoint = {"127.0.0.1", 1U};
  config.heartbeat_period = 20ms;
  config.lease_timeout = 150ms;
  config.max_members = 4U;
  config.max_peers = 4U;
  return config;
}

int DuplicateMembership() {
  auto supervisor = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("supervisor")));
  auto first = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("planning")));
  auto duplicate = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("planning")));
  CHECK(first->AddPeer(supervisor->LocalEndpoint()));
  CHECK(duplicate->AddPeer(supervisor->LocalEndpoint()));
  CHECK(supervisor->Start());
  CHECK(first->Start());
  CHECK(duplicate->Start());
  CHECK(WaitUntil(
      [&] {
        return supervisor->Stats().duplicate_announcements > 0U;
      },
      1s));
  CHECK(supervisor->Members().size() == 1U);
  CHECK(first->Stop());
  CHECK(duplicate->Stop());
  CHECK(supervisor->Stop());
  return 0;
}

int ServiceTimeout() {
  auto executor = std::make_shared<autoruntime::Executor>();
  auto transport =
      std::make_shared<autoruntime::InMemoryTransport>();
  const auto group = Take(executor->CreateCallbackGroup(
      {"service", 1U, 4U}));
  autoruntime::Node server_node(
      {"server", 1U}, executor, transport);
  autoruntime::Node client_node(
      {"client", 1U}, executor, transport);
  auto service = server_node.CreateService(
      "slow", group,
      [](const autoruntime::Message&)
          -> autoruntime::Result<std::vector<std::byte>> {
        std::this_thread::sleep_for(100ms);
        return std::vector<std::byte>{std::byte{0x01}};
      });
  auto client = client_node.CreateClient("slow");
  CHECK(service && client);
  CHECK(executor->Start());
  const std::vector<std::byte> payload{std::byte{0x01}};
  const auto response = client.value().Call(
      payload, autoruntime::Deadline::After(10ms));
  CHECK(!response);
  CHECK(response.status().code() == autoruntime::StatusCode::Timeout);
  std::this_thread::sleep_for(110ms);
  CHECK(executor->Stop(autoruntime::Deadline::After(1s)));
  return 0;
}

#if defined(AUTORUNTIME_HAS_DDS)
int DdsParticipantLoss() {
  autoruntime::DdsTransportConfig config;
  config.domain_id =
      static_cast<std::uint32_t>(10 + (::getpid() % 200));
  config.participant_name = "fault-participant";
  auto transport = autoruntime::DdsTransport::Create(config);
  CHECK(transport);
  CHECK(transport.value()->Close());

  autoruntime::Message message;
  message.envelope.trace_id = 1U;
  const auto status = transport.value()->Publish(
      "closed/participant", std::move(message), {});
  CHECK(!status);
  CHECK(status.code() == autoruntime::StatusCode::Closed);
  CHECK(transport.value()->Stats().publish_failures == 1U);
  return 0;
}
#endif

using FaultFunction = int (*)();

struct FaultCase {
  std::string_view name;
  FaultFunction run;
};

constexpr std::array<FaultCase, 14U> kFaultCases{{
    {"transport_closed", TransportClosed},
    {"queue_overflow", QueueOverflow},
    {"slow_callback_isolation", SlowCallbackIsolation},
    {"deadline_miss", DeadlineMiss},
    {"cancelled_task", CancelledTask},
    {"heartbeat_loss", HeartbeatLoss},
    {"no_progress", NoProgress},
    {"backlog_degraded", BacklogDegraded},
    {"process_exit", ProcessExit},
    {"stale_generation", StaleGeneration},
    {"rpc_disconnect", RpcDisconnect},
    {"rpc_corrupt_frame", RpcCorruptFrame},
    {"duplicate_membership", DuplicateMembership},
    {"service_timeout", ServiceTimeout},
}};

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: fault_injection_test CASE\n";
      return 2;
    }
    const std::string_view requested(argv[1]);
#if defined(AUTORUNTIME_HAS_DDS)
    if (requested == "dds_participant_loss") {
      const int result = DdsParticipantLoss();
      if (result == 0) {
        std::cout
            << "{\"fault\":\"dds_participant_loss\",\"status\":\"passed\"}\n";
      }
      return result;
    }
#endif
    const auto iterator = std::find_if(
        kFaultCases.begin(), kFaultCases.end(),
        [&](const FaultCase& fault) { return fault.name == requested; });
    if (iterator == kFaultCases.end()) {
      std::cerr << "unknown fault case: " << requested << '\n';
      return 2;
    }
    const int result = iterator->run();
    if (result == 0) {
      std::cout << "{\"fault\":\"" << requested
                << "\",\"status\":\"passed\"}\n";
    }
    return result;
  } catch (const std::exception& error) {
    std::cerr << "fault case threw: " << error.what() << '\n';
    return 1;
  }
}
