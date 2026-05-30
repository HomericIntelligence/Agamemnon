#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace keystone {
namespace core {

/**
 * @brief Message priority levels
 *
 * Phase C: Priority-based message processing.
 * Higher priority messages are processed before lower priority ones.
 */
enum class Priority {
  HIGH = 0,    ///< Urgent, time-sensitive messages
  NORMAL = 1,  ///< Standard priority (default)
  LOW = 2      ///< Background, non-urgent messages
};

/**
 * @brief Convert Priority to string
 */
inline std::string priorityToString(Priority priority) {
  switch (priority) {
    case Priority::HIGH:
      return "HIGH";
    case Priority::NORMAL:
      return "NORMAL";
    case Priority::LOW:
      return "LOW";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief Action types for agent communication in HMAS
 *
 * Defines the different types of actions that can be requested or performed
 * in the hierarchical agent system.
 */
enum class ActionType {
  DECOMPOSE,      ///< Decompose a goal into subtasks/subgoals
  EXECUTE,        ///< Execute a concrete task or command
  RETURN_RESULT,  ///< Return the result of a computation
  SHUTDOWN,       ///< Graceful shutdown signal
  CANCEL_TASK,    ///< Cancel a running task (Issue #52)
  TASK_FAILED     ///< Report task failure to parent agent (Issue #87)
};

/**
 * @brief Content type for message payload
 *
 * Indicates the format/encoding of the message payload.
 */
enum class ContentType {
  TEXT_PLAIN,   ///< Plain text payload (default)
  BINARY_CISTA  ///< Binary payload serialized with Cista
};

/**
 * @brief Convert ActionType to string
 */
inline std::string actionTypeToString(ActionType type) {
  switch (type) {
    case ActionType::DECOMPOSE:
      return "DECOMPOSE";
    case ActionType::EXECUTE:
      return "EXECUTE";
    case ActionType::RETURN_RESULT:
      return "RETURN_RESULT";
    case ActionType::SHUTDOWN:
      return "SHUTDOWN";
    case ActionType::CANCEL_TASK:
      return "CANCEL_TASK";
    case ActionType::TASK_FAILED:
      return "TASK_FAILED";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief Convert ContentType to string
 */
inline std::string contentTypeToString(ContentType type) {
  switch (type) {
    case ContentType::TEXT_PLAIN:
      return "TEXT_PLAIN";
    case ContentType::BINARY_CISTA:
      return "BINARY_CISTA";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief Keystone Interchange Message (KIM) protocol
 *
 * Enhanced message structure for work-stealing concurrent agent communication.
 * Messages are passed between agents to delegate tasks and return results.
 *
 * Phase A additions:
 * - action_type: Semantic action classification
 * - content_type: Payload format specification
 * - session_id: Context isolation for concurrent operations
 * - metadata: Extensible key-value attributes
 *
 * Phase 1.2 (Issue #52) additions:
 * - task_id: Optional task identifier for cancellation tracking
 */
struct KeystoneMessage {
  // Core identifiers
  std::string msg_id;       ///< Unique message identifier (UUID)
  std::string sender_id;    ///< ID of the sending agent
  std::string receiver_id;  ///< ID of the receiving agent

  // Phase A: Enhanced fields
  ActionType action_type;                       ///< Type of action requested/performed
  ContentType content_type;                     ///< Format of the payload
  std::string session_id;                       ///< Session/context identifier
  std::map<std::string, std::string> metadata;  ///< Extensible metadata

  // Phase C: Priority field
  Priority priority;  ///< Message priority (HIGH/NORMAL/LOW)

  // Phase C: Deadline scheduling
  std::optional<std::chrono::system_clock::time_point> deadline;  ///< Optional processing deadline

  // Phase 1.2 (Issue #52): Task cancellation
  std::optional<std::string> task_id;  ///< Optional task ID for tracking/cancellation

  // Issue #285: Cross-host tracing
  std::optional<std::string> correlation_id;  ///< Optional correlation ID for distributed tracing

  // Payload and timing
  [[deprecated("command is a legacy/convenience field; use payload with ActionType instead")]]
  std::string command;                 ///< Command string to execute (legacy/convenience)
  std::optional<std::string> payload;  ///< Optional payload data
  std::chrono::system_clock::time_point timestamp;  ///< Message timestamp

  // Declare special members out-of-line so their definitions (in message.cpp)
  // can suppress -Wdeprecated-declarations for the internal move/copy of the
  // deprecated 'command' field.  External code that reads/writes 'command'
  // directly will still receive the deprecation diagnostic.
  KeystoneMessage();
  KeystoneMessage(const KeystoneMessage&);
  KeystoneMessage(KeystoneMessage&&) noexcept;
  KeystoneMessage& operator=(const KeystoneMessage&);
  KeystoneMessage& operator=(KeystoneMessage&&) noexcept;
  ~KeystoneMessage();

  /**
   * @brief Create a new message with generated ID (legacy interface)
   *
   * This maintains backward compatibility with existing code.
   * Uses ActionType::EXECUTE by default.
   *
   * @param sender Sender agent ID
   * @param receiver Receiver agent ID
   * @param cmd Command string
   * @param data Optional payload data
   * @return KeystoneMessage New message with auto-generated ID
   */
  static KeystoneMessage create(const std::string& sender, const std::string& receiver,
                                const std::string& cmd,
                                const std::optional<std::string>& data = std::nullopt);

  /**
   * @brief Create a new enhanced message with all fields
   *
   * @param sender Sender agent ID
   * @param receiver Receiver agent ID
   * @param action Action type
   * @param session Session identifier
   * @param data Optional payload data
   * @param content Content type (default: TEXT_PLAIN)
   * @return KeystoneMessage New message with auto-generated ID
   */
  static KeystoneMessage create(const std::string& sender, const std::string& receiver,
                                ActionType action, const std::string& session,
                                const std::optional<std::string>& data = std::nullopt,
                                ContentType content = ContentType::TEXT_PLAIN);

  /**
   * @brief Set deadline relative to current time
   *
   * @param duration_ms Deadline in milliseconds from now
   */
  void setDeadlineFromNow(std::chrono::milliseconds duration_ms);

  /**
   * @brief Check if message has missed its deadline
   *
   * @return true if deadline exists and has passed, false otherwise
   */
  bool hasDeadlinePassed() const;

  /**
   * @brief Get time remaining until deadline
   *
   * @return milliseconds until deadline, nullopt if no deadline set
   */
  std::optional<std::chrono::milliseconds> getTimeUntilDeadline() const;

  /**
   * @brief Create a task cancellation message
   *
   * Phase 1.2 (Issue #52): Factory method for creating CANCEL_TASK messages.
   * Cancellation is cooperative - agents check for cancellation and respond
   * gracefully.
   *
   * @param sender Sender agent ID (parent requesting cancellation)
   * @param receiver Receiver agent ID (child executing the task)
   * @param task_id Task identifier to cancel
   * @param session Session identifier (default: "default")
   * @return KeystoneMessage Cancellation message with CANCEL_TASK action
   */
  static KeystoneMessage createCancellation(const std::string& sender, const std::string& receiver,
                                            const std::string& task_id,
                                            const std::string& session = "default");
};

/**
 * @brief Response to a KeystoneMessage
 */
struct Response {
  std::string msg_id;       ///< ID of the original message
  std::string sender_id;    ///< ID of the responding agent
  std::string receiver_id;  ///< ID of the original sender
  enum class Status { Success, Error } status;
  std::string result;  ///< Result data or error message
  std::chrono::system_clock::time_point timestamp;

  /**
   * @brief Create a success response
   *
   * @param original_msg The message being responded to
   * @param sender The responding agent ID
   * @param result_data The result data
   * @return Response Success response
   */
  static Response createSuccess(const KeystoneMessage& original_msg, const std::string& sender,
                                const std::string& result_data);

  /**
   * @brief Create an error response
   *
   * @param original_msg The message being responded to
   * @param sender The responding agent ID
   * @param error_msg The error message
   * @return Response Error response
   */
  static Response createError(const KeystoneMessage& original_msg, const std::string& sender,
                              const std::string& error_msg);
};

}  // namespace core
}  // namespace keystone
