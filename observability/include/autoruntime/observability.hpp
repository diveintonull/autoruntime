#pragma once

#include <autoruntime/status.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace autoruntime {

enum class MetricKind {
  Counter,
  Gauge,
  Histogram,
};

struct DistributionSummary {
  std::uint64_t count{0U};
  std::size_t sampled_count{0U};
  double minimum{0.0};
  double maximum{0.0};
  double mean{0.0};
  double p50{0.0};
  double p95{0.0};
  double p99{0.0};
};

struct MetricSample {
  std::string name;
  MetricKind kind{MetricKind::Gauge};
  double value{0.0};
  DistributionSummary distribution;
};

class MetricsRegistry {
 public:
  explicit MetricsRegistry(
      std::size_t maximum_histogram_samples = 8192U);
  ~MetricsRegistry();

  MetricsRegistry(const MetricsRegistry&) = delete;
  MetricsRegistry& operator=(const MetricsRegistry&) = delete;
  MetricsRegistry(MetricsRegistry&&) = delete;
  MetricsRegistry& operator=(MetricsRegistry&&) = delete;

  Status IncrementCounter(std::string_view name, double delta = 1.0);
  Status SetGauge(std::string_view name, double value);
  Status Observe(std::string_view name, double value);

  [[nodiscard]] std::vector<MetricSample> Snapshot() const;
  [[nodiscard]] std::string RenderJson() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class LogLevel {
  Debug,
  Info,
  Warn,
  Error,
};

struct LogRecord {
  LogLevel level{LogLevel::Info};
  std::string component;
  std::string event;
  std::uint64_t generation{0U};
  std::uint64_t trace_id{0U};
  std::uint64_t span_id{0U};
  StatusCode status{StatusCode::Ok};
  std::string detail;
  std::vector<std::pair<std::string, std::string>> attributes;
};

using LogSink = std::function<void(std::string)>;

class StructuredLogger {
 public:
  explicit StructuredLogger(LogSink sink);
  ~StructuredLogger();

  StructuredLogger(const StructuredLogger&) = delete;
  StructuredLogger& operator=(const StructuredLogger&) = delete;
  StructuredLogger(StructuredLogger&&) = delete;
  StructuredLogger& operator=(StructuredLogger&&) = delete;

  Status Log(LogRecord record);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct TraceSpan {
  std::uint64_t trace_id{0U};
  std::uint64_t span_id{0U};
  std::uint64_t parent_span_id{0U};
  std::uint64_t generation{0U};
  std::string component;
  std::string operation;
  std::uint64_t start_monotonic_ns{0U};
  std::uint64_t finish_monotonic_ns{0U};
  StatusCode status{StatusCode::Ok};
};

class TraceRecorder {
 public:
  TraceRecorder();
  ~TraceRecorder();

  TraceRecorder(const TraceRecorder&) = delete;
  TraceRecorder& operator=(const TraceRecorder&) = delete;
  TraceRecorder(TraceRecorder&&) = delete;
  TraceRecorder& operator=(TraceRecorder&&) = delete;

  Status Record(TraceSpan span);
  [[nodiscard]] std::vector<TraceSpan> Snapshot(
      std::uint64_t trace_id = 0U) const;
  [[nodiscard]] DistributionSummary
  EndToEndLatencyMicroseconds() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct ProcessCpuMetrics {
  bool available{false};
  double user_seconds{0.0};
  double system_seconds{0.0};
  std::uint64_t maximum_resident_bytes{0U};
  std::uint64_t voluntary_context_switches{0U};
  std::uint64_t involuntary_context_switches{0U};
};

[[nodiscard]] ProcessCpuMetrics SampleProcessCpu() noexcept;
[[nodiscard]] std::string_view MetricKindName(MetricKind kind) noexcept;
[[nodiscard]] std::string_view LogLevelName(LogLevel level) noexcept;
[[nodiscard]] std::string_view StatusCodeName(StatusCode code) noexcept;

}  // namespace autoruntime
