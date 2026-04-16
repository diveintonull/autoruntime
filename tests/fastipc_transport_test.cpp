#include "test_support.hpp"

#include <autoruntime/fastipc_transport.hpp>
#include <autoruntime/health_monitor.hpp>
#include <autoruntime/node.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <span>
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
  std::uint64_t receiver_generation{0U};
  std::uint32_t payload_size{0U};
  std::array<char, 32> payload{};
};

struct SubscriberChild {
  pid_t process{-1};
  int observation_fd{-1};
};

bool WriteExact(int descriptor, const void* data, std::size_t size) {
  const auto* cursor = static_cast<const std::byte*>(data);
  while (size > 0U) {
    const auto written = ::write(descriptor, cursor, size);
    if (written <= 0) {
      return false;
    }
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
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      return false;
    }
    const auto wait_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    pollfd descriptor_event{descriptor, POLLIN, 0};
    const int result =
        ::poll(&descriptor_event, 1, static_cast<int>(wait_ms.count()));
    if (result <= 0) {
      return false;
    }
    const auto count = ::read(descriptor, cursor, size);
    if (count <= 0) {
      return false;
    }
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

int SubscriberProcess(std::string channel, int observation_fd,
                      std::uint64_t generation, bool hold_after_message) {
  auto transport_result = autoruntime::FastIpcTransport::Create(
      Config(std::move(channel),
             autoruntime::FastIpcDirection::Subscribe, false));
  if (!transport_result) {
    return 11;
  }
  auto transport = transport_result.value();
  auto executor = std::make_shared<autoruntime::Executor>();
  auto group = executor->CreateCallbackGroup(
      autoruntime::CallbackGroupConfig{"receiver", 1U, 16U});
  if (!group) {
    return 12;
  }

  autoruntime::Node node({"planning", generation}, executor, transport);
  Observation observation;
  observation.receiver_generation = generation;
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
  if (!subscription || !executor->Start()) {
    return 13;
  }

  const char ready = 'R';
  if (!WriteExact(observation_fd, &ready, 1U)) {
    return 14;
  }
  if (!WaitUntil(
          [&] { return received.load(std::memory_order_acquire); }, 4s)) {
    return 15;
  }
  if (!WriteExact(observation_fd, &observation, sizeof(observation))) {
    return 16;
  }

  if (hold_after_message) {
    for (;;) {
      ::pause();
    }
  }

  if (!subscription.value().Close()) {
    return 17;
  }
  if (!executor->Stop(autoruntime::Deadline::After(1s))) {
    return 18;
  }
  if (!transport->Close()) {
    return 19;
  }
  static_cast<void>(::close(observation_fd));
  return 0;
}

SubscriberChild SpawnSubscriber(const std::string& executable,
                                const std::string& channel,
                                std::uint64_t generation,
                                bool hold_after_message) {
  int response_pipe[2]{-1, -1};
  if (::pipe(response_pipe) != 0) {
    return {};
  }

  const auto descriptor_text = std::to_string(response_pipe[1]);
  const auto generation_text = std::to_string(generation);
  const std::string hold_text = hold_after_message ? "1" : "0";
  const pid_t process = ::fork();
  if (process < 0) {
    static_cast<void>(::close(response_pipe[0]));
    static_cast<void>(::close(response_pipe[1]));
    return {};
  }
  if (process == 0) {
    static_cast<void>(::close(response_pipe[0]));
    ::execl(executable.c_str(), executable.c_str(),
            "--recovery-subscriber", channel.c_str(),
            descriptor_text.c_str(), generation_text.c_str(),
            hold_text.c_str(), static_cast<char*>(nullptr));
    std::_Exit(127);
  }

  static_cast<void>(::close(response_pipe[1]));
  return SubscriberChild{process, response_pipe[0]};
}

void CleanupChild(SubscriberChild& child) {
  if (child.process > 0) {
    static_cast<void>(::kill(child.process, SIGKILL));
    int status = 0;
    static_cast<void>(::waitpid(child.process, &status, 0));
    child.process = -1;
  }
  if (child.observation_fd >= 0) {
    static_cast<void>(::close(child.observation_fd));
    child.observation_fd = -1;
  }
}

bool WaitReady(const SubscriberChild& child) {
  char ready = 0;
  return ReadExact(child.observation_fd, &ready, 1U, 4s) && ready == 'R';
}

bool Matches(const Observation& observation, std::string_view payload,
             std::uint64_t receiver_generation,
             std::uint64_t sequence) {
  return observation.trace_id != 0U &&
         observation.sequence == sequence &&
         observation.source_generation == 9U &&
         observation.receiver_generation == receiver_generation &&
         observation.payload_size == payload.size() &&
         std::string_view(observation.payload.data(),
                          observation.payload_size) == payload;
}

int Kill9RecoveryRestoresFastIpcFlow(const std::string& executable) {
  const std::string channel =
      "autoruntime_fastipc_recovery_" + std::to_string(::getpid());
  auto transport_result = autoruntime::FastIpcTransport::Create(
      Config(channel, autoruntime::FastIpcDirection::Publish, true));
  CHECK(transport_result);
  auto transport = transport_result.value();
  auto executor = std::make_shared<autoruntime::Executor>();
  autoruntime::Node camera({"camera", 9U}, executor, transport);
  auto publisher = camera.CreatePublisher("sensor/frame");
  CHECK(publisher);

  SubscriberChild initial =
      SpawnSubscriber(executable, channel, 7U, true);
  CHECK(initial.process > 0);
  CHECK(initial.observation_fd >= 0);
  CHECK(WaitReady(initial));

  const std::string_view before_text = "before-crash";
  CHECK(publisher.value().Publish(std::as_bytes(std::span(before_text))));
  Observation before;
  CHECK(ReadExact(initial.observation_fd, &before, sizeof(before), 4s));
  CHECK(Matches(before, before_text, 7U, 1U));

  autoruntime::HealthPolicy policy;
  policy.restart_backoff = 10ms;
  policy.recovery_timeout = 3s;
  autoruntime::HealthMonitor monitor;
  const auto observed_at = autoruntime::HealthMonitor::Clock::now();
  CHECK(monitor.Register(
      {"planning", initial.process, 7U, policy}, observed_at));
  CHECK(monitor.Heartbeat("planning", 7U, 1U, observed_at));
  CHECK(monitor.Progress("planning", 7U, 1U, observed_at));

  CHECK(::kill(initial.process, SIGKILL) == 0);
  int initial_status = 0;
  CHECK(::waitpid(initial.process, &initial_status, 0) == initial.process);
  CHECK(WIFSIGNALED(initial_status));
  CHECK(WTERMSIG(initial_status) == SIGKILL);
  initial.process = -1;
  static_cast<void>(::close(initial.observation_fd));
  initial.observation_fd = -1;

  static_cast<void>(
      monitor.Evaluate(autoruntime::HealthMonitor::Clock::now()));
  const auto failed = monitor.Snapshot("planning");
  CHECK(failed);
  CHECK(failed.value().state == autoruntime::HealthState::Failed);
  CHECK(failed.value().reason ==
        autoruntime::HealthReason::ProcessExited);

  SubscriberChild replacement;
  bool cleanup_ran = false;
  bool reconnect_ran = false;
  bool flow_restored = false;
  autoruntime::RecoveryHooks hooks;
  hooks.cleanup = [&](std::string_view name,
                      std::uint64_t old_generation) {
    cleanup_ran = name == "planning" && old_generation == 7U;
    return autoruntime::Status::Ok();
  };
  hooks.start = [&](std::string_view name,
                    std::uint64_t new_generation)
      -> autoruntime::Result<std::int64_t> {
    if (name != "planning" || new_generation != 8U) {
      return autoruntime::Status(
          autoruntime::StatusCode::Internal,
          "unexpected replacement generation");
    }
    replacement =
        SpawnSubscriber(executable, channel, new_generation, false);
    if (replacement.process <= 0 || !WaitReady(replacement)) {
      CleanupChild(replacement);
      return autoruntime::Status(
          autoruntime::StatusCode::Internal,
          "replacement subscriber did not become ready");
    }
    return static_cast<std::int64_t>(replacement.process);
  };
  hooks.reconnect = [&](std::string_view name,
                        std::uint64_t new_generation) {
    reconnect_ran = name == "planning" && new_generation == 8U;
    const std::string_view after_text = "after-restart";
    const auto publish_status = publisher.value().Publish(
        std::as_bytes(std::span(after_text)));
    if (!publish_status) {
      return publish_status;
    }
    Observation after;
    if (!ReadExact(
            replacement.observation_fd, &after, sizeof(after), 4s) ||
        !Matches(after, after_text, 8U, 2U)) {
      return autoruntime::Status(
          autoruntime::StatusCode::Internal,
          "message flow did not recover on generation 8");
    }
    flow_restored = true;
    return autoruntime::Status::Ok();
  };

  auto recovered = monitor.Recover(
      "planning", hooks, autoruntime::Deadline::After(4s));
  if (!recovered) {
    CleanupChild(replacement);
  }
  CHECK(recovered);
  CHECK(cleanup_ran && reconnect_ran && flow_restored);
  CHECK(recovered.value().generation == 8U);
  CHECK(recovered.value().process_id == replacement.process);
  CHECK(monitor.Heartbeat("planning", 8U, 1U));
  CHECK(monitor.Progress("planning", 8U, 1U));
  CHECK(monitor.Snapshot("planning").value().state ==
        autoruntime::HealthState::Running);

  int replacement_status = 0;
  CHECK(::waitpid(replacement.process, &replacement_status, 0) ==
        replacement.process);
  CHECK(WIFEXITED(replacement_status));
  CHECK(WEXITSTATUS(replacement_status) == 0);
  replacement.process = -1;
  static_cast<void>(::close(replacement.observation_fd));
  replacement.observation_fd = -1;

  const auto stats = transport->Stats();
  CHECK(stats.published_messages == 2U);
  CHECK(stats.publish_failures == 0U);
  CHECK(transport->Close());
  CleanupChild(initial);
  CleanupChild(replacement);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 6 &&
      std::string_view(argv[1]) == "--recovery-subscriber") {
    const int descriptor = std::stoi(argv[3]);
    const auto generation =
        static_cast<std::uint64_t>(std::stoull(argv[4]));
    return SubscriberProcess(
        argv[2], descriptor, generation,
        std::string_view(argv[5]) == "1");
  }
  return Kill9RecoveryRestoresFastIpcFlow(argv[0]);
}
