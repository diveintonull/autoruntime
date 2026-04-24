#include "test_support.hpp"

#include <autoruntime/dds_transport.hpp>
#include <autoruntime/node.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

using namespace std::chrono_literals;

namespace {

std::span<const std::byte> ByteView(std::string_view text) {
  return std::as_bytes(std::span(text));
}

std::vector<std::byte> OwnedBytes(std::string_view text) {
  const auto bytes = ByteView(text);
  return {bytes.begin(), bytes.end()};
}

std::string Text(std::span<const std::byte> bytes) {
  return std::string(
      reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

autoruntime::Message Reply(
    autoruntime::Message request, std::string_view payload) {
  request.payload = OwnedBytes(payload);
  request.envelope.parent_span_id = request.envelope.span_id;
  request.envelope.span_id = autoruntime::NextSpanId();
  request.envelope.publish_timestamp_ns =
      autoruntime::MonotonicNanoseconds();
  return request;
}

autoruntime::DdsTransportConfig Config(
    std::string participant_name, std::uint32_t offset) {
  autoruntime::DdsTransportConfig config;
  config.domain_id = static_cast<std::uint32_t>(
      20U + (static_cast<std::uint32_t>(::getpid()) + offset) % 180U);
  config.participant_name = std::move(participant_name);
  config.receive_poll_interval = 1ms;
  config.reliability_max_blocking_time = 250ms;
  return config;
}

autoruntime::Result<autoruntime::Message> CallEventually(
    const autoruntime::Client& client, std::string_view payload,
    autoruntime::TraceContext trace = {}) {
  auto last = autoruntime::Result<autoruntime::Message>(
      autoruntime::Status(
          autoruntime::StatusCode::Timeout,
          "DDS discovery did not complete"));
  const auto end = std::chrono::steady_clock::now() + 4s;
  while (std::chrono::steady_clock::now() < end) {
    last = client.Call(ByteView(payload),
                       autoruntime::Deadline::After(400ms), trace);
    if (last ||
        last.status().code() != autoruntime::StatusCode::Timeout) {
      return last;
    }
    std::this_thread::sleep_for(50ms);
  }
  return last;
}

int ControlServicesUseNodeClientAndService() {
  auto service_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-control-server", 1U));
  auto client_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-control-client", 1U));
  CHECK(service_transport && client_transport);

  auto executor = std::make_shared<autoruntime::Executor>();
  auto group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{
          "dds-rpc-control", 2U, 64U});
  CHECK(group);

  autoruntime::Node server(
      {"dds-rpc-control-server", 7U}, executor,
      service_transport.value());
  autoruntime::Node client(
      {"dds-rpc-control-client", 11U}, executor,
      client_transport.value());

  auto health = server.CreateService(
      "HealthQuery", group.value(),
      [](const autoruntime::Message& request)
          -> autoruntime::Result<std::vector<std::byte>> {
        if (Text(request.payload) != "health?") {
          return autoruntime::Status(
              autoruntime::StatusCode::InvalidArgument,
              "unexpected health query");
        }
        return OwnedBytes("healthy");
      });
  auto runtime_status = server.CreateService(
      "RuntimeStatus", group.value(),
      [](const autoruntime::Message&)
          -> autoruntime::Result<std::vector<std::byte>> {
        return OwnedBytes("running:generation=7");
      });
  auto config_query = server.CreateService(
      "ConfigQuery", group.value(),
      [](const autoruntime::Message& request)
          -> autoruntime::Result<std::vector<std::byte>> {
        if (Text(request.payload) != "worker_count") {
          return autoruntime::Status(
              autoruntime::StatusCode::NotFound,
              "configuration key was not found");
        }
        return OwnedBytes("2");
      });
  CHECK(health && runtime_status && config_query);

  auto health_client = client.CreateClient("HealthQuery");
  auto runtime_client = client.CreateClient("RuntimeStatus");
  auto config_client = client.CreateClient("ConfigQuery");
  CHECK(health_client && runtime_client && config_client);
  CHECK(executor->Start());

  auto health_response = CallEventually(
      health_client.value(), "health?",
      autoruntime::TraceContext{1001U, 2001U, 3001U});
  CHECK(health_response);
  CHECK(Text(health_response.value().payload) == "healthy");
  CHECK(health_response.value().envelope.trace_id == 1001U);

  auto runtime_response =
      CallEventually(runtime_client.value(), "");
  CHECK(runtime_response);
  CHECK(Text(runtime_response.value().payload) ==
        "running:generation=7");

  auto config_response =
      CallEventually(config_client.value(), "worker_count");
  CHECK(config_response);
  CHECK(Text(config_response.value().payload) == "2");

  const auto client_stats = client_transport.value()->Stats();
  CHECK(client_stats.rpc_requests >= 3U);
  CHECK(health.value().Close());
  CHECK(runtime_status.value().Close());
  CHECK(config_query.value().Close());
  CHECK(executor->Stop(autoruntime::Deadline::After(2s)));
  CHECK(client_transport.value()->Close());
  CHECK(service_transport.value()->Close());
  return 0;
}

int TimeoutErrorAndLateResponseStayCorrelated() {
  auto service_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-correlation-server", 2U));
  auto client_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-correlation-client", 2U));
  CHECK(service_transport && client_transport);

  auto service = service_transport.value()->AdvertiseService(
      "RuntimeStatus",
      [](autoruntime::Message request, autoruntime::Deadline)
          -> autoruntime::Result<autoruntime::Message> {
        const auto command = Text(request.payload);
        if (command == "error") {
          return autoruntime::Status(
              autoruntime::StatusCode::NotFound,
              "runtime status is unavailable");
        }
        if (command == "slow") {
          std::this_thread::sleep_for(180ms);
          return Reply(std::move(request), "late-slow-response");
        }
        return Reply(std::move(request), "fast-response");
      });
  CHECK(service);

  auto executor = std::make_shared<autoruntime::Executor>();
  autoruntime::Node client_node(
      {"dds-rpc-correlation-client", 5U}, executor,
      client_transport.value());
  auto client = client_node.CreateClient("RuntimeStatus");
  CHECK(client);
  std::this_thread::sleep_for(600ms);

  auto error =
      client.value().Call(ByteView("error"),
                          autoruntime::Deadline::After(2s));
  CHECK(!error);
  CHECK(error.status().code() == autoruntime::StatusCode::NotFound);
  CHECK(error.status().detail() == "runtime status is unavailable");

  auto timed_out =
      client.value().Call(ByteView("slow"),
                          autoruntime::Deadline::After(30ms));
  CHECK(!timed_out);
  CHECK(timed_out.status().code() == autoruntime::StatusCode::Timeout);

  auto fast =
      client.value().Call(ByteView("fast"),
                          autoruntime::Deadline::After(2s));
  CHECK(fast);
  CHECK(Text(fast.value().payload) == "fast-response");
  CHECK(Text(fast.value().payload) != "late-slow-response");

  const auto stats = client_transport.value()->Stats();
  CHECK(stats.rpc_requests == 3U);
  CHECK(stats.rpc_failures >= 2U);
  CHECK(service_transport.value()->RemoveService(service.value()));
  CHECK(client_transport.value()->Close());
  CHECK(service_transport.value()->Close());
  return 0;
}

int ConcurrentRequestsStayExactlyCorrelated() {
  constexpr std::size_t kRequestCount = 16U;
  auto service_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-concurrent-server", 3U));
  auto client_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-concurrent-client", 3U));
  CHECK(service_transport && client_transport);

  auto service = service_transport.value()->AdvertiseService(
      "ConfigQuery",
      [](autoruntime::Message request, autoruntime::Deadline)
          -> autoruntime::Result<autoruntime::Message> {
        auto response = Text(request.payload);
        response.append("-response");
        return Reply(std::move(request), response);
      });
  CHECK(service);

  auto executor = std::make_shared<autoruntime::Executor>();
  autoruntime::Node client_node(
      {"dds-rpc-concurrent-client", 6U}, executor,
      client_transport.value());
  auto client = client_node.CreateClient("ConfigQuery");
  CHECK(client);
  std::this_thread::sleep_for(600ms);

  std::vector<
      std::future<autoruntime::Result<autoruntime::Message>>>
      calls;
  calls.reserve(kRequestCount);
  for (std::size_t index = 0U;
       index < kRequestCount; ++index) {
    calls.emplace_back(std::async(
        std::launch::async,
        [client = client.value(), index] {
          const auto request =
              "request-" + std::to_string(index);
          return client.Call(
              ByteView(request),
              autoruntime::Deadline::After(3s));
        }));
  }

  for (std::size_t index = 0U;
       index < calls.size(); ++index) {
    auto result = calls[index].get();
    CHECK(result);
    const auto expected =
        "request-" + std::to_string(index) + "-response";
    CHECK(Text(result.value().payload) == expected);
  }
  const auto stats = client_transport.value()->Stats();
  CHECK(stats.rpc_requests == kRequestCount);
  CHECK(stats.rpc_failures == 0U);
  CHECK(service_transport.value()->RemoveService(service.value()));
  CHECK(client_transport.value()->Close());
  CHECK(service_transport.value()->Close());
  return 0;
}

int ValidationAndCloseWakePendingCall() {
  auto service_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-validation-server", 3U));
  CHECK(service_transport);

  auto invalid_name = service_transport.value()->AdvertiseService(
      "", [](autoruntime::Message request, autoruntime::Deadline) {
        return autoruntime::Result<autoruntime::Message>(
            std::move(request));
      });
  CHECK(!invalid_name);
  CHECK(invalid_name.status().code() ==
        autoruntime::StatusCode::InvalidArgument);

  auto invalid_callback =
      service_transport.value()->AdvertiseService("HealthQuery", {});
  CHECK(!invalid_callback);
  CHECK(invalid_callback.status().code() ==
        autoruntime::StatusCode::InvalidArgument);

  auto service = service_transport.value()->AdvertiseService(
      "HealthQuery",
      [](autoruntime::Message request, autoruntime::Deadline) {
        return autoruntime::Result<autoruntime::Message>(
            Reply(std::move(request), "healthy"));
      });
  CHECK(service);
  auto duplicate = service_transport.value()->AdvertiseService(
      "HealthQuery",
      [](autoruntime::Message request, autoruntime::Deadline) {
        return autoruntime::Result<autoruntime::Message>(
            std::move(request));
      });
  CHECK(!duplicate);
  CHECK(duplicate.status().code() ==
        autoruntime::StatusCode::AlreadyExists);
  CHECK(service_transport.value()->RemoveService(service.value()));
  const auto missing_remove =
      service_transport.value()->RemoveService(service.value());
  CHECK(!missing_remove);
  CHECK(missing_remove.code() == autoruntime::StatusCode::NotFound);
  CHECK(service_transport.value()->Close());

  auto client_transport = autoruntime::DdsTransport::Create(
      Config("dds-rpc-close-client", 4U));
  CHECK(client_transport);
  auto pending = std::async(
      std::launch::async,
      [transport = client_transport.value()] {
        autoruntime::Message request;
        request.payload = OwnedBytes("wait-forever");
        return transport->Request(
            "MissingService", std::move(request),
            autoruntime::Deadline::Infinite());
      });
  std::this_thread::sleep_for(200ms);
  CHECK(client_transport.value()->Close());
  CHECK(pending.wait_for(2s) == std::future_status::ready);
  auto result = pending.get();
  CHECK(!result);
  CHECK(result.status().code() == autoruntime::StatusCode::Closed);
  return 0;
}

struct NamedTest {
  std::string_view name;
  int (*run)();
};

constexpr std::array kTests{
    NamedTest{"control_services",
              ControlServicesUseNodeClientAndService},
    NamedTest{"timeout_correlation",
              TimeoutErrorAndLateResponseStayCorrelated},
    NamedTest{"concurrent_correlation",
              ConcurrentRequestsStayExactlyCorrelated},
    NamedTest{"close_wakes_pending",
              ValidationAndCloseWakePendingCall},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--case") {
    const std::string_view requested(argv[2]);
    for (const auto& test : kTests) {
      if (test.name == requested) {
        return test.run();
      }
    }
    return 2;
  }
  if (argc != 1) {
    return 2;
  }
  for (const auto& test : kTests) {
    const int result = test.run();
    if (result != 0) {
      return result;
    }
  }
  return 0;
}
