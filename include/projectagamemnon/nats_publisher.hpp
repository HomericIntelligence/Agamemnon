#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

/// Abstract interface for NATS publishing.  Allows tests to inject a fake
/// publisher without requiring a live NATS connection.
class NatsPublisher {
 public:
  virtual ~NatsPublisher() = default;

  virtual bool publish(const std::string& subject, const std::string& payload) = 0;

  virtual void publish_log(const std::string& subject, const std::string& level,
                           const std::string& message, const nlohmann::json& metadata) = 0;
};

}  // namespace projectagamemnon
