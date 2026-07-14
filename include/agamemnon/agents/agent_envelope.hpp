#pragma once

#include "agamemnon/agents/agent_action_type.hpp"

#include <map>
#include <optional>
#include <string>

namespace agamemnon {
namespace agents {

/**
 * @brief Transport-level action types understood by the pure transport layer.
 *
 * Mirrors the slimmed ProjectKeystone core::ActionType (post Issue #515): the
 * transport layer understands only EXECUTE / RETURN_RESULT / SHUTDOWN.
 * Orchestration semantics live in AgentActionType (this file's sibling).
 */
enum class TransportActionType {
  EXECUTE,        ///< Execute a concrete task or command
  RETURN_RESULT,  ///< Return the result of a computation
  SHUTDOWN,       ///< Graceful shutdown signal
};

/**
 * @brief Transport-level message priority levels.
 */
enum class TransportPriority {
  HIGH = 0,    ///< Urgent, time-sensitive messages
  NORMAL = 1,  ///< Standard priority (default)
  LOW = 2,     ///< Background, non-urgent messages
};

/**
 * @brief Minimal vendored transport message struct.
 *
 * Agamemnon does not link ProjectKeystone's C++ headers directly; all
 * inter-component traffic crosses Keystone as opaque (subject, payload)
 * strings.  This struct is the minimal transport shape AgentEnvelope wraps:
 * routing identifiers, a transport action, an optional payload, and a
 * priority.  It deliberately carries NO orchestration fields (session_id,
 * task_id, metadata, orchestration actions) — those are the envelope's job.
 *
 * If/when Agamemnon gains a richer transport type (e.g. a generated KIM
 * binding), this struct can be replaced by a thin alias without changing the
 * AgentEnvelope contract.
 */
struct TransportMessage {
  std::string sender_id;                                          ///< ID of the sending agent
  std::string receiver_id;                                        ///< ID of the receiving agent
  TransportActionType action_type{TransportActionType::EXECUTE};  ///< Transport action
  std::optional<std::string> payload;                             ///< Optional payload data
  TransportPriority priority{TransportPriority::NORMAL};          ///< Message priority

  /**
   * @brief Create a transport message.
   *
   * @param sender Sender agent ID
   * @param receiver Receiver agent ID
   * @param action Transport action type
   * @param data Optional payload data
   * @return TransportMessage populated message
   */
  static TransportMessage create(const std::string& sender, const std::string& receiver,
                                 TransportActionType action,
                                 const std::optional<std::string>& data = std::nullopt) {
    TransportMessage msg;
    msg.sender_id = sender;
    msg.receiver_id = receiver;
    msg.action_type = action;
    msg.payload = data;
    return msg;
  }
};

/**
 * @brief Agent-layer message envelope wrapping a transport message.
 *
 * AgentEnvelope sits above the transport layer and carries orchestration-level
 * metadata that does not belong on a transport message (a pure transport
 * struct).  It was introduced for Keystone Issue #515 (SOLID/SRP: remove
 * orchestration concerns from the transport struct) and, per ADR-015, the
 * orchestration half lives in Agamemnon — agent-orchestration types
 * belong here, not in Keystone's pure transport layer.
 *
 * The transport layer (MessageBus, NATS bridge) never sees this type; it only
 * passes raw transport messages.  Agent code that needs session isolation,
 * task tracking, or orchestration action semantics wraps the incoming
 * transport message in an AgentEnvelope:
 *
 *   auto env = AgentEnvelope::wrap(msg);
 *   if (env.agent_action == AgentActionType::CANCEL_TASK) { ... }
 *
 * The agent_action field is populated by decoding the payload prefix
 * convention agreed between agents (see wrap()).
 */
struct AgentEnvelope {
  /// The underlying transport message
  TransportMessage transport_msg;

  /// Agent-level action type (orchestration semantics)
  std::optional<AgentActionType> agent_action;

  /// Session/context identifier for concurrent operation isolation
  std::string session_id{"default"};

  /// Optional task identifier for tracking/cancellation
  std::optional<std::string> task_id;

  /// Extensible key-value metadata (agent-layer, not serialized on wire)
  std::map<std::string, std::string> metadata;

  /**
   * @brief Wrap a raw transport message in an AgentEnvelope.
   *
   * Decodes agent_action from the transport message payload. The convention:
   * - transport EXECUTE with payload prefix "CANCEL_TASK:" → CANCEL_TASK
   * - transport EXECUTE with payload prefix "TASK_FAILED:" → TASK_FAILED
   * - transport EXECUTE with payload prefix "DECOMPOSE:"   → DECOMPOSE
   * All other messages have agent_action == std::nullopt (pure transport).
   *
   * @param msg Raw transport message
   * @return AgentEnvelope wrapping the message
   */
  static AgentEnvelope wrap(const TransportMessage& msg);

  /**
   * @brief Create an agent envelope for a new outgoing message.
   *
   * @param sender Sender agent ID
   * @param receiver Receiver agent ID
   * @param action Agent-level action type
   * @param session Session identifier
   * @param data Optional payload
   * @return AgentEnvelope with transport_msg populated
   */
  static AgentEnvelope create(const std::string& sender, const std::string& receiver,
                              AgentActionType action, const std::string& session = "default",
                              const std::optional<std::string>& data = std::nullopt);

  /**
   * @brief Create a task cancellation envelope.
   *
   * Cancellation is cooperative: agents check and respond gracefully.
   *
   * @param sender Sender agent ID (parent requesting cancellation)
   * @param receiver Receiver agent ID (child executing the task)
   * @param task_id_val Task identifier to cancel
   * @param session Session identifier
   * @return AgentEnvelope with CANCEL_TASK agent_action
   */
  static AgentEnvelope createCancellation(const std::string& sender, const std::string& receiver,
                                          const std::string& task_id_val,
                                          const std::string& session = "default");

  /**
   * @brief Create a task failure notification envelope.
   *
   * Sent by a subordinate agent to its parent when execution fails.
   *
   * @param sender Sender agent ID (child reporting failure)
   * @param receiver Receiver agent ID (parent waiting for result)
   * @param error_msg Human-readable failure description
   * @param session Session identifier
   * @return AgentEnvelope with TASK_FAILED agent_action
   */
  static AgentEnvelope createFailure(const std::string& sender, const std::string& receiver,
                                     const std::string& error_msg,
                                     const std::string& session = "default");
};

}  // namespace agents
}  // namespace agamemnon
