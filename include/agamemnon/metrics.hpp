#pragma once

#include <chrono>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>
#include <string>

namespace agamemnon {

/// Owns all Prometheus metric families and provides a serialize() method that
/// produces Prometheus text-format output for the /metrics HTTP endpoint.
///
/// Intended usage: construct once in main(), then pass by reference to Routes,
/// Store, and NatsClient.  Thread-safe: prometheus-cpp families/counters are
/// internally locked.
class MetricsRegistry {
 public:
  MetricsRegistry();

  // Non-copyable, non-movable (holds references into registry_).
  MetricsRegistry(const MetricsRegistry&) = delete;
  MetricsRegistry& operator=(const MetricsRegistry&) = delete;

  // ── HTTP ────────────────────────────────────────────────────────────────
  /// Record a completed HTTP request: increments request counter, observes
  /// latency histogram, and increments error counter for 4xx/5xx responses.
  void record_request(const std::string& method, const std::string& path, int status_code,
                      double duration_seconds);

  // ── Tasks ───────────────────────────────────────────────────────────────
  /// Increment the tasks-created counter and update the current total gauge.
  void record_task_created();

  /// Record a task state transition (e.g. "pending" -> "completed").
  void record_task_state_change(const std::string& from_status, const std::string& to_status);

  /// Adjust the live task gauge by delta (pass -1 on delete).
  void adjust_task_count(int delta);

  // ── Agents ──────────────────────────────────────────────────────────────
  /// Adjust the live agent gauge by delta (pass -1 on delete).
  void adjust_agent_count(int delta);

  // ── NATS ────────────────────────────────────────────────────────────────
  /// Increment the published messages counter with a subject prefix label.
  void record_nats_publish(const std::string& subject_prefix);

  /// Increment the received messages counter with a subject prefix label.
  void record_nats_receive(const std::string& subject_prefix);

  /// Set the NATS connectivity gauge (1 = connected, 0 = disconnected).
  void set_nats_connected(bool connected);

  // ── GitHub Inbound Sync (#165) ──────────────────────────────────────────
  /// Record inbound sync event outcome (applied, skipped_stale, closed, reopened).
  void record_inbound_sync(std::string_view outcome);

  // ── Serialization ───────────────────────────────────────────────────────
  /// Return the full Prometheus text-format snapshot (thread-safe).
  std::string serialize() const;

 private:
  std::shared_ptr<prometheus::Registry> registry_;

  // HTTP
  prometheus::Family<prometheus::Counter>& http_requests_total_;
  prometheus::Family<prometheus::Histogram>& http_request_duration_seconds_;
  prometheus::Family<prometheus::Counter>& http_errors_total_;

  // Tasks
  prometheus::Family<prometheus::Gauge>& tasks_total_;
  prometheus::Family<prometheus::Counter>& task_state_transitions_total_;

  // Agents
  prometheus::Family<prometheus::Gauge>& agents_total_;

  // NATS
  prometheus::Family<prometheus::Counter>& nats_messages_published_total_;
  prometheus::Family<prometheus::Counter>& nats_messages_received_total_;
  prometheus::Family<prometheus::Gauge>& nats_connected_;

  // GitHub inbound sync
  prometheus::Family<prometheus::Counter>& github_inbound_sync_total_;

  // Process / build info
  prometheus::Family<prometheus::Gauge>& process_start_time_seconds_;
  prometheus::Family<prometheus::Gauge>& build_info_;
};

/// RAII helper: records HTTP request duration on destruction.
/// Construct at the top of a route handler, destruction fires automatically.
class RequestTimer {
 public:
  RequestTimer(MetricsRegistry& reg, std::string method, std::string path)
      : reg_(reg),
        method_(std::move(method)),
        path_(std::move(path)),
        start_(std::chrono::steady_clock::now()) {}

  ~RequestTimer() {
    auto elapsed = std::chrono::steady_clock::now() - start_;
    double secs = std::chrono::duration<double>(elapsed).count();
    reg_.record_request(method_, path_, status_code_, secs);
  }

  void set_status(int code) noexcept { status_code_ = code; }

  // Non-copyable.
  RequestTimer(const RequestTimer&) = delete;
  RequestTimer& operator=(const RequestTimer&) = delete;

 private:
  MetricsRegistry& reg_;
  std::string method_;
  std::string path_;
  std::chrono::steady_clock::time_point start_;
  int status_code_ = 200;
};

}  // namespace agamemnon
