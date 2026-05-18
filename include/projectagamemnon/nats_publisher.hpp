#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

class CircuitBreaker;
class DeadLetterQueue;

/// Abstract interface for NATS publishing.  Allows tests to inject a fake
/// publisher without requiring a live NATS connection.
class NatsPublisher {
 public:
  virtual ~NatsPublisher() = default;

  virtual bool publish(const std::string& subject, const std::string& payload) = 0;

  virtual void publish_log(const std::string& subject, const std::string& level,
                           const std::string& message, const nlohmann::json& metadata) = 0;

  /// Access the underlying dead-letter queue, if the publisher has one.
  /// Returns nullptr for publishers that don't track DLQ state (e.g.
  /// FakeNatsPublisher in tests).  Removes the need for dynamic_cast in
  /// route handlers that surface DLQ state via /v1/dead-letter.
  virtual DeadLetterQueue* dead_letter_queue() { return nullptr; }

  /// Access the underlying circuit breaker, if the publisher has one.
  /// Returns nullptr for publishers that don't track breaker state.
  virtual CircuitBreaker* circuit_breaker() { return nullptr; }
};

}  // namespace projectagamemnon
