#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_GRPC
#include "hmas_coordinator.pb.h"
#include "network/grpc_client.hpp"
#include "network/hmas_coordinator_client.hpp"
#include "network/service_registry_client.hpp"
#include "network/yaml_parser.hpp"
#endif

namespace keystone {
namespace agents {

/**
 * @brief Template for coordinating hierarchical agent operations
 *
 * Eliminates code duplication between ComponentLeadAgent and ModuleLeadAgent
 * by extracting common coordination patterns:
 * - State machine management (IDLE → PLANNING → WAITING →
 * AGGREGATING/SYNTHESIZING)
 * - Result collection from subordinate agents
 * - Execution trace tracking
 * - gRPC service integration (optional)
 *
 * Template Parameters:
 * @tparam StateEnum Enum class defining agent states
 *                   (must have: IDLE, PLANNING, WAITING,
 * AGGREGATING/SYNTHESIZING, ERROR)
 * @tparam ResultType Type of results collected from subordinates
 *
 * Usage Example:
 * @code
 * enum class ComponentState { IDLE, PLANNING, WAITING_FOR_MODULES, AGGREGATING,
 * ERROR }; class ComponentLeadAgent { CoordinationState<ComponentState,
 * std::string> coordination_;
 * };
 * @endcode
 *
 * Lines Saved: ~300 per agent (ComponentLead and ModuleLead)
 * Total Impact: ~600 lines eliminated
 */
template <typename StateEnum, typename ResultType = std::string>
class CoordinationState {
 public:
  // ========================================================================
  // Constructor & Core State Management
  // ========================================================================

  CoordinationState() : current_state_(StateEnum::IDLE) {}

  /**
   * @brief Transition to a new state and record in execution trace
   *
   * @param new_state New state to transition to
   * @param state_name String representation of the state (for trace)
   */
  void transitionTo(StateEnum new_state, const std::string& state_name) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state_ = new_state;
    execution_trace_.push_back(state_name);
  }

  /**
   * @brief Get current state
   */
  StateEnum getCurrentState() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_state_;
  }

  /**
   * @brief Get execution trace (history of state transitions)
   */
  std::vector<std::string> getExecutionTrace() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return execution_trace_;
  }

  // ========================================================================
  // Result Aggregation
  // ========================================================================

  /**
   * @brief Initialize coordination for N expected results
   */
  void initializeCoordination(size_t expected_count) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    expected_results_ = expected_count;
    received_results_ = 0;
    failed_results_ = 0;
    results_.clear();
    failure_messages_.clear();
    pending_subordinates_.clear();
  }

  /**
   * @brief Record a result from a subordinate agent
   *
   * @param result Result payload
   * @return true if all results received (success + failure), false otherwise
   */
  bool recordResult(const ResultType& result) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    results_.push_back(result);
    received_results_++;
    return received_results_ >= expected_results_;
  }

  /**
   * @brief Record a failure from a subordinate agent (Issue #87)
   *
   * Counts the failure as a terminal result so all-received completion is
   * detected and downstream agents are not permanently stalled.
   *
   * @param error_msg Error description from the failing subordinate
   * @return true if all results (successes + failures) have been received
   */
  bool recordFailure(const std::string& error_msg) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    failure_messages_.push_back(error_msg);
    failed_results_++;
    received_results_++;
    return received_results_ >= expected_results_;
  }

  /**
   * @brief Check if any subordinate reported a failure (Issue #87)
   */
  bool hasFailures() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return failed_results_ > 0;
  }

  /**
   * @brief Get count of failed results (Issue #87)
   */
  size_t getFailureCount() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return failed_results_;
  }

  /**
   * @brief Get failure messages from failed subordinates (Issue #87)
   */
  std::vector<std::string> getFailureMessages() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return failure_messages_;
  }

  /**
   * @brief Check if all expected results have been received
   */
  bool isComplete() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return received_results_ >= expected_results_;
  }

  /**
   * @brief Get count of expected results
   */
  size_t getExpectedCount() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return expected_results_;
  }

  /**
   * @brief Get count of received results
   */
  size_t getReceivedCount() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return received_results_;
  }

  /**
   * @brief Get all collected results
   */
  std::vector<ResultType> getResults() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return results_;
  }

  /**
   * @brief Track a pending subordinate by message ID
   */
  void trackPendingSubordinate(const std::string& msg_id, const std::string& subordinate_id) {
    pending_subordinates_[msg_id] = subordinate_id;
  }

  /**
   * @brief Get pending subordinates map
   */
  const std::unordered_map<std::string, std::string>& getPendingSubordinates() const {
    return pending_subordinates_;
  }

  // ========================================================================
  // Goal & Context Management
  // ========================================================================

  void setCurrentGoal(const std::string& goal) { current_goal_ = goal; }
  const std::string& getCurrentGoal() const { return current_goal_; }

  void setRequesterId(const std::string& requester_id) { requester_id_ = requester_id; }
  const std::string& getRequesterId() const { return requester_id_; }

#ifdef ENABLE_GRPC
  // ========================================================================
  // gRPC Integration (Optional - only if ENABLE_GRPC defined)
  // ========================================================================

  /**
   * @brief Initialize gRPC clients for distributed operation
   *
   * Sets up connections to HMASCoordinator and ServiceRegistry.
   *
   * @param agent_id Agent's unique identifier
   * @param coordinator_address HMASCoordinator gRPC address
   * @param registry_address ServiceRegistry gRPC address
   * @param agent_type Agent type string (e.g., "ComponentLead", "ModuleLead")
   * @param level Hierarchy level (1 for ComponentLead, 2 for ModuleLead)
   * @param capabilities List of agent capabilities
   * @param max_concurrent_tasks Maximum concurrent task capacity
   * @throws std::runtime_error if registration fails
   */
  void initializeGrpc(const std::string& agent_id, const std::string& coordinator_address,
                      const std::string& registry_address, const std::string& agent_type,
                      uint8_t level, const std::vector<std::string>& capabilities,
                      uint32_t max_concurrent_tasks) {
    agent_type_ = agent_type;
    agent_level_ = level;

    // Create gRPC clients
    network::GrpcClientConfig coordinator_config;
    coordinator_config.server_address = coordinator_address;
    coordinator_client_ = std::make_unique<network::HMASCoordinatorClient>(coordinator_config);

    network::GrpcClientConfig registry_config;
    registry_config.server_address = registry_address;
    registry_client_ = std::make_unique<network::ServiceRegistryClient>(registry_config);

    // Register with ServiceRegistry
    hmas::AgentRegistration registration;
    registration.set_agent_id(agent_id);
    registration.set_agent_type(agent_type_);
    registration.set_level(level);
    for (const auto& cap : capabilities) {
      registration.add_capabilities(cap);
    }
    registration.set_max_concurrent_tasks(max_concurrent_tasks);

    try {
      auto response = registry_client_->registerAgent(registration);
      if (response.success()) {
        startHeartbeat(agent_id);
      } else {
        throw std::runtime_error("Failed to register with ServiceRegistry: " + response.message());
      }
    } catch (const std::exception& e) {
      throw std::runtime_error("gRPC registration failed: " + std::string(e.what()));
    }
  }

  /**
   * @brief Start heartbeat thread
   */
  void startHeartbeat(const std::string& agent_id) {
    heartbeat_running_.store(true);
    heartbeat_thread_ = std::thread([this, agent_id]() { heartbeatLoop(agent_id); });
  }

  /**
   * @brief Stop heartbeat thread
   */
  void stopHeartbeat() {
    heartbeat_running_.store(false);
    if (heartbeat_thread_.joinable()) {
      heartbeat_thread_.join();
    }
  }

  /**
   * @brief Query available child agents from service registry
   *
   * @param child_agent_type Type of child agents to query
   *                        (e.g., "ModuleLead" for ComponentLead,
   *                               "Task" for ModuleLead)
   * @return List of available child agent IDs
   */
  std::vector<std::string> queryAvailableChildren(const std::string& child_agent_type) {
    std::vector<std::string> available_agents;

    if (!registry_client_) {
      return available_agents;
    }

    try {
      // Query registry for agents of the specified type
      hmas::AgentQuery query;
      query.set_agent_type(child_agent_type);
      query.set_require_healthy(true);

      auto agents = registry_client_->queryAgents(query);

      // Extract agent IDs
      for (const auto& agent : agents) {
        available_agents.push_back(agent.agent_id());
      }
    } catch (const std::exception& e) {
      // Return empty list on error
    }

    return available_agents;
  }

  /**
   * @brief Get current task ID (for YAML processing)
   */
  const std::string& getCurrentTaskId() const { return current_task_id_; }

  /**
   * @brief Set current task ID
   */
  void setCurrentTaskId(const std::string& task_id) { current_task_id_ = task_id; }

  /**
   * @brief Get coordinator client (for result submission)
   */
  network::HMASCoordinatorClient* getCoordinatorClient() { return coordinator_client_.get(); }

  /**
   * @brief Shutdown gRPC connections
   */
  void shutdownGrpc() {
    stopHeartbeat();
    coordinator_client_.reset();
    registry_client_.reset();
  }

  /**
   * @brief Destructor - shuts down gRPC connections if enabled
   */
  ~CoordinationState() {
#ifdef ENABLE_GRPC
    shutdownGrpc();
#endif
  }

 private:
  /**
   * @brief Heartbeat loop (runs in separate thread)
   */
  void heartbeatLoop(const std::string& agent_id) {
    constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(1);

    while (heartbeat_running_.load()) {
      try {
        if (registry_client_) {
          hmas::Heartbeat heartbeat;
          heartbeat.set_agent_id(agent_id);
          heartbeat.set_timestamp(std::chrono::system_clock::now().time_since_epoch().count());
          heartbeat.set_active_tasks(received_results_);

          registry_client_->sendHeartbeat(heartbeat);
        }
      } catch (const std::exception&) {
        // Ignore heartbeat errors (continue trying)
      }

      std::this_thread::sleep_for(HEARTBEAT_INTERVAL);
    }
  }

  // gRPC state
  std::string agent_type_;
  uint8_t agent_level_{0};
  std::string current_task_id_;
  std::unique_ptr<network::HMASCoordinatorClient> coordinator_client_;
  std::unique_ptr<network::ServiceRegistryClient> registry_client_;
  std::atomic<bool> heartbeat_running_{false};
  std::thread heartbeat_thread_;
#endif  // ENABLE_GRPC

 private:
  // Core state
  StateEnum current_state_;
  std::vector<std::string> execution_trace_;
  mutable std::mutex state_mutex_;

  // Result aggregation
  size_t expected_results_{0};
  size_t received_results_{0};
  size_t failed_results_{0};
  std::vector<ResultType> results_;
  std::vector<std::string> failure_messages_;
  std::unordered_map<std::string, std::string> pending_subordinates_;
  mutable std::mutex results_mutex_;

  // Context
  std::string current_goal_;
  std::string requester_id_;
};

}  // namespace agents
}  // namespace keystone
