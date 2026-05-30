#pragma once

// This file contains the implementation of LeadAgentBase template class
// It is included by lead_agent_base.hpp

namespace keystone {
namespace agents {

template <typename StateEnum>
LeadAgentBase<StateEnum>::LeadAgentBase(const std::string& agent_id, StateEnum idle_state,
                                        StateEnum planning_state, StateEnum waiting_state,
                                        StateEnum aggregating_state, StateEnum error_state)
    : AsyncAgent(agent_id),
      idle_state_(idle_state),
      planning_state_(planning_state),
      waiting_state_(waiting_state),
      aggregating_state_(aggregating_state),
      error_state_(error_state) {
  // Initialize to IDLE state (hardcoded string to avoid calling pure virtual in
  // constructor)
  coordination_.transitionTo(idle_state_, "IDLE");
}

template <typename StateEnum>
concurrency::Task<core::Response> LeadAgentBase<StateEnum>::processMessage(
    const core::KeystoneMessage& msg) {
  // Step 1: Handle CANCEL_TASK action type (from
  // MESSAGE_PROTOCOL_EXTENSIONS.md)
  if (msg.action_type == core::ActionType::CANCEL_TASK) {
    auto response = handleCancellation(msg);

    // Send acknowledgement back to sender via MessageBus
    auto response_msg =
        core::KeystoneMessage::create(agent_id_,
                                      msg.sender_id,  // Route back to original sender
                                      "response", response.result);
    response_msg.msg_id = msg.msg_id;  // Keep same msg_id for tracking

    sendMessage(response_msg);

    co_return response;
  }

  // Step 2a: Handle subordinate failure (Issue #87 - prevents DAG deadlock)
  if (msg.action_type == core::ActionType::TASK_FAILED) {
    processSubordinateFailure(msg);
    co_return core::Response::createSuccess(msg, agent_id_, "Subordinate failure recorded");
  }

  // Step 2b: Check if this is a subordinate result or a new goal
  if (isSubordinateResult(msg)) {
    // This is a subordinate result - process it
    processSubordinateResult(msg);
    co_return core::Response::createSuccess(msg, agent_id_, "Subordinate result processed");
  }

  // Step 3: This is a new goal - decompose and delegate
  coordination_.transitionTo(planning_state_, stateToString(planning_state_));

  _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
      coordination_.setCurrentGoal(msg.command);
  _Pragma("GCC diagnostic pop") coordination_.setRequesterId(msg.sender_id);

  // Step 4: Decompose goal into subtasks (hook method - subclass implements)
  auto subtasks = decomposeGoal(coordination_.getCurrentGoal());

  if (subtasks.empty()) {
    coordination_.transitionTo(error_state_, stateToString(error_state_));
    co_return core::Response::createError(msg, agent_id_, "Failed to decompose goal");
  }

  // SECURITY FIX: Check for integer overflow before cast
  // size_t can be much larger than int (e.g., 2^64-1 vs 2^31-1)
  if (subtasks.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    coordination_.transitionTo(error_state_, stateToString(error_state_));
    co_return core::Response::createError(
        msg, agent_id_,
        "Subtask count exceeds maximum: " + std::to_string(subtasks.size()) + " > " +
            std::to_string(std::numeric_limits<int>::max()));
  }

  // Step 5: Initialize coordination for expected results
  coordination_.initializeCoordination(subtasks.size());

  // Step 6: Transition to waiting state and delegate
  coordination_.transitionTo(waiting_state_, stateToString(waiting_state_));
  delegateSubtasks(subtasks);  // Hook method - subclass implements

  // Step 7: Return success response
  co_return core::Response::createSuccess(
      msg, agent_id_, "Goal decomposed into " + std::to_string(subtasks.size()) + " subtasks");
}

}  // namespace agents
}  // namespace keystone
