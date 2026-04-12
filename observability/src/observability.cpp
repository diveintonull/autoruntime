#include <autoruntime/observability.hpp>

#include <autoruntime/message.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace autoruntime {
namespace {

[[nodiscard]] std::string JsonEscape(std::string_view value) {
  std::ostringstream output;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20U) {
          output << "\\u" << std::hex << std::setw(4)
                 << std::setfill('0')
                 << static_cast<unsigned int>(character) << std::dec
                 << std::setfill(' ');
        } else {
          output << static_cast<char>(character);
        }
        break;
    }
  }
  return output.str();
}

[[nodiscard]] double Percentile(std::vector<double> values,
                                double quantile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const auto rank = static_cast<std::size_t>(
      std::ceil(quantile * static_cast<double>(values.size())));
  const auto index =
      std::min(values.size() - 1U, rank == 0U ? 0U : rank - 1U);
  return values[index];
}

[[nodiscard]] DistributionSummary Summarize(
    const std::vector<double>& values) {
  DistributionSummary result;
  if (values.empty()) {
    return result;
  }
  result.count = static_cast<std::uint64_t>(values.size());
  result.sampled_count = values.size();
  const auto [minimum, maximum] =
      std::minmax_element(values.begin(), values.end());
  result.minimum = *minimum;
  result.maximum = *maximum;
  double sum = 0.0;
  for (const auto value : values) {
    sum += value;
  }
  result.mean = sum / static_cast<double>(values.size());
  result.p50 = Percentile(values, 0.50);
  result.p95 = Percentile(values, 0.95);
  result.p99 = Percentile(values, 0.99);
  return result;
}

[[nodiscard]] std::uint64_t SystemNanoseconds() noexcept {
  const auto since_epoch =
      std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          since_epoch)
          .count());
}

}  // namespace

struct MetricsRegistry::Impl {
  struct Entry {
    explicit Entry(MetricKind configured_kind)
        : kind(configured_kind) {}

    MetricKind kind{MetricKind::Gauge};
    double value{0.0};
    std::uint64_t count{0U};
    double sum{0.0};
    double minimum{0.0};
    double maximum{0.0};
    std::deque<double> samples;
  };

  explicit Impl(std::size_t maximum_samples)
      : maximum_histogram_samples(maximum_samples) {}

  mutable std::mutex mutex;
  std::size_t maximum_histogram_samples{0U};
  std::unordered_map<std::string, Entry> entries;

  [[nodiscard]] Status CheckKindLocked(
      std::string_view name, MetricKind expected) const {
    const auto iterator = entries.find(std::string(name));
    if (iterator != entries.end() &&
        iterator->second.kind != expected) {
      return Status(StatusCode::AlreadyExists,
                    "metric name already has a different kind");
    }
    return Status::Ok();
  }
};

MetricsRegistry::MetricsRegistry(
    std::size_t maximum_histogram_samples)
    : impl_(std::make_unique<Impl>(maximum_histogram_samples)) {}

MetricsRegistry::~MetricsRegistry() = default;

Status MetricsRegistry::IncrementCounter(std::string_view name,
                                         double delta) {
  if (name.empty() || !std::isfinite(delta) || delta < 0.0) {
    return Status(StatusCode::InvalidArgument,
                  "counter requires a name and nonnegative finite delta");
  }
  std::lock_guard lock(impl_->mutex);
  const auto kind_status =
      impl_->CheckKindLocked(name, MetricKind::Counter);
  if (!kind_status) {
    return kind_status;
  }
  auto [iterator, inserted] = impl_->entries.try_emplace(
      std::string(name),
      Impl::Entry(MetricKind::Counter));
  static_cast<void>(inserted);
  iterator->second.value += delta;
  return Status::Ok();
}

Status MetricsRegistry::SetGauge(std::string_view name, double value) {
  if (name.empty() || !std::isfinite(value)) {
    return Status(StatusCode::InvalidArgument,
                  "gauge requires a name and finite value");
  }
  std::lock_guard lock(impl_->mutex);
  const auto kind_status =
      impl_->CheckKindLocked(name, MetricKind::Gauge);
  if (!kind_status) {
    return kind_status;
  }
  auto [iterator, inserted] = impl_->entries.try_emplace(
      std::string(name), Impl::Entry(MetricKind::Gauge));
  static_cast<void>(inserted);
  iterator->second.value = value;
  return Status::Ok();
}

Status MetricsRegistry::Observe(std::string_view name, double value) {
  if (name.empty() || !std::isfinite(value) ||
      impl_->maximum_histogram_samples == 0U) {
    return Status(StatusCode::InvalidArgument,
                  "histogram requires a name, finite value, and capacity");
  }
  std::lock_guard lock(impl_->mutex);
  const auto kind_status =
      impl_->CheckKindLocked(name, MetricKind::Histogram);
  if (!kind_status) {
    return kind_status;
  }
  auto [iterator, inserted] = impl_->entries.try_emplace(
      std::string(name), Impl::Entry(MetricKind::Histogram));
  auto& entry = iterator->second;
  if (inserted) {
    entry.minimum = value;
    entry.maximum = value;
  } else {
    entry.minimum = std::min(entry.minimum, value);
    entry.maximum = std::max(entry.maximum, value);
  }
  ++entry.count;
  entry.sum += value;
  entry.value = value;
  if (entry.samples.size() == impl_->maximum_histogram_samples) {
    entry.samples.pop_front();
  }
  entry.samples.push_back(value);
  return Status::Ok();
}

std::vector<MetricSample> MetricsRegistry::Snapshot() const {
  std::vector<MetricSample> snapshot;
  {
    std::lock_guard lock(impl_->mutex);
    snapshot.reserve(impl_->entries.size());
    for (const auto& [name, entry] : impl_->entries) {
      MetricSample sample;
      sample.name = name;
      sample.kind = entry.kind;
      sample.value = entry.value;
      if (entry.kind == MetricKind::Histogram &&
          entry.count != 0U) {
        const std::vector<double> retained(
            entry.samples.begin(), entry.samples.end());
        sample.distribution.count = entry.count;
        sample.distribution.sampled_count = retained.size();
        sample.distribution.minimum = entry.minimum;
        sample.distribution.maximum = entry.maximum;
        sample.distribution.mean =
            entry.sum / static_cast<double>(entry.count);
        sample.distribution.p50 = Percentile(retained, 0.50);
        sample.distribution.p95 = Percentile(retained, 0.95);
        sample.distribution.p99 = Percentile(retained, 0.99);
      }
      snapshot.push_back(std::move(sample));
    }
  }
  std::sort(snapshot.begin(), snapshot.end(),
            [](const MetricSample& left,
               const MetricSample& right) {
              return left.name < right.name;
            });
  return snapshot;
}

std::string MetricsRegistry::RenderJson() const {
  const auto snapshot = Snapshot();
  std::ostringstream output;
  output << std::setprecision(15);
  output << "{\"captured_monotonic_ns\":"
         << MonotonicNanoseconds() << ",\"metrics\":[";
  for (std::size_t index = 0U; index < snapshot.size(); ++index) {
    const auto& sample = snapshot[index];
    if (index != 0U) {
      output << ',';
    }
    output << "{\"name\":\"" << JsonEscape(sample.name)
           << "\",\"kind\":\"" << MetricKindName(sample.kind) << "\"";
    if (sample.kind == MetricKind::Histogram) {
      output << ",\"count\":" << sample.distribution.count
             << ",\"sampled_count\":"
             << sample.distribution.sampled_count
             << ",\"min\":" << sample.distribution.minimum
             << ",\"max\":" << sample.distribution.maximum
             << ",\"mean\":" << sample.distribution.mean
             << ",\"p50\":" << sample.distribution.p50
             << ",\"p95\":" << sample.distribution.p95
             << ",\"p99\":" << sample.distribution.p99;
    } else {
      output << ",\"value\":" << sample.value;
    }
    output << '}';
  }
  output << "]}";
  return output.str();
}

struct StructuredLogger::Impl {
  explicit Impl(LogSink configured_sink)
      : sink(std::move(configured_sink)) {}

  std::mutex mutex;
  LogSink sink;
};

StructuredLogger::StructuredLogger(LogSink sink)
    : impl_(std::make_unique<Impl>(std::move(sink))) {}

StructuredLogger::~StructuredLogger() = default;

Status StructuredLogger::Log(LogRecord record) {
  if (!impl_->sink || record.component.empty() ||
      record.event.empty()) {
    return Status(StatusCode::InvalidArgument,
                  "structured log requires sink, component, and event");
  }

  std::ostringstream output;
  output << "{\"timestamp_ns\":" << SystemNanoseconds()
         << ",\"monotonic_ns\":" << MonotonicNanoseconds()
         << ",\"level\":\"" << LogLevelName(record.level)
         << "\",\"component\":\"" << JsonEscape(record.component)
         << "\",\"event\":\"" << JsonEscape(record.event)
         << "\",\"generation\":" << record.generation
         << ",\"trace_id\":" << record.trace_id
         << ",\"span_id\":" << record.span_id
         << ",\"status\":\"" << StatusCodeName(record.status)
         << "\",\"detail\":\"" << JsonEscape(record.detail)
         << "\",\"attributes\":{";
  for (std::size_t index = 0U;
       index < record.attributes.size(); ++index) {
    if (record.attributes[index].first.empty()) {
      return Status(StatusCode::InvalidArgument,
                    "structured log attribute key is empty");
    }
    if (index != 0U) {
      output << ',';
    }
    output << '\"' << JsonEscape(record.attributes[index].first)
           << "\":\""
           << JsonEscape(record.attributes[index].second) << '\"';
  }
  output << "}}";

  try {
    std::lock_guard lock(impl_->mutex);
    impl_->sink(output.str());
  } catch (const std::exception& error) {
    return Status(StatusCode::Internal,
                  std::string("log sink threw: ") + error.what());
  } catch (...) {
    return Status(StatusCode::Internal, "log sink threw");
  }
  return Status::Ok();
}

struct TraceRecorder::Impl {
  mutable std::mutex mutex;
  std::vector<TraceSpan> spans;
};

TraceRecorder::TraceRecorder() : impl_(std::make_unique<Impl>()) {}

TraceRecorder::~TraceRecorder() = default;

Status TraceRecorder::Record(TraceSpan span) {
  if (span.trace_id == 0U || span.span_id == 0U ||
      span.component.empty() || span.operation.empty() ||
      span.finish_monotonic_ns < span.start_monotonic_ns) {
    return Status(StatusCode::InvalidArgument,
                  "invalid trace span");
  }
  std::lock_guard lock(impl_->mutex);
  const auto duplicate = std::find_if(
      impl_->spans.begin(), impl_->spans.end(),
      [&](const TraceSpan& existing) {
        return existing.trace_id == span.trace_id &&
               existing.span_id == span.span_id;
      });
  if (duplicate != impl_->spans.end()) {
    return Status(StatusCode::AlreadyExists,
                  "trace span already exists");
  }
  impl_->spans.push_back(std::move(span));
  return Status::Ok();
}

std::vector<TraceSpan> TraceRecorder::Snapshot(
    std::uint64_t trace_id) const {
  std::vector<TraceSpan> snapshot;
  {
    std::lock_guard lock(impl_->mutex);
    for (const auto& span : impl_->spans) {
      if (trace_id == 0U || span.trace_id == trace_id) {
        snapshot.push_back(span);
      }
    }
  }
  std::sort(snapshot.begin(), snapshot.end(),
            [](const TraceSpan& left, const TraceSpan& right) {
              if (left.trace_id != right.trace_id) {
                return left.trace_id < right.trace_id;
              }
              if (left.start_monotonic_ns !=
                  right.start_monotonic_ns) {
                return left.start_monotonic_ns <
                       right.start_monotonic_ns;
              }
              return left.span_id < right.span_id;
            });
  return snapshot;
}

DistributionSummary
TraceRecorder::EndToEndLatencyMicroseconds() const {
  struct Bounds {
    std::uint64_t start{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t finish{0U};
  };
  std::unordered_map<std::uint64_t, Bounds> bounds;
  {
    std::lock_guard lock(impl_->mutex);
    for (const auto& span : impl_->spans) {
      auto& trace = bounds[span.trace_id];
      trace.start = std::min(trace.start, span.start_monotonic_ns);
      trace.finish = std::max(trace.finish, span.finish_monotonic_ns);
    }
  }

  std::vector<double> latencies;
  latencies.reserve(bounds.size());
  for (const auto& [trace_id, trace] : bounds) {
    static_cast<void>(trace_id);
    if (trace.start != std::numeric_limits<std::uint64_t>::max() &&
        trace.finish >= trace.start) {
      latencies.push_back(
          static_cast<double>(trace.finish - trace.start) / 1000.0);
    }
  }
  return Summarize(latencies);
}

ProcessCpuMetrics SampleProcessCpu() noexcept {
  ProcessCpuMetrics result;
#if defined(__unix__) || defined(__APPLE__)
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    return result;
  }
  result.available = true;
  result.user_seconds =
      static_cast<double>(usage.ru_utime.tv_sec) +
      static_cast<double>(usage.ru_utime.tv_usec) / 1'000'000.0;
  result.system_seconds =
      static_cast<double>(usage.ru_stime.tv_sec) +
      static_cast<double>(usage.ru_stime.tv_usec) / 1'000'000.0;
#if defined(__APPLE__)
  result.maximum_resident_bytes =
      static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  result.maximum_resident_bytes =
      static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
  result.voluntary_context_switches =
      static_cast<std::uint64_t>(usage.ru_nvcsw);
  result.involuntary_context_switches =
      static_cast<std::uint64_t>(usage.ru_nivcsw);
#endif
  return result;
}

std::string_view MetricKindName(MetricKind kind) noexcept {
  switch (kind) {
    case MetricKind::Counter:
      return "counter";
    case MetricKind::Gauge:
      return "gauge";
    case MetricKind::Histogram:
      return "histogram";
  }
  return "unknown";
}

std::string_view LogLevelName(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
  }
  return "UNKNOWN";
}

std::string_view StatusCodeName(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok:
      return "ok";
    case StatusCode::InvalidArgument:
      return "invalid_argument";
    case StatusCode::AlreadyExists:
      return "already_exists";
    case StatusCode::NotFound:
      return "not_found";
    case StatusCode::Closed:
      return "closed";
    case StatusCode::Timeout:
      return "timeout";
    case StatusCode::QueueFull:
      return "queue_full";
    case StatusCode::Dropped:
      return "dropped";
    case StatusCode::Cancelled:
      return "cancelled";
    case StatusCode::StaleGeneration:
      return "stale_generation";
    case StatusCode::TransportError:
      return "transport_error";
    case StatusCode::Unsupported:
      return "unsupported";
    case StatusCode::Internal:
      return "internal";
  }
  return "unknown";
}

}  // namespace autoruntime
