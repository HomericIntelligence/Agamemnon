#include "agamemnon/metrics.hpp"

#include <chrono>
#include <string>

namespace agamemnon {

namespace {

// Extract a short subject prefix (first two dot-separated tokens) to keep
// cardinality low — e.g. "hi.tasks.team1.abc123.updated" -> "hi.tasks".
std::string subject_prefix(const std::string& subject) {
  std::size_t first = subject.find('.');
  if (first == std::string::npos) return subject;
  std::size_t second = subject.find('.', first + 1);
  if (second == std::string::npos) return subject;
  return subject.substr(0, second);
}

}  // anonymous namespace

MetricsRegistry::MetricsRegistry()
    : registry_(std::make_shared<prometheus::Registry>()),

      http_requests_total_(prometheus::BuildCounter()
                               .Name("hi_http_requests_total")
                               .Help("Total HTTP requests handled")
                               .Register(*registry_)),

      http_request_duration_seconds_(prometheus::BuildHistogram()
                                         .Name("hi_http_request_duration_seconds")
                                         .Help("HTTP request latency in seconds")
                                         .Register(*registry_)),

      http_errors_total_(prometheus::BuildCounter()
                             .Name("hi_http_errors_total")
                             .Help("Total HTTP 4xx/5xx responses")
                             .Register(*registry_)),

      tasks_total_(prometheus::BuildGauge()
                       .Name("hi_tasks_total")
                       .Help("Current number of tasks in the store")
                       .Register(*registry_)),

      task_state_transitions_total_(prometheus::BuildCounter()
                                        .Name("hi_task_state_transitions_total")
                                        .Help("Total task state transitions")
                                        .Register(*registry_)),

      agents_total_(prometheus::BuildGauge()
                        .Name("hi_agents_total")
                        .Help("Current number of agents in the store")
                        .Register(*registry_)),

      nats_messages_published_total_(prometheus::BuildCounter()
                                         .Name("hi_nats_messages_published_total")
                                         .Help("Total NATS messages published")
                                         .Register(*registry_)),

      nats_messages_received_total_(prometheus::BuildCounter()
                                        .Name("hi_nats_messages_received_total")
                                        .Help("Total NATS messages received")
                                        .Register(*registry_)),

      nats_connected_(prometheus::BuildGauge()
                          .Name("hi_nats_connected")
                          .Help("1 if connected to NATS, 0 otherwise")
                          .Register(*registry_)),

      github_inbound_sync_total_(prometheus::BuildCounter()
                                     .Name("hi_github_inbound_sync_total")
                                     .Help("Total GitHub inbound sync events")
                                     .Register(*registry_)),

      process_start_time_seconds_(prometheus::BuildGauge()
                                      .Name("hi_process_start_time_seconds")
                                      .Help("Unix timestamp of process start (seconds)")
                                      .Register(*registry_)),

      build_info_(prometheus::BuildGauge()
                      .Name("hi_build_info")
                      .Help("Build metadata (value is always 1)")
                      .Register(*registry_)) {
  // Record process start time at construction.
  auto now = std::chrono::system_clock::now();
  double ts = std::chrono::duration<double>(now.time_since_epoch()).count();
  process_start_time_seconds_.Add({}).Set(ts);

  // Build info gauge — labels carry the metadata, value is always 1.
  build_info_.Add({{"service", "agamemnon"}, {"version", "0.1.0"}}).Set(1.0);

  // Pre-create the NATS connectivity gauge at 0 (disconnected until connect() is called).
  nats_connected_.Add({}).Set(0.0);
}

// ── HTTP ──────────────────────────────────────────────────────────────────────

void MetricsRegistry::record_request(const std::string& method, const std::string& path,
                                     int status_code, double duration_seconds) {
  std::string status_str = std::to_string(status_code);
  http_requests_total_.Add({{"method", method}, {"path", path}, {"status", status_str}})
      .Increment();

  http_request_duration_seconds_
      .Add({{"method", method}, {"path", path}},
           prometheus::Histogram::BucketBoundaries{0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0,
                                                   2.5, 5.0, 10.0})
      .Observe(duration_seconds);

  if (status_code >= 400) {
    http_errors_total_.Add({{"method", method}, {"path", path}, {"status", status_str}})
        .Increment();
  }
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

void MetricsRegistry::record_task_created() { tasks_total_.Add({}).Increment(); }

void MetricsRegistry::record_task_state_change(const std::string& from_status,
                                               const std::string& to_status) {
  task_state_transitions_total_.Add({{"from", from_status}, {"to", to_status}}).Increment();
}

void MetricsRegistry::adjust_task_count(int delta) { tasks_total_.Add({}).Increment(delta); }

// ── Agents ────────────────────────────────────────────────────────────────────

void MetricsRegistry::adjust_agent_count(int delta) { agents_total_.Add({}).Increment(delta); }

// ── NATS ──────────────────────────────────────────────────────────────────────

void MetricsRegistry::record_nats_publish(const std::string& subject) {
  nats_messages_published_total_.Add({{"subject_prefix", subject_prefix(subject)}}).Increment();
}

void MetricsRegistry::record_nats_receive(const std::string& subject) {
  nats_messages_received_total_.Add({{"subject_prefix", subject_prefix(subject)}}).Increment();
}

void MetricsRegistry::set_nats_connected(bool connected) {
  nats_connected_.Add({}).Set(connected ? 1.0 : 0.0);
}

// ── GitHub Inbound Sync ───────────────────────────────────────────────────────

void MetricsRegistry::record_inbound_sync(std::string_view outcome) {
  github_inbound_sync_total_.Add({{"outcome", std::string(outcome)}}).Increment();
}

// ── Serialization ─────────────────────────────────────────────────────────────

std::string MetricsRegistry::serialize() const {
  prometheus::TextSerializer serializer;
  return serializer.Serialize(registry_->Collect());
}

}  // namespace agamemnon
