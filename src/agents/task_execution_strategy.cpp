#include "agents/task_execution_strategy.hpp"

#include <array>
#include <cstdint>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "core/error_sanitizer.hpp"

namespace keystone {
namespace agents {

// Whitelisted safe commands (same as TaskAgent)
static const std::unordered_set<std::string> ALLOWED_COMMANDS = {
    "date",    // Show current date/time
    "whoami",  // Show current user
    "pwd",     // Print working directory
    "uname",   // Show system information
    "ls",      // List files
    "cat",     // Display file contents
    "head",    // Show first lines
    "tail",    // Show last lines
    "wc",      // Word count
    "echo",    // Print text
    "grep",    // Search text
    "find",    // Find files
    "bc"       // Calculator
};

concurrency::Task<core::Response> TaskExecutionStrategy::process(const core::KeystoneMessage& msg) {
  try {
    // Execute the bash command
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
        std::string result = executeBashCommand(msg.command);
    _Pragma("GCC diagnostic pop")

        // Create success response
        auto response = core::Response::createSuccess(msg, "strategy", result);
    co_return response;

  } catch (const std::exception& e) {
    // Sanitize error message to prevent information disclosure
    std::string sanitized_error = core::sanitizeErrorMessage(e.what());

    // Create error response with sanitized message
    auto response = core::Response::createError(msg, "strategy", sanitized_error);
    co_return response;
  }
}

bool TaskExecutionStrategy::isCommandAllowed(const std::string& command) const {
  // FIX P0-001: Security validation with pattern matching for safe operations

  // 1. Check for empty command
  if (command.empty()) {
    return false;
  }

  // 2. PATTERN MATCHING: Allow specific safe command patterns

  // Pattern 1: Shell arithmetic - echo $((arithmetic expression))
  std::regex arithmetic_pattern(R"(^echo \$\(\([-+*/0-9 ()]+\)\)$)");
  if (std::regex_match(command, arithmetic_pattern)) {
    return true;  // SAFE: Arithmetic operation
  }

  // Pattern 2: Simple echo with safe characters
  std::regex simple_echo_pattern(R"(^echo [-a-zA-Z0-9 .,!?'"]+$)");
  if (std::regex_match(command, simple_echo_pattern)) {
    return true;  // SAFE: Simple text echo
  }

  // Pattern 3: Whitelisted commands with safe arguments
  std::istringstream iss(command);
  std::string base_command;
  iss >> base_command;

  if (ALLOWED_COMMANDS.find(base_command) != ALLOWED_COMMANDS.end()) {
    // Check for dangerous characters in arguments
    std::string args = command.substr(base_command.length());
    const std::string very_dangerous = ";|&`$<>!{}[]";
    if (args.find_first_of(very_dangerous) == std::string::npos &&
        args.find("..") == std::string::npos) {  // No directory traversal
      return true;                               // SAFE: Whitelisted command with safe arguments
    }
  }

  return false;  // DEFAULT: REJECT
}

std::string TaskExecutionStrategy::executeBashCommand(const std::string& cmd) const {
  // Validate command BEFORE execution
  if (!isCommandAllowed(cmd)) {
    std::stringstream ss;
    ss << "Command does not match any allowed patterns.\n";
    ss << "Allowed patterns:\n";
    ss << "  1. Arithmetic: echo $((expression))\n";
    ss << "  2. Simple echo: echo <text>\n";
    ss << "  3. Whitelisted commands: ";
    bool first = true;
    for (const auto& command : ALLOWED_COMMANDS) {
      if (!first) ss << ", ";
      ss << command;
      first = false;
    }
    ss << "\nYour command: " << cmd;
    throw std::runtime_error(ss.str());
  }

  std::array<char, 4096> buffer;
  std::string result;

  // RAII pipe handle - automatically closes on exception or return
  PipeHandle pipe(popen(cmd.c_str(), "r"));
  if (!pipe.fp) {
    throw std::runtime_error("popen() failed");
  }

  // Read command output
  while (fgets(buffer.data(), buffer.size(), pipe.fp) != nullptr) {
    result += buffer.data();
  }

  // Get exit status
  int32_t status = pclose(pipe.fp);
  pipe.fp = nullptr;  // Mark as closed

  if (status != 0) {
    std::stringstream ss;
    ss << "Command exited with status " << status;
    throw std::runtime_error(ss.str());
  }

  // Remove trailing newline if present
  if (!result.empty() && result.back() == '\n') {
    result.pop_back();
  }

  return result;
}

}  // namespace agents
}  // namespace keystone
