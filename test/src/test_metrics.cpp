#include "agamemnon/metrics.hpp"

#include <string>

#include <gtest/gtest.h>

namespace agamemnon::test {

// ── Serialize helper ──────────────────────────────────────────────────────────

static bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// ── Counter tests ─────────────────────────────────────────────────────────────

TEST(MetricsRegistryCounterTest, RequestCounterIncrements) {
  MetricsRegistry reg;
  reg.record_request("GET", "/v1/health", 200, 0.001);
  reg.record_request("GET", "/v1/health", 200, 0.002);

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_http_requests_total"));
  // Counter value should be 2 for this label combination
  EXPECT_TRUE(contains(output, "hi_http_requests_total{"));
}

TEST(MetricsRegistryCounterTest, ErrorCounterOnlyFor4xxAnd5xx) {
  MetricsRegistry reg;
  reg.record_request("GET", "/v1/agents", 200, 0.001);      // not an error
  reg.record_request("GET", "/v1/agents/bad", 404, 0.001);  // error

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_http_errors_total"));
}

TEST(MetricsRegistryCounterTest, NoErrorCounterFor2xx) {
  MetricsRegistry reg;
  reg.record_request("POST", "/v1/agents", 201, 0.005);
  // 201 must not create an hi_http_errors_total entry
  std::string output = reg.serialize();
  // hi_http_errors_total TYPE line may still be present, but there must be no
  // data line with status="201"
  EXPECT_FALSE(contains(output, "hi_http_errors_total{") && contains(output, "status=\"201\""));
}

// ── Histogram tests ───────────────────────────────────────────────────────────

TEST(MetricsRegistryHistogramTest, DurationHistogramPresent) {
  MetricsRegistry reg;
  reg.record_request("GET", "/v1/version", 200, 0.042);

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_http_request_duration_seconds"));
  EXPECT_TRUE(contains(output, "_bucket{"));
  EXPECT_TRUE(contains(output, "_sum{"));
  EXPECT_TRUE(contains(output, "_count{"));
}

// ── Serialization format tests ────────────────────────────────────────────────

TEST(MetricsRegistrySerializeTest, OutputContainsTypeComments) {
  MetricsRegistry reg;

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "# TYPE hi_http_requests_total counter"));
  EXPECT_TRUE(contains(output, "# TYPE hi_http_request_duration_seconds histogram"));
  EXPECT_TRUE(contains(output, "# TYPE hi_http_errors_total counter"));
  EXPECT_TRUE(contains(output, "# TYPE hi_tasks_total gauge"));
  EXPECT_TRUE(contains(output, "# TYPE hi_task_state_transitions_total counter"));
  EXPECT_TRUE(contains(output, "# TYPE hi_agents_total gauge"));
  EXPECT_TRUE(contains(output, "# TYPE hi_nats_messages_published_total counter"));
  EXPECT_TRUE(contains(output, "# TYPE hi_nats_messages_received_total counter"));
  EXPECT_TRUE(contains(output, "# TYPE hi_nats_connected gauge"));
  EXPECT_TRUE(contains(output, "# TYPE hi_process_start_time_seconds gauge"));
  EXPECT_TRUE(contains(output, "# TYPE hi_build_info gauge"));
}

TEST(MetricsRegistrySerializeTest, ProcessStartTimeIsNonZero) {
  MetricsRegistry reg;
  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_process_start_time_seconds"));
  // The value follows the metric name; it must not be 0
  EXPECT_FALSE(contains(output, "hi_process_start_time_seconds{} 0"));
}

TEST(MetricsRegistrySerializeTest, BuildInfoLabelPresent) {
  MetricsRegistry reg;
  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_build_info{"));
  EXPECT_TRUE(contains(output, "service=\"agamemnon\""));
}

// ── Task tests ────────────────────────────────────────────────────────────────

TEST(MetricsRegistryTaskTest, RecordTaskCreatedIncrementsGauge) {
  MetricsRegistry reg;
  reg.record_task_created();
  reg.record_task_created();

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_tasks_total"));
}

TEST(MetricsRegistryTaskTest, RecordStateTransitionIncrements) {
  MetricsRegistry reg;
  reg.record_task_state_change("pending", "completed");
  reg.record_task_state_change("pending", "in_progress");

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_task_state_transitions_total"));
  EXPECT_TRUE(contains(output, "from=\"pending\""));
}

TEST(MetricsRegistryTaskTest, AdjustTaskCountPositiveAndNegative) {
  MetricsRegistry reg;
  reg.adjust_task_count(5);
  reg.adjust_task_count(-2);

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_tasks_total"));
}

// ── Agent tests ───────────────────────────────────────────────────────────────

TEST(MetricsRegistryAgentTest, AdjustAgentCountTracked) {
  MetricsRegistry reg;
  reg.adjust_agent_count(3);
  reg.adjust_agent_count(-1);

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_agents_total"));
}

// ── NATS tests ────────────────────────────────────────────────────────────────

TEST(MetricsRegistryNatsTest, ConnectedStartsAtZero) {
  MetricsRegistry reg;
  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_nats_connected{} 0"));
}

TEST(MetricsRegistryNatsTest, SetConnectedSetsGaugeToOne) {
  MetricsRegistry reg;
  reg.set_nats_connected(true);

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_nats_connected{} 1"));
}

TEST(MetricsRegistryNatsTest, SetDisconnectedSetsGaugeToZero) {
  MetricsRegistry reg;
  reg.set_nats_connected(true);
  reg.set_nats_connected(false);

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_nats_connected{} 0"));
}

TEST(MetricsRegistryNatsTest, PublishCounterIncrements) {
  MetricsRegistry reg;
  reg.record_nats_publish("hi.tasks.created");
  reg.record_nats_publish("hi.tasks.updated");

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_nats_messages_published_total"));
  EXPECT_TRUE(contains(output, "subject_prefix=\"hi.tasks\""));
}

TEST(MetricsRegistryNatsTest, ReceiveCounterIncrements) {
  MetricsRegistry reg;
  reg.record_nats_receive("hi.myrmidon.general.abc123");

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_nats_messages_received_total"));
  EXPECT_TRUE(contains(output, "subject_prefix=\"hi.myrmidon\""));
}

// ── RequestTimer tests ────────────────────────────────────────────────────────

TEST(RequestTimerTest, TimerRecordsRequestOnDestruction) {
  MetricsRegistry reg;
  {
    RequestTimer t(reg, "GET", "/v1/health");
    t.set_status(200);
  }  // destructor fires here

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_http_requests_total{"));
  EXPECT_TRUE(contains(output, "method=\"GET\""));
  EXPECT_TRUE(contains(output, "path=\"/v1/health\""));
}

TEST(RequestTimerTest, TimerDefaultStatusIs200) {
  MetricsRegistry reg;
  {
    RequestTimer t(reg, "POST", "/v1/teams");
    // Do not call set_status — default should be 200
  }

  std::string output = reg.serialize();
  // Should record with status 200 (not an error)
  EXPECT_FALSE(contains(output, "hi_http_errors_total{"));
}

TEST(RequestTimerTest, TimerRecordsErrorForStatus4xx) {
  MetricsRegistry reg;
  {
    RequestTimer t(reg, "GET", "/v1/agents/nonexistent");
    t.set_status(404);
  }

  std::string output = reg.serialize();
  EXPECT_TRUE(contains(output, "hi_http_errors_total{"));
  EXPECT_TRUE(contains(output, "status=\"404\""));
}

}  // namespace agamemnon::test
