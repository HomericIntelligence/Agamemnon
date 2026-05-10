#pragma once

#include "projectagamemnon/nats_publisher.hpp"

#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

/// Header-only test double that records every publish() call.
/// Thread-safety is not required — integration tests run single-threaded
/// per fixture setup.
class FakeNatsPublisher : public NatsPublisher {
 public:
  struct Call {
    std::string subject;
    std::string payload;
  };

  bool publish(const std::string& subject, const std::string& payload) override {
    calls.push_back({subject, payload});
    return true;
  }

  void publish_log(const std::string& subject, const std::string& /*level*/,
                   const std::string& /*message*/, const nlohmann::json& /*metadata*/) override {
    log_calls.push_back(subject);
  }

  bool has_subject(const std::string& subject) const {
    for (const auto& c : calls) {
      if (c.subject == subject) return true;
    }
    return false;
  }

  bool has_subject_prefix(const std::string& prefix) const {
    for (const auto& c : calls) {
      if (c.subject.rfind(prefix, 0) == 0) return true;
    }
    return false;
  }

  void clear() {
    calls.clear();
    log_calls.clear();
  }

  std::vector<Call> calls;
  std::vector<std::string> log_calls;
};

}  // namespace projectagamemnon
