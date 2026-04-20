#include "test_support.hpp"

#include <autoruntime/observability.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

const autoruntime::MetricSample* FindMetric(
    const std::vector<autoruntime::MetricSample>& samples,
    std::string_view name) {
  const auto iterator = std::find_if(
      samples.begin(), samples.end(),
      [&](const autoruntime::MetricSample& sample) {
        return sample.name == name;
      });
  return iterator == samples.end() ? nullptr : &*iterator;
}

bool Near(double left, double right, double tolerance = 0.0001) {
  return std::abs(left - right) <= tolerance;
}

int MetricsHaveStableKindsAndPercentiles() {
  autoruntime::MetricsRegistry metrics(256U);
  CHECK(metrics.IncrementCounter("node.sensor.messages", 2.0));
  CHECK(metrics.IncrementCounter("node.sensor.messages", 3.0));
  CHECK(metrics.SetGauge("node.sensor.queue_depth", 7.0));
  for (int value = 1; value <= 100; ++value) {
    CHECK(metrics.Observe(
        "pipeline.e2e_latency_us", static_cast<double>(value)));
  }

  const auto conflict =
      metrics.SetGauge("node.sensor.messages", 1.0);
  CHECK(!conflict);
  CHECK(conflict.code() == autoruntime::StatusCode::AlreadyExists);

  const auto snapshot = metrics.Snapshot();
  const auto* counter = FindMetric(snapshot, "node.sensor.messages");
  const auto* gauge = FindMetric(snapshot, "node.sensor.queue_depth");
  const auto* histogram =
      FindMetric(snapshot, "pipeline.e2e_latency_us");
  CHECK(counter != nullptr);
  CHECK(counter->kind == autoruntime::MetricKind::Counter);
  CHECK(Near(counter->value, 5.0));
  CHECK(gauge != nullptr);
  CHECK(gauge->kind == autoruntime::MetricKind::Gauge);
  CHECK(Near(gauge->value, 7.0));
  CHECK(histogram != nullptr);
  CHECK(histogram->kind == autoruntime::MetricKind::Histogram);
  CHECK(histogram->distribution.count == 100U);
  CHECK(histogram->distribution.sampled_count == 100U);
  CHECK(Near(histogram->distribution.minimum, 1.0));
  CHECK(Near(histogram->distribution.maximum, 100.0));
  CHECK(Near(histogram->distribution.mean, 50.5));
  CHECK(Near(histogram->distribution.p50, 50.0));
  CHECK(Near(histogram->distribution.p95, 95.0));
  CHECK(Near(histogram->distribution.p99, 99.0));
  CHECK(Near(histogram->distribution.p99_9, 100.0));

  const auto json = metrics.RenderJson();
  CHECK(json.find("\"pipeline.e2e_latency_us\"") != std::string::npos);
  CHECK(json.find("\"p99\":99") != std::string::npos);
  CHECK(json.find("\"p99_9\":100") != std::string::npos);
  return 0;
}

int StructuredLogsEscapeAndCarryTraceContext() {
  std::vector<std::string> lines;
  autoruntime::StructuredLogger logger(
      [&](std::string line) { lines.push_back(std::move(line)); });

  autoruntime::LogRecord record;
  record.level = autoruntime::LogLevel::Warn;
  record.component = "planning";
  record.event = "deadline_miss";
  record.generation = 4U;
  record.trace_id = 91U;
  record.span_id = 92U;
  record.status = autoruntime::StatusCode::Timeout;
  record.detail = "late by 3ms";
  record.attributes.push_back({"path", "a\"b\n"});
  CHECK(logger.Log(std::move(record)));
  CHECK(lines.size() == 1U);
  CHECK(lines.front().find("\"level\":\"WARN\"") !=
        std::string::npos);
  CHECK(lines.front().find("\"trace_id\":91") !=
        std::string::npos);
  CHECK(lines.front().find("a\\\"b\\n") != std::string::npos);
  return 0;
}

int TraceAggregationComputesEndToEndLatency() {
  autoruntime::TraceRecorder traces;
  CHECK(traces.Record(autoruntime::TraceSpan{
      10U, 100U, 0U, 1U, "sensor", "publish", 1000U, 2000U,
      autoruntime::StatusCode::Ok}));
  CHECK(traces.Record(autoruntime::TraceSpan{
      10U, 101U, 100U, 1U, "planning", "callback", 2200U, 3500U,
      autoruntime::StatusCode::Ok}));
  CHECK(traces.Record(autoruntime::TraceSpan{
      10U, 102U, 101U, 1U, "control", "callback", 3600U, 5000U,
      autoruntime::StatusCode::Ok}));
  CHECK(traces.Record(autoruntime::TraceSpan{
      20U, 200U, 0U, 2U, "sensor", "control", 10000U, 13000U,
      autoruntime::StatusCode::Ok}));

  const auto trace = traces.Snapshot(10U);
  CHECK(trace.size() == 3U);
  const auto summary = traces.EndToEndLatencyMicroseconds();
  CHECK(summary.count == 2U);
  CHECK(Near(summary.minimum, 3.0));
  CHECK(Near(summary.maximum, 4.0));
  CHECK(Near(summary.p50, 3.0));
  CHECK(Near(summary.p95, 4.0));
  return 0;
}

int ProcessCpuAndContextSwitchesAreObservable() {
  const auto cpu = autoruntime::SampleProcessCpu();
#if defined(__unix__) || defined(__APPLE__)
  CHECK(cpu.available);
#endif
  CHECK(cpu.user_seconds >= 0.0);
  CHECK(cpu.system_seconds >= 0.0);
  return 0;
}

}  // namespace

int main() {
  if (const int result = MetricsHaveStableKindsAndPercentiles();
      result != 0) {
    return result;
  }
  if (const int result = StructuredLogsEscapeAndCarryTraceContext();
      result != 0) {
    return result;
  }
  if (const int result = TraceAggregationComputesEndToEndLatency();
      result != 0) {
    return result;
  }
  return ProcessCpuAndContextSwitchesAreObservable();
}
