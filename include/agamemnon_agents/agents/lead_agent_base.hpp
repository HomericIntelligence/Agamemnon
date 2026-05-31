#pragma once

#include <string>
#include <vector>

#include "agents/async_agent.hpp"
#include "agents/coordination_state.hpp"
#include "core/message.hpp"

#ifdef ENABLE_GRPC
#include "hmas_coordinator.pb.h"
#include "network/yaml_parser.hpp"
#endif

namespace keystone {
namespace agents {

/**
 * @brief Base class for Lead Agents using Template Method Pattern
 *
 * Eliminates code duplication between ComponentLeadAgent and ModuleLeadAgent
 * by extracting common message processing workflow.
 *
 * Template Method: processMessage() defines the workflow skeleton
 * Hook Methods: Subclasses override pure virtual methods for specific behavior
 *
 * @tparam StateEnum Agent-specific state enum (ComponentLeadAgent::State or
 *         ModuleLeadAgent::State)
 */
template <typename StateEnum>
class LeadAgentBase : public AsyncAgent {
 public:
  /**
   * @brief Construct a new Lead Agent Base
   *
   * @param agent_id Unique identifier for this agent
   * @param idle_state Initial state (IDLE)
   * @param planning_state Planning state value
   * @param waiting_state Waiting for subordinates state value
   * @param aggregating_state Aggregating/Synthesizing results state value
   * @param error_state Error state value
   */
  explicit LeadAgentBase(const std::string& agent_id, StateEnum idle_state,
                         StateEnum planning_state, StateEnum waiting_state,
                         StateEnum aggregating_state, StateEnum error_state);

  /**
   * @brief Process incoming message asynchronously (TEMPLATE METHOD - FINAL)
   *
   * This method defines the common workflow for all lead agents:
   * 1. Handle CANCEL_TASK messages
   * 2. Check if message is subordinate result or new goal
   * 3. If result: process and check completion
   * 4. If goal: decompose → delegate → return success
   *
   * Subclasses CANNOT override this method. They must implement
   * the pure virtual hook methods instead.
   *
   * @param msg Message to process
   * @return concurrency::Task<core::Response> Async task with response
   */
  concurrency::Task<core::Response> processMessage(const core::KeystoneMessage& msg) final;

  /**
   * @brief Get execution trace for testing/debugging
   *
   * @return std::vector<std::string> State transition history
   */
  std::vector<std::string> getExecutionTrace() const { return coordination_.getExecutionTrace(); }

  /**
   * @brief Get current state
   *
   * @return StateEnum Current agent state
   */
  StateEnum getCurrentState() const { return coordination_.getCurrentState(); }

 protected:
  /**
   * @brief HOOK: Decompose high-level goal into subtasks
   *
   * Subclasses must implement this to define their decomposition logic.
   * Example:
   * - ComponentLead: "Component goal" -> ["Module 1", "Module 2"]
   * - ModuleLead: "Module goal" -> ["Task 1", "Task 2", "Task 3"]
   *
   * @param goal High-level goal string
   * @return std::vector<std::string> List of subtasks/subgoals
   */
  virtual std::vector<std::string> decomposeGoal(const std::string& goal) = 0;

  /**
   * @brief HOOK: Delegate subtasks to subordinate agents
   *
   * Subclasses must implement this to send messages to their subordinates.
   * Example:
   * - ComponentLead: Send module goals to ModuleLeadAgents
   * - ModuleLead: Send tasks to TaskAgents
   *
   * @param subtasks List of subtasks to delegate
   */
  virtual void delegateSubtasks(const std::vector<std::string>& subtasks) = 0;

  /**
   * @brief HOOK: Check if message is a subordinate result
   *
   * Subclasses must implement this to identify result messages.
   * Example:
   * - ComponentLead: Check if msg.command == "module_result"
   * - ModuleLead: Check if msg.command == "response"
   *
   * @param msg Message to check
   * @return true If message is a subordinate result
   * @return false If message is a new goal
   */
  virtual bool isSubordinateResult(const core::KeystoneMessage& msg) = 0;

  /**
   * @brief HOOK: Process a result from a subordinate agent
   *
   * Subclasses must implement this to handle result messages.
   * This method should:
   * 1. Extract result from message payload
   * 2. Record result in coordination state
   * 3. Check if all results are complete
   * 4. Transition to aggregating state if complete
   *
   * @param result_msg Message containing subordinate result
   */
  virtual void processSubordinateResult(const core::KeystoneMessage& result_msg) = 0;

  /**
   * @brief HOOK: Handle a failure reported by a subordinate agent (Issue #87)
   *
   * Called when a subordinate sends a TASK_FAILED message. The default
   * implementation records the failure in coordination state and transitions
   * to ERROR when all results (successes + failures) have been received,
   * preventing permanent DAG deadlock.
   *
   * Subclasses may override to add custom failure propagation logic.
   *
   * @param failure_msg Message with action_type == TASK_FAILED
   */
  virtual void processSubordinateFailure(const core::KeystoneMessage& failure_msg) {
    std::string error = failure_msg.payload.value_or("subordinate task failed");
    bool all_done = coordination_.recordFailure(error);
    if (all_done) {
      coordination_.transitionTo(error_state_, stateToString(error_state_));

      // Propagate failure upward to grandparent (Issue #185).
      // When all subordinates have reported terminal events and at least one
      // has failed, send a TASK_FAILED message to our own requester so the
      // ComponentLeadAgent (or ChiefArchitect) above us does not remain
      // permanently stuck in WAITING_FOR_MODULES / WAITING_FOR_TASKS.
      const std::string& parent_id = coordination_.getRequesterId();
      if (!parent_id.empty()) {
        auto failed_msg = core::KeystoneMessage::create(
            agent_id_, parent_id, core::ActionType::TASK_FAILED, failure_msg.session_id, error);
        sendMessage(failed_msg);
      }
    }
  }

  /**
   * @brief HOOK: Convert state enum to string for logging
   *
   * Subclasses must implement this to provide state names for tracing.
   *
   * @param state State to convert
   * @return std::string String representation
   */
  virtual std::string stateToString(StateEnum state) const = 0;

#ifdef ENABLE_GRPC
  /**
   * @brief Mark spec as FAILED and submit the error result via gRPC if
   * possible.
   *
   * Shared implementation extracted from ComponentLeadAgent and ModuleLeadAgent
   * (Issue #348). Both subclasses had byte-for-byte identical bodies.
   *
   * @param spec  Task spec to update (modified in place)
   * @param error Human-readable error message
   */
  void submitFailureResult(network::HierarchicalTaskSpec& spec, const std::string& error) {
    spec.status.phase = "FAILED";
    spec.status.error = error;
    this->coordination_.transitionTo(this->error_state_, stateToString(this->error_state_));

    std::string result_yaml = network::YamlParser::generateTaskSpec(spec);
    auto coordinator_client = this->coordination_.getCoordinatorClient();
    if (coordinator_client && spec.metadata.parent_task_id) {
      hmas::TaskResult task_result;
      task_result.set_task_id(spec.metadata.task_id);
      task_result.set_result_yaml(result_yaml);
      task_result.set_success(false);
      task_result.set_error_message(*spec.status.error);
      coordinator_client->submitResult(task_result);
    }
  }
#endif

  // Coordination state (shared by all lead agents)
  CoordinationState<StateEnum, std::string> coordination_;

  // State values (provided by subclass constructor)
  StateEnum idle_state_;
  StateEnum planning_state_;
  StateEnum waiting_state_;
  StateEnum aggregating_state_;
  StateEnum error_state_;
};

}  // namespace agents
}  // namespace keystone

// Include implementation (template class must be in header)
#include "agents/lead_agent_base_impl.hpp"
