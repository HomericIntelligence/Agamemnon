#include "agamemnon/agents/agent_envelope.hpp"

#include <string_view>

namespace agamemnon {
namespace agents {

namespace {
// Payload prefix convention for encoding AgentActionType on the wire.
// The transport layer sees only EXECUTE action_type + a payload; the agent
// layer decodes the orchestration intent from the prefix.
constexpr std::string_view kCancelPrefix = "CANCEL_TASK:";
constexpr std::string_view kFailedPrefix = "TASK_FAILED:";
constexpr std::string_view kDecomposePrefix = "DECOMPOSE:";
}  // namespace

// static
AgentEnvelope AgentEnvelope::wrap(const TransportMessage& msg) {
  AgentEnvelope env;
  env.transport_msg = msg;
  env.session_id = "default";

  if (msg.payload.has_value()) {
    const std::string& p = *msg.payload;
    if (p.rfind(std::string(kCancelPrefix), 0) == 0) {
      env.agent_action = AgentActionType::CANCEL_TASK;
      // task_id follows the prefix
      std::string tid = p.substr(kCancelPrefix.size());
      if (!tid.empty()) {
        env.task_id = tid;
      }
    } else if (p.rfind(std::string(kFailedPrefix), 0) == 0) {
      env.agent_action = AgentActionType::TASK_FAILED;
    } else if (p.rfind(std::string(kDecomposePrefix), 0) == 0) {
      env.agent_action = AgentActionType::DECOMPOSE;
    }
  }

  return env;
}

// static
AgentEnvelope AgentEnvelope::create(const std::string& sender, const std::string& receiver,
                                    AgentActionType action, const std::string& session,
                                    const std::optional<std::string>& data) {
  AgentEnvelope env;
  env.agent_action = action;
  env.session_id = session;

  // Encode agent_action as payload prefix so the transport message remains
  // self-describing to agent receivers that call wrap().
  std::string prefix;
  switch (action) {
    case AgentActionType::CANCEL_TASK:
      prefix = std::string(kCancelPrefix);
      break;
    case AgentActionType::TASK_FAILED:
      prefix = std::string(kFailedPrefix);
      break;
    case AgentActionType::DECOMPOSE:
      prefix = std::string(kDecomposePrefix);
      break;
  }

  std::string payload = prefix + data.value_or("");
  env.transport_msg =
      TransportMessage::create(sender, receiver, TransportActionType::EXECUTE, payload);

  return env;
}

// static
AgentEnvelope AgentEnvelope::createCancellation(const std::string& sender,
                                                const std::string& receiver,
                                                const std::string& task_id_val,
                                                const std::string& session) {
  AgentEnvelope env;
  env.agent_action = AgentActionType::CANCEL_TASK;
  env.session_id = session;
  env.task_id = task_id_val;

  // Encode task_id in the payload so the receiver can decode it via wrap().
  std::string payload = std::string(kCancelPrefix) + task_id_val;
  env.transport_msg =
      TransportMessage::create(sender, receiver, TransportActionType::EXECUTE, payload);
  env.transport_msg.priority = TransportPriority::HIGH;

  return env;
}

// static
AgentEnvelope AgentEnvelope::createFailure(const std::string& sender, const std::string& receiver,
                                           const std::string& error_msg,
                                           const std::string& session) {
  AgentEnvelope env;
  env.agent_action = AgentActionType::TASK_FAILED;
  env.session_id = session;

  std::string payload = std::string(kFailedPrefix) + error_msg;
  env.transport_msg =
      TransportMessage::create(sender, receiver, TransportActionType::EXECUTE, payload);

  return env;
}

}  // namespace agents
}  // namespace agamemnon
