#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agents/async_agent.hpp"
#include "agents/coordination_state.hpp"
#include "agents/lead_agent_base.hpp"

#ifdef ENABLE_GRPC
#include "network/result_aggregator.hpp"
#include "network/yaml_parser.hpp"
#endif

namespace keystone {
namespace agents {

/**
 * @brief State enum for ModuleLeadAgent workflow tracking
 */
enum class ModuleLeadState {
  IDLE,               // No active work
  PLANNING,           // Decomposing module goal into tasks
  WAITING_FOR_TASKS,  // Tasks delegated, waiting for results
  SYNTHESIZING,       // Combining results from all tasks
  ERROR               // Error state
};

/**
 * @brief Level 2 Module Lead Agent
 *
 * Coordinates multiple TaskAgents to accomplish module-level goals.
 * Uses Template Method Pattern from LeadAgentBase to eliminate code
 * duplication.
 *
 * Responsibilities:
 * - Decompose module goals into individual tasks
 * - Distribute tasks to available TaskAgents
 * - Collect and synthesize results from TaskAgents
 * - Report synthesized results to ChiefArchitect
 *
 * State Machine: IDLE → PLANNING → WAITING_FOR_TASKS → SYNTHESIZING → IDLE
 */
class ModuleLeadAgent : public LeadAgentBase<ModuleLeadState> {
 public:
  // Type alias for backward compatibility with existing code
  using State = ModuleLeadState;

  /**
   * @brief Construct a new Module Lead Agent
   *
   * @param agent_id Unique identifier for this agent
   */
  explicit ModuleLeadAgent(const std::string& agent_id);

  /**
   * @brief Configure available TaskAgents for delegation
   *
   * @param task_agent_ids List of TaskAgent IDs available for work
   */
  void setAvailableTaskAgents(const std::vector<std::string>& task_agent_ids);

  /**
   * @brief Synthesize results from all completed tasks
   *
   * Public API for manual result synthesis (typically called after all results
   * received)
   *
   * @return std::string Synthesized module-level result
   */
  std::string synthesizeResults();

  // getExecutionTrace() and getCurrentState() inherited from LeadAgentBase

#ifdef ENABLE_GRPC
  /**
   * @brief Initialize gRPC clients and register with ServiceRegistry
   *
   * @param coordinator_address HMASCoordinator server address
   * @param registry_address ServiceRegistry server address
   * @param agent_type Agent type (default: "ModuleLeadAgent")
   * @param level Agent level (default: 2)
   */
  void initializeGrpc(const std::string& coordinator_address, const std::string& registry_address,
                      const std::string& agent_type = "ModuleLeadAgent", uint8_t level = 2);

  /**
   * @brief Process incoming YAML task specification
   *
   * @param yaml_spec YAML module specification
   */
  void processYamlModule(const std::string& yaml_spec);

  /**
   * @brief Handle a gRPC TaskResult callback from a child TaskAgent (Issue
   * #186)
   *
   * Called externally when the coordinator delivers a task result via gRPC.
   * Checks hmas::TaskResult::success() and calls coordination_.recordFailure()
   * when the result is not successful so that hasFailures() is set correctly
   * and the agent does not remain permanently stuck in WAITING_FOR_TASKS.
   *
   * @param subtask_id Identifier of the child task (child task_id)
   * @param result     gRPC TaskResult from the coordinator
   */
  void processTaskResult(const std::string& subtask_id, const hmas::TaskResult& result);

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

 protected:
  // === Hook Methods (override pure virtuals from LeadAgentBase) ===

  /**
   * @brief Decompose module goal into individual tasks (HOOK METHOD)
   *
   * @param goal High-level module goal
   * @return std::vector<std::string> List of individual tasks
   */
  std::vector<std::string> decomposeGoal(const std::string& goal) override;

  /**
   * @brief Delegate tasks to available TaskAgents (HOOK METHOD)
   *
   * @param subtasks List of tasks to delegate
   */
  void delegateSubtasks(const std::vector<std::string>& subtasks) override;

  /**
   * @brief Check if message is a task result (HOOK METHOD)
   *
   * @param msg Message to check
   * @return true If msg.command == "response"
   */
  bool isSubordinateResult(const core::KeystoneMessage& msg) override;

  /**
   * @brief Process a result from a TaskAgent (HOOK METHOD)
   *
   * @param result_msg Message containing task result
   */
  void processSubordinateResult(const core::KeystoneMessage& result_msg) override;

  /**
   * @brief Convert state enum to string (HOOK METHOD)
   *
   * @param state State to convert
   * @return std::string String representation
   */
  std::string stateToString(State state) const override;

 private:
  // Agent-specific configuration
  std::vector<std::string> available_task_agents_;

#ifdef ENABLE_GRPC
  // Result aggregator (module-specific, not in template)
  std::unique_ptr<network::ResultAggregator> result_aggregator_;
#endif
};

}  // namespace agents
}  // namespace keystone
