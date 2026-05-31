#include "core/failure_injector.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>

namespace keystone {
namespace core {

FailureInjector::FailureInjector(uint32_t seed) : rng_(seed == 0 ? std::random_device{}() : seed) {}

// ============================================================================
// Agent Crash Simulation
// ============================================================================

void FailureInjector::injectAgentCrash(const std::string& agent_id) {
  std::lock_guard<std::mutex> lock(crashed_mutex_);
  crashed_agents_.insert(agent_id);
  total_failures_++;
}

bool FailureInjector::isAgentCrashed(const std::string& agent_id) const {
  std::lock_guard<std::mutex> lock(crashed_mutex_);
  return crashed_agents_.find(agent_id) != crashed_agents_.end();
}

void FailureInjector::recoverAgent(const std::string& agent_id) {
  std::lock_guard<std::mutex> lock(crashed_mutex_);
  crashed_agents_.erase(agent_id);
}

// ============================================================================
// Agent Timeout Simulation
// ============================================================================

void FailureInjector::injectAgentTimeout(const std::string& agent_id,
                                         std::chrono::milliseconds delay) {
  std::lock_guard<std::mutex> lock(timeout_mutex_);
  timeout_agents_[agent_id] = delay;
  total_failures_++;
}

std::chrono::milliseconds FailureInjector::getAgentTimeout(const std::string& agent_id) const {
  std::lock_guard<std::mutex> lock(timeout_mutex_);
  auto it = timeout_agents_.find(agent_id);
  if (it != timeout_agents_.end()) {
    return it->second;
  }
  return std::chrono::milliseconds{0};
}

void FailureInjector::clearAgentTimeout(const std::string& agent_id) {
  std::lock_guard<std::mutex> lock(timeout_mutex_);
  timeout_agents_.erase(agent_id);
}

// ============================================================================
// Random Failure Simulation
// ============================================================================

void FailureInjector::setFailureRate(double rate) {
  std::lock_guard<std::mutex> lock(failure_rate_mutex_);
  failure_rate_ = std::clamp(rate, 0.0, 1.0);
}

bool FailureInjector::shouldFail() {
  std::lock_guard<std::mutex> lock(rng_mutex_);
  double random_value = dist_(rng_);

  std::lock_guard<std::mutex> rate_lock(failure_rate_mutex_);
  return random_value < failure_rate_;
}

bool FailureInjector::shouldAgentFail(const std::string& agent_id) {
  // Check if agent is already crashed
  if (isAgentCrashed(agent_id)) {
    return true;
  }

  // Roll dice for random failure
  return shouldFail();
}

// ============================================================================
// Statistics
// ============================================================================

int32_t FailureInjector::getTotalFailures() const { return total_failures_.load(); }

std::vector<std::string> FailureInjector::getFailedAgents() const {
  std::lock_guard<std::mutex> lock(crashed_mutex_);
  std::vector<std::string> agents;
  agents.reserve(crashed_agents_.size());
  for (const auto& agent_id : crashed_agents_) {
    agents.push_back(agent_id);
  }
  return agents;
}

std::vector<std::string> FailureInjector::getTimeoutAgents() const {
  std::lock_guard<std::mutex> lock(timeout_mutex_);
  std::vector<std::string> agents;
  agents.reserve(timeout_agents_.size());
  for (const auto& [agent_id, _] : timeout_agents_) {
    agents.push_back(agent_id);
  }
  return agents;
}

void FailureInjector::reset() {
  {
    std::lock_guard<std::mutex> lock(crashed_mutex_);
    crashed_agents_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(timeout_mutex_);
    timeout_agents_.clear();
  }
  total_failures_ = 0;
}

std::string FailureInjector::getStatistics() const {
  std::ostringstream oss;
  oss << "FailureInjector Statistics:\n";
  oss << "  Total failures injected: " << getTotalFailures() << "\n";

  auto crashed = getFailedAgents();
  oss << "  Crashed agents: " << crashed.size() << "\n";
  for (const auto& agent_id : crashed) {
    oss << "    - " << agent_id << "\n";
  }

  auto timeout_agents = getTimeoutAgents();
  oss << "  Timeout agents: " << timeout_agents.size() << "\n";
  for (const auto& agent_id : timeout_agents) {
    auto delay = getAgentTimeout(agent_id);
    oss << "    - " << agent_id << " (" << delay.count() << "ms delay)\n";
  }

  {
    std::lock_guard<std::mutex> lock(failure_rate_mutex_);
    oss << "  Random failure rate: " << (failure_rate_ * 100.0) << "%\n";
  }

  return oss.str();
}

}  // namespace core
}  // namespace keystone
