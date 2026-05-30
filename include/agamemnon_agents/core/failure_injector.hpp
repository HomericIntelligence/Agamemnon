#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace keystone {
namespace core {

/**
 * @brief Chaos engineering tool for injecting failures into the HMAS
 *
 * Phase 5: Robustness & Chaos Engineering
 *
 * The FailureInjector allows controlled simulation of various failure modes:
 * - Agent crashes (agent stops responding)
 * - Agent timeouts (agent responds slowly)
 * - Agent overload (agent's queue is full)
 * - Random failures (probabilistic failure injection)
 *
 * Usage:
 * ```cpp
 * FailureInjector injector;
 * injector.setFailureRate(0.1);  // 10% random failure rate
 *
 * if (injector.shouldAgentFail(agent_id)) {
 *     // Simulate failure
 * }
 * ```
 */
class FailureInjector {
 public:
  /**
   * @brief Construct a new Failure Injector
   *
   * @param seed Random seed for reproducibility (0 = random)
   */
  explicit FailureInjector(uint32_t seed = 0);

  // ========================================================================
  // Agent Crash Simulation
  // ========================================================================

  /**
   * @brief Mark an agent as crashed (stops processing messages)
   *
   * @param agent_id Agent to crash
   */
  void injectAgentCrash(const std::string& agent_id);

  /**
   * @brief Check if an agent is in crashed state
   *
   * @param agent_id Agent to check
   * @return true if agent is crashed
   */
  bool isAgentCrashed(const std::string& agent_id) const;

  /**
   * @brief Recover a crashed agent
   *
   * @param agent_id Agent to recover
   */
  void recoverAgent(const std::string& agent_id);

  // ========================================================================
  // Agent Timeout Simulation
  // ========================================================================

  /**
   * @brief Inject a timeout delay for an agent
   *
   * When an agent has a timeout, its responses are delayed.
   *
   * @param agent_id Agent to delay
   * @param delay Response delay
   */
  void injectAgentTimeout(const std::string& agent_id, std::chrono::milliseconds delay);

  /**
   * @brief Get the timeout delay for an agent
   *
   * @param agent_id Agent to check
   * @return std::chrono::milliseconds Delay (0 if no timeout)
   */
  std::chrono::milliseconds getAgentTimeout(const std::string& agent_id) const;

  /**
   * @brief Clear timeout for an agent
   *
   * @param agent_id Agent to clear
   */
  void clearAgentTimeout(const std::string& agent_id);

  // ========================================================================
  // Random Failure Simulation
  // ========================================================================

  /**
   * @brief Set the random failure rate
   *
   * @param rate Failure rate (0.0 = never fail, 1.0 = always fail)
   */
  void setFailureRate(double rate);

  /**
   * @brief Get current failure rate
   *
   * @return double Failure rate
   */
  double getFailureRate() const { return failure_rate_; }

  /**
   * @brief Check if a random failure should occur
   *
   * Uses the configured failure rate to probabilistically return true.
   *
   * @return true if failure should occur
   */
  bool shouldFail();

  /**
   * @brief Check if a specific agent should fail (random)
   *
   * Combines agent-specific crash state with random failures.
   *
   * @param agent_id Agent to check
   * @return true if agent should fail
   */
  bool shouldAgentFail(const std::string& agent_id);

  // ========================================================================
  // Statistics
  // ========================================================================

  /**
   * @brief Get total number of injected failures
   *
   * @return int32_t Total failures
   */
  int32_t getTotalFailures() const;

  /**
   * @brief Get list of currently failed agents
   *
   * @return std::vector<std::string> Failed agent IDs
   */
  std::vector<std::string> getFailedAgents() const;

  /**
   * @brief Get list of agents with timeouts
   *
   * @return std::vector<std::string> Agent IDs with timeouts
   */
  std::vector<std::string> getTimeoutAgents() const;

  /**
   * @brief Reset all failures and timeouts
   */
  void reset();

  /**
   * @brief Get failure statistics
   *
   * @return std::string Human-readable statistics
   */
  std::string getStatistics() const;

 private:
  // Crashed agents (cannot process messages)
  std::unordered_set<std::string> crashed_agents_;
  mutable std::mutex crashed_mutex_;

  // Agents with timeouts (delayed responses)
  std::unordered_map<std::string, std::chrono::milliseconds> timeout_agents_;
  mutable std::mutex timeout_mutex_;

  // Random failure configuration
  double failure_rate_{0.0};  // 0.0 - 1.0
  mutable std::mutex failure_rate_mutex_;

  // Random number generation
  mutable std::mt19937 rng_;
  mutable std::uniform_real_distribution<double> dist_{0.0, 1.0};
  mutable std::mutex rng_mutex_;

  // Statistics
  mutable std::atomic<int32_t> total_failures_{0};
};

}  // namespace core
}  // namespace keystone
