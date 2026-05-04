#pragma once

#include <functional>
#include <string>

#include "nlohmann/json.hpp"
#include "projectagamemnon/circuit_breaker.hpp"
#include "projectagamemnon/dead_letter_queue.hpp"

namespace projectagamemnon {

/// Thin wrapper around the nats.c client library with JetStream support.
///
/// Includes:
///   - Exponential-backoff retry on infra-class publish failures (up to kMaxRetries)
///   - Circuit breaker (Closed → Open → HalfOpen) to avoid hammering a dead broker
///   - Bounded dead-letter queue for messages that exhaust all retries
///
/// Designed for graceful degradation: if the NATS server is unavailable,
/// connect() returns false and is_connected() returns false.  All publish /
/// subscribe calls are no-ops when not connected.
class NatsClient {
 public:
  static constexpr int kMaxRetries = 3;
  static constexpr int kBaseRetryMs = 50;

  explicit NatsClient(const std::string& url);
  NatsClient(const std::string& url, CircuitBreaker::Config cb_cfg, std::size_t dlq_capacity);
  ~NatsClient();

  // Non-copyable, non-movable (holds raw pointers).
  NatsClient(const NatsClient&) = delete;
  NatsClient& operator=(const NatsClient&) = delete;

  /// Connect to the NATS server.  Returns false and logs a warning on failure.
  bool connect();

  /// Close the connection gracefully.
  void close();

  bool is_connected() const { return connected_; }

  /// Create JetStream streams (idempotent — safe to call even if they already exist).
  void ensure_streams();

  /// Publish a JSON string to a NATS subject.
  /// Retries up to kMaxRetries times with exponential backoff on infra failures.
  /// Pushes to the dead-letter queue after all retries are exhausted.
  /// Returns false if circuit is open, not connected, or all retries fail.
  bool publish(const std::string& subject, const std::string& payload);

  /// Subscribe to a subject with a callback.
  /// The callback receives (subject, data) strings.
  using MessageCallback = std::function<void(const std::string& subject, const std::string& data)>;
  bool subscribe(const std::string& subject, MessageCallback cb);

  /// Publish a structured log event to hi.logs.agamemnon.<event> (ADR-005).
  /// Fire-and-forget: NATS failures are logged but do not propagate.
  void publish_log(const std::string& subject, const std::string& level, const std::string& message,
                   const nlohmann::json& metadata);

  /// Access the dead-letter queue (for drain/clear endpoints).
  DeadLetterQueue& dead_letter_queue() { return dlq_; }
  const DeadLetterQueue& dead_letter_queue() const { return dlq_; }

  /// Access the circuit breaker (for health endpoints).
  const CircuitBreaker& circuit_breaker() const { return breaker_; }

 private:
  std::string url_;
  void* conn_ = nullptr;  // natsConnection*  (opaque to avoid header leak)
  void* js_ = nullptr;    // jsCtx*
  bool connected_ = false;

  CircuitBreaker breaker_;
  DeadLetterQueue dlq_;

  /// Attempt a single low-level publish. Returns natsStatus as int.
  int do_publish_once(const std::string& subject, const std::string& payload);
};

}  // namespace projectagamemnon
