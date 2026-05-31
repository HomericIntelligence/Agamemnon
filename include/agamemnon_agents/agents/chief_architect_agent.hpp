#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <thread>

#include "agents/async_agent.hpp"

#ifdef ENABLE_GRPC
#include "network/grpc_client.hpp"
#include "network/yaml_parser.hpp"
#endif

namespace keystone {
namespace agents {

/**
 * @brief Level 0 Chief Architect Agent
 *
 * Strategic orchestrator that delegates tasks to lower-level agents.
 * For Phase 1, delegates directly to TaskAgent (L3).
 */
class ChiefArchitectAgent : public AsyncAgent {
 public:
  /**
   * @brief Construct a new Chief Architect Agent
   *
   * @param agent_id Unique identifier for this agent
   */
  explicit ChiefArchitectAgent(const std::string& agent_id);

  /**
   * @brief Process incoming message asynchronously
   *
   * FIX C3: Changed to async (returns Task<Response>)
   *
   * @param msg Message to process
   * @return concurrency::Task<core::Response> Async task with response
   */
  concurrency::Task<core::Response> processMessage(const core::KeystoneMessage& msg) override;

  /**
   * @brief Send a command to a task agent and wait for response
   *
   * FIX C3: Changed to async (returns Task<Response>)
   *
   * @param command Command string to execute
   * @param task_agent_id Target task agent ID
   * @return concurrency::Task<core::Response> Async task with response
   */
  concurrency::Task<core::Response> sendCommand(const std::string& command,
                                                const std::string& task_agent_id);

#ifdef ENABLE_GRPC
  /**
   * @brief Initialize gRPC clients and register with ServiceRegistry
   *
   * @param coordinator_address HMASCoordinator server address
   * @param registry_address ServiceRegistry server address
   * @param agent_type Agent type (default: "ChiefArchitectAgent")
   * @param level Agent level (default: 0)
   */
  void initializeGrpc(const std::string& coordinator_address, const std::string& registry_address,
                      const std::string& agent_type = "ChiefArchitectAgent", uint8_t level = 0);

  /**
   * @brief Submit a user goal and get component result
   *
   * @param user_goal High-level user goal
   * @param session_id Session identifier
   * @return std::string Final result from the component
   */
  std::string submitUserGoal(const std::string& user_goal, const std::string& session_id = "");

  /**
   * @brief Start heartbeat thread (sends heartbeat every 1s)
   */
  void startHeartbeat();

  /**
   * @brief Stop heartbeat thread
   */
  void stopHeartbeat();

  /**
   * @brief Shutdown agent and unregister from ServiceRegistry
   */
  void shutdown();
#endif

 private:
#ifdef ENABLE_GRPC
  /**
   * @brief Heartbeat loop (runs in separate thread)
   */
  void heartbeatLoop();

  /**
   * @brief Query ServiceRegistry for available ComponentLeadAgents
   *
   * @return std::string ComponentLeadAgent ID, or empty if none available
   */
  std::string queryComponentLeadAgent();

  // gRPC clients
  std::unique_ptr<network::HMASCoordinatorClient> coordinator_client_;
  std::unique_ptr<network::ServiceRegistryClient> registry_client_;

  // Heartbeat thread
  std::thread heartbeat_thread_;
  std::atomic<bool> heartbeat_running_{false};

  // Agent metadata
  std::string agent_type_{"ChiefArchitectAgent"};
  uint8_t agent_level_{0};
#endif
};

}  // namespace agents
}  // namespace keystone
