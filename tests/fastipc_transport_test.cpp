#include "test_support.hpp"

#include <autoruntime/fastipc_transport.hpp>
#include <autoruntime/node.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

struct Observation {
  std::uint64_t trace_id{0U};
  std::uint64_t sequence{0U};
  std::uint64_t source_generation{0U};
  std::uint32_t payload_size{0U};
  std::array<char, 32> payload{};
};

bool WriteExact(int descriptor, const void* data, std::size_t size) {
  const auto* cursor = static_cast<const std::byte*>(data);
  while (size > 0U) {
    const auto written = ::write(descriptor, cursor, size);
    if (written <= 0) return false;
    const auto count = static_cast<std::size_t>(written);
    cursor += count;
    size -= count;
  }
  return true;
}

bool ReadExact(int descriptor, void* data, std::size_t size,
               std::chrono::milliseconds timeout) {
  auto* cursor = static_cast<std::byte*>(data);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (size > 0U) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return false;
    const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining);
    pollfd descriptor_event{descriptor, POLLIN, 0};
    const int result =
        ::poll(&descriptor_event, 1, static_cast<int>(wait_ms.count()));
    if (result <= 0) return false;
    const auto count = ::read(descriptor, cursor, size);
    if (count <= 0) return false;
    const auto converted = static_cast<std::size_t>(count);
    cursor += converted;
    size -= converted;
  }
  return true;
}

autoruntime::FastIpcTransportConfig Config(
    std::string channel, autoruntime::FastIpcDirection direction,
    bool unlink_on_close) {
  autoruntime::FastIpcTransportConfig config;
  config.open_timeout = 2s;
  config.endpoints.push_back(autoruntime::FastIpcEndpointConfig{
      "sensor/frame", std::move(channel), direction, 8U, 4096U, 500ms,
      unlink_on_close});
  return config;
}

int CrossProcessNodeApiPreservesEnvelope() {
  int parent_to_child[2]{-1, -1};
  int child_to_parent[2]{-1, -1};
  CHECK(::pipe(parent_to_child) == 0);
  CHECK(::pipe(child_to_parent) == 0);

  const std::string channel =
      "autoruntime_fastipc_" + std::to_string(::getpid());
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    ::close(parent_to_child[1]);
    ::close(child_to_parent[0]);
    char start = 0;
    if (!ReadExact(parent_to_child[0], &start, 1U, 3s)) std::_Exit(10);

    auto transport_result = autoruntime::FastIpcTransport::Create(
        Config(channel, autoruntime::FastIpcDirection::Subscribe, false));
    if (!transport_result) std::_Exit(11);
    auto transport = transport_result.value();
    auto executor = std::make_shared<autoruntime::Executor>();
    auto group = executor->CreateCallbackGroup(
        autoruntime::CallbackGroupConfig{"receiver", 1U, 16U});
    if (!group) std::_Exit(12);

    autoruntime::Node node({"perception", 2U}, executor, transport);
    Observation observation;
    std::atomic<bool> received{false};
    auto subscription = node.CreateSubscriber(
        "sensor/frame",
        autoruntime::SubscriptionOptions{
            group.value(), 4U, autoruntime::OverflowPolicy::DropNewest},
        [&](const autoruntime::Message& message) {
          observation.trace_id = message.envelope.trace_id;
          observation.sequence = message.envelope.sequence;
          observation.source_generation =
              message.envelope.source_generation;
          observation.payload_size =
              static_cast<std::uint32_t>(message.payload.size());
          if (message.payload.size() <= observation.payload.size()) {
            std::memcpy(observation.payload.data(), message.payload.data(),
                        message.payload.size());
          }
          received.store(true, std::memory_order_release);
        });
    if (!subscription || !executor->Start()) std::_Exit(13);

    const char ready = 'R';
    if (!WriteExact(child_to_parent[1], &ready, 1U)) std::_Exit(14);
    if (!WaitUntil(
            [&] { return received.load(std::memory_order_acquire); }, 3s)) {
      std::_Exit(15);
    }
    if (!executor->Stop(autoruntime::Deadline::After(1s))) std::_Exit(16);
    if (!WriteExact(child_to_parent[1], &observation,
                    sizeof(observation))) {
      std::_Exit(17);
    }
    std::_Exit(0);
  }

  ::close(parent_to_child[0]);
  ::close(child_to_parent[1]);
  auto transport_result = autoruntime::FastIpcTransport::Create(
      Config(channel, autoruntime::FastIpcDirection::Publish, true));
  CHECK(transport_result);
  auto transport = transport_result.value();
  auto executor = std::make_shared<autoruntime::Executor>();
  autoruntime::Node node({"camera", 9U}, executor, transport);
  auto publisher = node.CreatePublisher("sensor/frame");
  CHECK(publisher);

  const char start = 'S';
  CHECK(WriteExact(parent_to_child[1], &start, 1U));
  char ready = 0;
  CHECK(ReadExact(child_to_parent[0], &ready, 1U, 3s));
  CHECK(ready == 'R');

  const std::string_view text = "frame-42";
  const auto payload = std::as_bytes(std::span(text));
  CHECK(publisher.value().Publish(payload));

  Observation observation;
  CHECK(ReadExact(child_to_parent[0], &observation,
                  sizeof(observation), 4s));
  int status = 0;
  CHECK(::waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(observation.trace_id != 0U);
  CHECK(observation.sequence == 1U);
  CHECK(observation.source_generation == 9U);
  CHECK(observation.payload_size == text.size());
  CHECK(std::string_view(observation.payload.data(),
                         observation.payload_size) == text);

  const auto stats = transport->Stats();
  CHECK(stats.published_messages == 1U);
  CHECK(stats.publish_failures == 0U);
  CHECK(transport->Close());
  ::close(parent_to_child[1]);
  ::close(child_to_parent[0]);
  return 0;
}

}  // namespace

int main() { return CrossProcessNodeApiPreservesEnvelope(); }
