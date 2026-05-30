#pragma once

#include <string>

namespace projectagamemnon {
namespace agents {

/**
 * @brief Agent-layer action types for HMAS orchestration
 *
 * These action types belong to the agent / orchestration layer
 * (ProjectAgamemnon) rather than to the transport layer.  They were moved out
 * of ProjectKeystone's transport struct per ADR-015 / Keystone Issue #515
 * (SOLID/SRP: a transport message must not carry orchestration semantics).
 *
 * Transport-level signals (EXECUTE, RETURN_RESULT, SHUTDOWN) stay on the
 * transport message's ActionType.  Orchestration-level signals live here.
 */
enum class AgentActionType {
  DECOMPOSE,    ///< Decompose a goal into subtasks/subgoals
  CANCEL_TASK,  ///< Cancel a running task
  TASK_FAILED   ///< Report task failure to parent agent
};

/**
 * @brief Convert AgentActionType to string
 */
inline std::string agentActionTypeToString(AgentActionType type) {
  switch (type) {
    case AgentActionType::DECOMPOSE:
      return "DECOMPOSE";
    case AgentActionType::CANCEL_TASK:
      return "CANCEL_TASK";
    case AgentActionType::TASK_FAILED:
      return "TASK_FAILED";
    default:
      return "UNKNOWN";
  }
}

}  // namespace agents
}  // namespace projectagamemnon
