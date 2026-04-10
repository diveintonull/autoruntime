#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace autoruntime {

inline constexpr std::uint16_t kMessageEnvelopeVersion = 1U;

[[nodiscard]] inline std::uint64_t MonotonicNanoseconds() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] inline std::uint64_t NextTraceId() noexcept {
  static std::atomic<std::uint64_t> next{1U};
  return next.fetch_add(1U, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t NextSpanId() noexcept {
  static std::atomic<std::uint64_t> next{1U};
  return next.fetch_add(1U, std::memory_order_relaxed);
}

struct TraceContext {
  std::uint64_t trace_id{0U};
  std::uint64_t parent_span_id{0U};
  std::uint64_t origin_timestamp_ns{0U};
};

struct MessageEnvelope {
  std::uint16_t version{kMessageEnvelopeVersion};
  std::uint64_t trace_id{0U};
  std::uint64_t span_id{0U};
  std::uint64_t parent_span_id{0U};
  std::uint64_t sequence{0U};
  std::uint64_t source_timestamp_ns{0U};
  std::uint64_t publish_timestamp_ns{0U};
  std::uint64_t source_generation{0U};
  std::int32_t priority{0};
};

struct Message {
  MessageEnvelope envelope;
  std::vector<std::byte> payload;

  [[nodiscard]] TraceContext ContinueTrace() const noexcept {
    return TraceContext{
        envelope.trace_id,
        envelope.span_id,
        envelope.source_timestamp_ns};
  }
};

}  // namespace autoruntime
