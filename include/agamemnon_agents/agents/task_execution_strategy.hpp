#pragma once

#include <memory>
#include <string>

#include "agents/message_processing_strategy.hpp"
#include "concurrency/task.hpp"
#include "core/message.hpp"

namespace keystone {
namespace agents {

/**
 * @brief Concrete strategy for executing bash commands (TaskAgent domain logic)
 *
 * ARCH-007: Extracted from TaskAgent to separate domain logic
 * (bash command execution) from agent infrastructure.
 *
 * Responsibilities:
 * - Validate bash commands against security whitelist
 * - Execute commands using RAII PipeHandle
 * - Sanitize command output for error messages
 * - Return execution results or errors
 *
 * This strategy can be tested independently without TaskAgent infrastructure.
 */
class TaskExecutionStrategy : public MessageProcessingStrategy {
 public:
  TaskExecutionStrategy() = default;
  ~TaskExecutionStrategy() override = default;

  /**
   * @brief Process a bash command execution request
   *
   * Domain logic:
   * 1. Extract command from message payload
   * 2. Validate command against allowed patterns:
   *    - Arithmetic: echo $((expression))
   *    - Simple echo: echo <text>
   *    - Whitelisted commands: bc, find, grep, etc.
   * 3. Execute command using popen (with RAII PipeHandle)
   * 4. Capture stdout and sanitize
   * 5. Return success/error response
   *
   * @param msg Message containing bash command in payload
   * @return Task<Response> Result of command execution
   */
  concurrency::Task<core::Response> process(const core::KeystoneMessage& msg) override;

 private:
  /**
   * @brief Validate command against security whitelist
   *
   * @param cmd Command to validate
   * @return true if command is allowed
   */
  bool isCommandAllowed(const std::string& cmd) const;

  /**
   * @brief Execute bash command and capture output
   *
   * Uses RAII PipeHandle to ensure pclose() is called even on exceptions.
   *
   * @param cmd Command to execute
   * @return std::string Command output or error message
   */
  std::string executeBashCommand(const std::string& cmd) const;

  /**
   * @brief RAII wrapper for FILE* from popen/pclose
   *
   * Ensures pclose() is called automatically when PipeHandle goes out of scope.
   */
  struct PipeHandle {
    FILE* fp;
    explicit PipeHandle(FILE* f) : fp(f) {}
    ~PipeHandle() {
      if (fp) pclose(fp);
    }
    PipeHandle(const PipeHandle&) = delete;
    PipeHandle& operator=(const PipeHandle&) = delete;
    PipeHandle(PipeHandle&& other) noexcept : fp(other.fp) { other.fp = nullptr; }
    PipeHandle& operator=(PipeHandle&& other) noexcept {
      if (this != &other) {
        if (fp) pclose(fp);
        fp = other.fp;
        other.fp = nullptr;
      }
      return *this;
    }
  };
};

}  // namespace agents
}  // namespace keystone
