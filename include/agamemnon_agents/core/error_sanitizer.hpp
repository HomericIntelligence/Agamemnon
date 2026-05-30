#pragma once

#include <regex>
#include <string>

namespace keystone {
namespace core {

/**
 * @brief Sanitize error messages to prevent information disclosure
 *
 * FIX P3-08: Removes internal implementation details from error messages
 * before sending to external systems or logs.
 *
 * Removes:
 * - File paths (e.g., /home/user/ProjectKeystone/src/agents/task_agent.cpp:85)
 * - Memory addresses (e.g., 0x7fff5fbff710)
 * - Internal namespaces (e.g., keystone::agents::)
 *
 * @param error_message Raw error message
 * @param production_mode If true, apply aggressive sanitization
 * @return Sanitized error message safe for external consumption
 */
inline std::string sanitizeErrorMessage(const std::string& error_message,
                                        bool production_mode = false) {
  std::string sanitized = error_message;

  // Static locals: compiled once on first call; C++11 guarantees thread-safe
  // initialization, avoiding a data race when multiple threads call this
  // simultaneously (std::regex construction touches locale state).

  // 1. Remove file paths (e.g., /path/to/file.cpp:123)
  static const std::regex path_regex(R"(\/[^\s:]+\.(cpp|hpp|h|cc):?\d*)");
  sanitized = std::regex_replace(sanitized, path_regex, "[internal]");

  // 2. Remove memory addresses (e.g., 0x7fff5fbff710)
  static const std::regex addr_regex(R"(0x[0-9a-fA-F]+)");
  sanitized = std::regex_replace(sanitized, addr_regex, "[address]");

  // 3. Remove C++ namespace paths (e.g., keystone::agents::TaskAgent)
  // Only in production mode to keep debugging info in dev
  if (production_mode) {
    static const std::regex namespace_regex(R"((\w+::)+)");
    sanitized = std::regex_replace(sanitized, namespace_regex, "[class]");
  }

  // 4. Remove "what(): " prefix if present (redundant in responses)
  static const std::regex what_prefix_regex(R"(what\(\):\s*)");
  sanitized = std::regex_replace(sanitized, what_prefix_regex, "");

  // 5. In production, redact specific keywords that might leak info
  if (production_mode) {
    static const std::regex home_regex(R"(/home/\w+)");
    sanitized = std::regex_replace(sanitized, home_regex, "~");

    static const std::regex root_regex(R"(/usr/[^\s]+)");
    sanitized = std::regex_replace(sanitized, root_regex, "[system]");
  }

  return sanitized;
}

/**
 * @brief Create a safe error response with sanitized message
 *
 * Convenience wrapper for creating error responses with automatic sanitization.
 *
 * @param original_error Raw error from exception or internal source
 * @param user_facing_context User-friendly context (e.g., "Command execution
 * failed")
 * @param production_mode Enable aggressive sanitization
 * @return Sanitized error message suitable for external responses
 */
inline std::string createSafeErrorResponse(const std::string& original_error,
                                           const std::string& user_facing_context = "",
                                           bool production_mode = false) {
  std::string sanitized = sanitizeErrorMessage(original_error, production_mode);

  if (!user_facing_context.empty()) {
    return user_facing_context + ": " + sanitized;
  }

  return sanitized;
}

}  // namespace core
}  // namespace keystone
