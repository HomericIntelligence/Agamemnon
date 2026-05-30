#include "core/message.hpp"

#include <iomanip>
#include <random>
#include <sstream>

namespace keystone {
namespace core {

// ---------------------------------------------------------------------------
// Out-of-line special-member definitions for KeystoneMessage.
//
// Defined here (rather than defaulted in the header) so that we can suppress
// -Wdeprecated-declarations for the internal copy/move of the deprecated
// 'command' field.  Callers that access 'command' directly still get the
// warning.
// ---------------------------------------------------------------------------
_Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")

    KeystoneMessage::KeystoneMessage() = default;
KeystoneMessage::KeystoneMessage(const KeystoneMessage&) = default;
KeystoneMessage::KeystoneMessage(KeystoneMessage&&) noexcept = default;
KeystoneMessage& KeystoneMessage::operator=(const KeystoneMessage&) = default;
KeystoneMessage& KeystoneMessage::operator=(KeystoneMessage&&) noexcept = default;
KeystoneMessage::~KeystoneMessage() = default;

_Pragma("GCC diagnostic pop")

    namespace {
  // Simple UUID generation (not cryptographically secure, but sufficient for
  // Phase 1) Thread-safe: uses thread_local to avoid data races across threads
  std::string generate_uuid() {
    thread_local std::random_device rd;
    thread_local std::mt19937 gen(rd());
    thread_local std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";

    std::stringstream ss;
    for (int32_t i = 0; i < 32; ++i) {
      if (i == 8 || i == 12 || i == 16 || i == 20) {
        ss << '-';
      }
      ss << hex[dis(gen)];
    }
    return ss.str();
  }
}  // namespace

KeystoneMessage KeystoneMessage::create(const std::string& sender, const std::string& receiver,
                                        const std::string& cmd,
                                        const std::optional<std::string>& data) {
  KeystoneMessage msg;
  msg.msg_id = generate_uuid();
  msg.sender_id = sender;
  msg.receiver_id = receiver;
  _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
      msg.command = cmd;
  _Pragma("GCC diagnostic pop") msg.payload = data;
  msg.timestamp = std::chrono::system_clock::now();

  // Phase A: Initialize new fields with defaults for backward compatibility
  msg.action_type = ActionType::EXECUTE;
  msg.content_type = ContentType::TEXT_PLAIN;
  msg.session_id = "default";
  // metadata is empty by default

  // Phase C: Initialize priority to NORMAL by default
  msg.priority = Priority::NORMAL;

  return msg;
}

KeystoneMessage KeystoneMessage::create(const std::string& sender, const std::string& receiver,
                                        ActionType action, const std::string& session,
                                        const std::optional<std::string>& data,
                                        ContentType content) {
  KeystoneMessage msg;
  msg.msg_id = generate_uuid();
  msg.sender_id = sender;
  msg.receiver_id = receiver;
  msg.action_type = action;
  msg.content_type = content;
  msg.session_id = session;
  msg.payload = data;
  msg.timestamp = std::chrono::system_clock::now();

  // Legacy field: set command based on action type
  _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
      msg.command = actionTypeToString(action);
  _Pragma("GCC diagnostic pop")

      // Phase C: Initialize priority and deadline (FIX: was missing!)
      msg.priority = Priority::NORMAL;
  msg.deadline = std::nullopt;

  return msg;
}

void KeystoneMessage::setDeadlineFromNow(std::chrono::milliseconds duration_ms) {
  deadline = std::chrono::system_clock::now() + duration_ms;
}

bool KeystoneMessage::hasDeadlinePassed() const {
  if (!deadline.has_value()) {
    return false;  // No deadline means it can't be passed
  }
  return std::chrono::system_clock::now() > *deadline;
}

std::optional<std::chrono::milliseconds> KeystoneMessage::getTimeUntilDeadline() const {
  if (!deadline.has_value()) {
    return std::nullopt;
  }

  auto now = std::chrono::system_clock::now();
  if (now > *deadline) {
    return std::chrono::milliseconds(0);  // Deadline already passed
  }

  return std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
}

KeystoneMessage KeystoneMessage::createCancellation(const std::string& sender,
                                                    const std::string& receiver,
                                                    const std::string& task_id,
                                                    const std::string& session) {
  KeystoneMessage msg;
  msg.msg_id = generate_uuid();
  msg.sender_id = sender;
  msg.receiver_id = receiver;
  msg.action_type = ActionType::CANCEL_TASK;
  msg.content_type = ContentType::TEXT_PLAIN;
  msg.session_id = session;
  msg.task_id = task_id;          // Set the task to cancel
  msg.priority = Priority::HIGH;  // Cancellations are high priority
  _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
      msg.command = "CANCEL_TASK";
  _Pragma("GCC diagnostic pop") msg.timestamp = std::chrono::system_clock::now();
  return msg;
}

Response Response::createSuccess(const KeystoneMessage& original_msg, const std::string& sender,
                                 const std::string& result_data) {
  Response resp;
  resp.msg_id = original_msg.msg_id;
  resp.sender_id = sender;
  resp.receiver_id = original_msg.sender_id;
  resp.status = Status::Success;
  resp.result = result_data;
  resp.timestamp = std::chrono::system_clock::now();
  return resp;
}

Response Response::createError(const KeystoneMessage& original_msg, const std::string& sender,
                               const std::string& error_msg) {
  Response resp;
  resp.msg_id = original_msg.msg_id;
  resp.sender_id = sender;
  resp.receiver_id = original_msg.sender_id;
  resp.status = Status::Error;
  resp.result = error_msg;
  resp.timestamp = std::chrono::system_clock::now();
  return resp;
}

}  // namespace core
}  // namespace keystone
