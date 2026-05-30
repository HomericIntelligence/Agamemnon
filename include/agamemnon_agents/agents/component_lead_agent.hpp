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
 * @brief State enum for ComponentLeadAgent workflow tracking
 */
enum class ComponentLeadState {
  IDLE,                 // No active work
  PLANNING,             // Decomposing component goal into modules
  WAITING_FOR_MODULES,  // Modules delegated, waiting for results
  AGGREGATING,          // Combining results from all modules
  ERROR                 // Error state
};

/**
 * @brief Level 1 Component Lead Agent
 *
 * Coordinates multiple ModuleLeadAgents to accomplish component-level goals.
 * Uses Template Method Pattern from LeadAgentBase to eliminate code
 * duplication.
 *
 * Responsibilities:
 * - Decompose component goals into module goals
 * - Distribute module goals to available ModuleLeadAgents
 * - Collect and aggregate results from ModuleLeadAgents
 * - Report aggregated results to ChiefArchitect
 *
 * State Machine: IDLE → PLANNING → WAITING_FOR_MODULES → AGGREGATING → IDLE
 */
class ComponentLeadAgent : public LeadAgentBase<ComponentLeadState> {
 public:
  // Type alias for backward compatibility with existing code
  using State = ComponentLeadState;

  /**
   * @brief Construct a new Component Lead Agent
   *
   * @param agent_id Unique identifier for this agent
   */
  explicit ComponentLeadAgent(const std::string& agent_id);

  /**
   * @brief Configure available ModuleLeadAgents for delegation
   *
   * @param module_lead_ids List of ModuleLead IDs available for work
   */
  void setAvailableModuleLeads(const std::vector<std::string>& module_lead_ids);

  /**
   * @brief Aggregate results from all completed modules
   *
   * Public API for manual result aggregation (typically called after all
   * results received)
   *
   * @return std::string Aggregated component-level result
   */
  std::string synthesizeComponentResult();

  // getExecutionTrace() and getCurrentState() inherited from LeadAgentBase

#ifdef ENABLE_GRPC
  /**
   * @brief Initialize gRPC clients and register with ServiceRegistry
   *
   * @param coordinator_address HMASCoordinator server address
   * @param registry_address ServiceRegistry server address
   * @param agent_type Agent type (default: "ComponentLeadAgent")
   * @param level Agent level (default: 1)
   */
  void initializeGrpc(const std::string& coordinator_address, const std::string& registry_address,
                      const std::string& agent_type = "ComponentLeadAgent", uint8_t level = 1);

  /**
   * @brief Process incoming YAML task specification
   *
   * @param yaml_spec YAML component specification
   */
  void processYamlComponent(const std::string& yaml_spec);

  /**
   * @brief Handle a gRPC TaskResult callback from a child ModuleLeadAgent
   * (Issue #186)
   *
   * Called externally when the coordinator delivers a module result via gRPC.
   * Checks hmas::TaskResult::success() and calls coordination_.recordFailure()
   * when the result is not successful so that hasFailures() is set correctly
   * and the agent does not remain permanently stuck in WAITING_FOR_MODULES.
   *
   * @param subtask_id Identifier of the child module task (child task_id)
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
   * @brief Decompose component goal into module goals (HOOK METHOD)
   *
   * @param goal High-level component goal
   * @return std::vector<std::string> List of module goals
   */
  std::vector<std::string> decomposeGoal(const std::string& goal) override;

  /**
   * @brief Delegate module goals to available ModuleLeadAgents (HOOK METHOD)
   *
   * @param subtasks List of module goals to delegate
   */
  void delegateSubtasks(const std::vector<std::string>& subtasks) override;

  /**
   * @brief Check if message is a module result (HOOK METHOD)
   *
   * @param msg Message to check
   * @return true If msg.command == "module_result"
   */
  bool isSubordinateResult(const core::KeystoneMessage& msg) override;

  /**
   * @brief Process a result from a ModuleLeadAgent (HOOK METHOD)
   *
   * @param result_msg Message containing module result
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
  std::vector<std::string> available_module_leads_;

#ifdef ENABLE_GRPC
  // Result aggregator (component-specific, not in template)
  std::unique_ptr<network::ResultAggregator> result_aggregator_;
#endif
};

}  // namespace agents
}  // namespace keystone
