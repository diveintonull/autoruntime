#include "test_support.hpp"

#include <autoruntime/distributed.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {

template <typename T>
T Take(autoruntime::Result<T> result) {
  if (!result) {
    throw std::runtime_error(result.status().detail());
  }
  return std::move(result).take_value();
}

autoruntime::DiscoveryConfig DiscoveryConfig(
    std::string node_id, std::uint64_t generation,
    std::chrono::milliseconds lease = 150ms,
    std::size_t max_members = 8U) {
  autoruntime::DiscoveryConfig config;
  config.node_id = std::move(node_id);
  config.generation = generation;
  config.bind_endpoint = {"127.0.0.1", 0U};
  config.advertised_rpc_endpoint = {"127.0.0.1", 1U};
  config.heartbeat_period = 20ms;
  config.lease_timeout = lease;
  config.max_members = max_members;
  config.max_peers = 8U;
  return config;
}

int GenerationCapacityAndLeaseAreBounded() {
  auto supervisor = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("supervisor", 1U, 120ms, 1U)));
  CHECK(supervisor->Start());

  auto planning_v1 = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("planning", 1U, 120ms)));
  CHECK(planning_v1->AddPeer(supervisor->LocalEndpoint()));
  CHECK(planning_v1->Start());
  CHECK(WaitUntil(
      [&] {
        auto member = supervisor->Find("planning");
        return member && member.value().generation == 1U;
      },
      1s));

  auto planning_v2 = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("planning", 2U, 120ms)));
  CHECK(planning_v2->AddPeer(supervisor->LocalEndpoint()));
  CHECK(planning_v2->Start());
  CHECK(WaitUntil(
      [&] {
        auto member = supervisor->Find("planning");
        return member && member.value().generation == 2U;
      },
      1s));
  CHECK(WaitUntil(
      [&] { return supervisor->Stats().stale_announcements > 0U; },
      1s));

  auto control = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("control", 1U, 120ms)));
  CHECK(control->AddPeer(supervisor->LocalEndpoint()));
  CHECK(control->Start());
  CHECK(WaitUntil(
      [&] { return supervisor->Stats().capacity_drops > 0U; },
      1s));
  CHECK(supervisor->Members().size() == 1U);
  CHECK(!supervisor->Find("control"));
  CHECK(supervisor->Find("control").status().code() ==
        autoruntime::StatusCode::NotFound);

  CHECK(control->Stop());
  CHECK(planning_v1->Stop());
  CHECK(planning_v2->Stop());
  CHECK(WaitUntil(
      [&] { return !supervisor->Find("planning"); },
      1s));
  CHECK(supervisor->Stats().expired_members >= 1U);
  CHECK(supervisor->Stop());
  return 0;
}

int ExpiredGenerationCannotResurrectWithinFenceWindow() {
  auto supervisor_config =
      DiscoveryConfig("fence-supervisor", 1U, 120ms);
  supervisor_config.generation_fence_timeout = 500ms;
  supervisor_config.max_generation_fences = 4U;
  auto supervisor = Take(
      autoruntime::DiscoveryService::Create(supervisor_config));
  CHECK(supervisor->Start());

  auto current = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("planning-fenced", 2U, 120ms)));
  CHECK(current->AddPeer(supervisor->LocalEndpoint()));
  CHECK(current->Start());
  CHECK(WaitUntil(
      [&] {
        const auto member = supervisor->Find("planning-fenced");
        return member && member.value().generation == 2U;
      },
      1s));

  auto stale = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("planning-fenced", 1U, 120ms)));
  CHECK(stale->AddPeer(supervisor->LocalEndpoint()));
  CHECK(stale->Start());
  CHECK(WaitUntil(
      [&] { return supervisor->Stats().stale_announcements > 0U; },
      1s));

  CHECK(current->Stop());
  CHECK(WaitUntil(
      [&] { return supervisor->Stats().expired_members >= 1U; },
      1s));
  const auto stale_before =
      supervisor->Stats().stale_announcements;
  std::this_thread::sleep_for(100ms);
  CHECK(!supervisor->Find("planning-fenced"));
  const auto fenced_stats = supervisor->Stats();
  CHECK(fenced_stats.generation_fences_created >= 1U);
  CHECK(fenced_stats.generation_fence_rejections > 0U);
  CHECK(fenced_stats.stale_announcements > stale_before);

  auto restarted = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("planning-fenced", 3U, 120ms)));
  CHECK(restarted->AddPeer(supervisor->LocalEndpoint()));
  CHECK(restarted->Start());
  CHECK(WaitUntil(
      [&] {
        const auto member = supervisor->Find("planning-fenced");
        return member && member.value().generation == 3U;
      },
      1s));

  CHECK(stale->Stop());
  CHECK(restarted->Stop());
  CHECK(supervisor->Stop());
  return 0;
}

int GenerationFenceStateIsBoundedAndExpires() {
  auto invalid_timeout =
      DiscoveryConfig("invalid-fence-timeout", 1U, 120ms);
  invalid_timeout.generation_fence_timeout = 100ms;
  const auto timeout_result =
      autoruntime::DiscoveryService::Create(invalid_timeout);
  CHECK(!timeout_result);
  CHECK(timeout_result.status().code() ==
        autoruntime::StatusCode::InvalidArgument);

  auto invalid_capacity =
      DiscoveryConfig("invalid-fence-capacity", 1U, 120ms);
  invalid_capacity.max_generation_fences = 0U;
  const auto capacity_result =
      autoruntime::DiscoveryService::Create(invalid_capacity);
  CHECK(!capacity_result);
  CHECK(capacity_result.status().code() ==
        autoruntime::StatusCode::InvalidArgument);

  auto supervisor_config =
      DiscoveryConfig("bounded-fence-supervisor", 1U, 80ms);
  supervisor_config.generation_fence_timeout = 500ms;
  supervisor_config.max_generation_fences = 1U;
  auto supervisor = Take(
      autoruntime::DiscoveryService::Create(supervisor_config));
  CHECK(supervisor->Start());

  auto first = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("fenced-first", 2U, 80ms)));
  CHECK(first->AddPeer(supervisor->LocalEndpoint()));
  CHECK(first->Start());
  CHECK(WaitUntil(
      [&] { return supervisor->Find("fenced-first").ok(); }, 1s));
  CHECK(first->Stop());
  CHECK(WaitUntil(
      [&] { return supervisor->Stats().expired_members >= 1U; },
      1s));

  auto second = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("fenced-second", 2U, 80ms)));
  CHECK(second->AddPeer(supervisor->LocalEndpoint()));
  CHECK(second->Start());
  CHECK(WaitUntil(
      [&] { return supervisor->Find("fenced-second").ok(); }, 1s));
  CHECK(second->Stop());
  CHECK(WaitUntil(
      [&] { return supervisor->Stats().expired_members >= 2U; },
      1s));

  const auto bounded_stats = supervisor->Stats();
  CHECK(bounded_stats.generation_fences_created >= 2U);
  CHECK(bounded_stats.generation_fence_evictions >= 1U);
  CHECK(WaitUntil(
      [&] {
        return supervisor->Stats().generation_fences_expired >= 1U;
      },
      1s));

  CHECK(supervisor->Stop());
  return 0;
}

struct ChildGuard {
  pid_t process{-1};

  ~ChildGuard() {
    if (process > 0) {
      static_cast<void>(::kill(process, SIGKILL));
      int status = 0;
      static_cast<void>(::waitpid(process, &status, 0));
    }
  }

  void Reaped() { process = -1; }
};

struct ChildPorts {
  std::uint16_t discovery{0U};
  std::uint16_t rpc{0U};
};

bool WriteAll(int descriptor, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::byte*>(data);
  std::size_t offset = 0U;
  while (offset < size) {
    const auto written =
        ::write(descriptor, bytes + offset, size - offset);
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool ReadAll(int descriptor, void* data, std::size_t size) {
  auto* bytes = static_cast<std::byte*>(data);
  std::size_t offset = 0U;
  while (offset < size) {
    const auto received =
        ::read(descriptor, bytes + offset, size - offset);
    if (received <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

[[noreturn]] void RunPlanningChild(int to_parent, int from_parent) {
  try {
    autoruntime::RpcServerConfig rpc_config;
    rpc_config.bind_endpoint = {"127.0.0.1", 0U};
    rpc_config.max_request_bytes = 1024U;
    rpc_config.io_timeout = 1s;
    auto rpc = Take(autoruntime::RpcServer::Create(rpc_config));
    const auto registration = rpc->RegisterHandler(
        "planning.echo",
        [](std::span<const std::byte> request)
            -> autoruntime::Result<std::vector<std::byte>> {
          std::vector<std::byte> response(request.begin(), request.end());
          std::reverse(response.begin(), response.end());
          return response;
        });
    if (!registration || !rpc->Start()) {
      _exit(2);
    }

    auto discovery_config = DiscoveryConfig("planning-process", 7U);
    discovery_config.advertised_rpc_endpoint = rpc->LocalEndpoint();
    auto discovery =
        Take(autoruntime::DiscoveryService::Create(discovery_config));
    const ChildPorts ports{
        discovery->LocalEndpoint().port, rpc->LocalEndpoint().port};
    if (!WriteAll(to_parent, &ports, sizeof(ports))) {
      _exit(3);
    }
    std::uint16_t parent_port = 0U;
    if (!ReadAll(from_parent, &parent_port, sizeof(parent_port)) ||
        parent_port == 0U ||
        !discovery->AddPeer({"127.0.0.1", parent_port}) ||
        !discovery->Start()) {
      _exit(4);
    }
    for (;;) {
      ::pause();
    }
  } catch (...) {
    _exit(5);
  }
}

int ForkedDiscoveryAndRpcRoundTrip() {
  int child_to_parent[2]{-1, -1};
  int parent_to_child[2]{-1, -1};
  CHECK(::pipe(child_to_parent) == 0);
  CHECK(::pipe(parent_to_child) == 0);

  ChildGuard child{::fork()};
  CHECK(child.process >= 0);
  if (child.process == 0) {
    static_cast<void>(::close(child_to_parent[0]));
    static_cast<void>(::close(parent_to_child[1]));
    RunPlanningChild(child_to_parent[1], parent_to_child[0]);
  }

  static_cast<void>(::close(child_to_parent[1]));
  static_cast<void>(::close(parent_to_child[0]));
  ChildPorts ports;
  CHECK(ReadAll(child_to_parent[0], &ports, sizeof(ports)));
  CHECK(ports.discovery != 0U && ports.rpc != 0U);

  auto supervisor = Take(
      autoruntime::DiscoveryService::Create(
          DiscoveryConfig("supervisor-process", 1U)));
  CHECK(supervisor->AddPeer({"127.0.0.1", ports.discovery}));
  const auto supervisor_port = supervisor->LocalEndpoint().port;
  CHECK(WriteAll(parent_to_child[1], &supervisor_port,
                 sizeof(supervisor_port)));
  CHECK(supervisor->Start());

  autoruntime::MemberRecord planning;
  CHECK(WaitUntil(
      [&] {
        auto found = supervisor->Find("planning-process");
        if (!found) {
          return false;
        }
        planning = found.value();
        return planning.generation == 7U &&
               planning.rpc_endpoint.port == ports.rpc;
      },
      2s));

  const std::vector<std::byte> request{
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  auto response = autoruntime::RpcClient::Call(
      planning.rpc_endpoint, "planning.echo", request,
      autoruntime::Deadline::After(1s));
  CHECK(response);
  CHECK(response.value() ==
        std::vector<std::byte>(
            {std::byte{0x03}, std::byte{0x02}, std::byte{0x01}}));

  CHECK(::kill(child.process, SIGKILL) == 0);
  int status = 0;
  CHECK(::waitpid(child.process, &status, 0) == child.process);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  child.Reaped();
  CHECK(WaitUntil(
      [&] { return !supervisor->Find("planning-process"); },
      2s));
  CHECK(supervisor->Stats().expired_members >= 1U);
  CHECK(supervisor->Stop());
  static_cast<void>(::close(child_to_parent[0]));
  static_cast<void>(::close(parent_to_child[1]));
  return 0;
}

int RpcDeadlineCancellationAndMissingMethod() {
  autoruntime::RpcServerConfig config;
  config.bind_endpoint = {"127.0.0.1", 0U};
  config.max_request_bytes = 1024U;
  config.io_timeout = 1s;
  auto server = Take(autoruntime::RpcServer::Create(config));
  CHECK(server->RegisterHandler(
      "slow",
      [](std::span<const std::byte>)
          -> autoruntime::Result<std::vector<std::byte>> {
        std::this_thread::sleep_for(150ms);
        return std::vector<std::byte>{std::byte{0x01}};
      }));
  CHECK(server->Start());

  const std::vector<std::byte> payload{std::byte{0x2A}};
  auto timed_out = autoruntime::RpcClient::Call(
      server->LocalEndpoint(), "slow", payload,
      autoruntime::Deadline::After(20ms));
  CHECK(!timed_out);
  CHECK(timed_out.status().code() == autoruntime::StatusCode::Timeout);

  std::stop_source cancellation;
  cancellation.request_stop();
  auto cancelled = autoruntime::RpcClient::Call(
      server->LocalEndpoint(), "slow", payload,
      autoruntime::Deadline::After(1s), cancellation.get_token());
  CHECK(!cancelled);
  CHECK(cancelled.status().code() == autoruntime::StatusCode::Cancelled);

  auto missing = autoruntime::RpcClient::Call(
      server->LocalEndpoint(), "missing", payload,
      autoruntime::Deadline::After(1s));
  CHECK(!missing);
  CHECK(missing.status().code() == autoruntime::StatusCode::NotFound);
  CHECK(WaitUntil(
      [&] { return server->Stats().requests >= 2U; }, 1s));
  CHECK(server->Stop());
  return 0;
}

}  // namespace

int main() {
  if (const int result = GenerationCapacityAndLeaseAreBounded();
      result != 0) {
    return result;
  }
  if (const int result =
          ExpiredGenerationCannotResurrectWithinFenceWindow();
      result != 0) {
    return result;
  }
  if (const int result = GenerationFenceStateIsBoundedAndExpires();
      result != 0) {
    return result;
  }
  if (const int result = ForkedDiscoveryAndRpcRoundTrip();
      result != 0) {
    return result;
  }
  return RpcDeadlineCancellationAndMissingMethod();
}
