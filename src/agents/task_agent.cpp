#include "agents/task_agent.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include "concurrency/logger.hpp"
#include "core/error_sanitizer.hpp"
#include "core/metrics.hpp"

#ifdef ENABLE_GRPC
#include "hmas_coordinator.pb.h"
#endif

namespace keystone {
namespace agents {

// FIX P1-03: Whitelist of allowed commands to prevent command injection
// This is a security-critical list - only safe, non-destructive commands
// allowed
static const std::unordered_set<std::string> ALLOWED_COMMANDS = {
    "echo",    // Print text
    "cat",     // Display file contents
    "ls",      // List directory
    "pwd",     // Print working directory
    "date",    // Show date/time
    "whoami",  // Show current user
    "uname",   // Show system info
    "head",    // Show first lines of file
    "tail",    // Show last lines of file
    "wc",      // Word count
    "grep",    // Search text
    "find",    // Find files
    "bc"       // Calculator (for test arithmetic)
};

TaskAgent::TaskAgent(const std::string& agent_id) : AsyncAgent(agent_id) {}

concurrency::Task<core::Response> TaskAgent::processMessage(const core::KeystoneMessage& msg) {
  // FIX C3: Changed to async (returns Task<Response>)
  // Record message processed for metrics tracking
  core::Metrics::getInstance().recordMessageProcessed(msg.msg_id);

  // Issue #376: Reject immediately if agent is in failed state
  if (isFailed()) {
    auto response = core::Response::createError(msg, agent_id_,
                                                "Agent is in failed state: " + getFailureReason());
    sendResponseMessage(msg, response.result);
    co_return response;
  }

  // Phase 1.2 (Issue #52): Handle CANCEL_TASK action type
  if (msg.action_type == core::ActionType::CANCEL_TASK) {
    auto response = handleCancellation(msg);
    sendResponseMessage(msg, response.result);
    co_return response;
  }

  // Check if deadline was missed
  if (msg.deadline.has_value() && msg.hasDeadlinePassed()) {
    auto time_remaining = msg.getTimeUntilDeadline();
    (void)time_remaining;  // Suppress unused warning - reserved for future use
    // Deadline passed - time_remaining will be 0, we want the absolute value
    // For now, record as 0ms late (we don't track when it was supposed to be
    // processed)
    core::Metrics::getInstance().recordDeadlineMiss(msg.msg_id, 0);
  }

  try {
    // Execute the bash command
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
        std::string result = executeBash(msg.command);

    // Log the execution (lock guards concurrent processMessage coroutines)
    {
      std::lock_guard<std::mutex> lock(log_mutex_);
      command_log_.emplace_back(msg.command, result);
    }
    _Pragma("GCC diagnostic pop")

        auto response = core::Response::createSuccess(msg, agent_id_, result);
    sendResponseMessage(msg, result);
    co_return response;

  } catch (const std::exception& e) {
    // FIX P3-08: Sanitize error message to prevent information disclosure
    std::string sanitized_error = core::sanitizeErrorMessage(e.what());
    auto response = core::Response::createError(msg, agent_id_, sanitized_error);

    // Issue #87: Send TASK_FAILED so parent Lead Agent can unblock the DAG
    auto failure_msg = core::KeystoneMessage::create(agent_id_, msg.sender_id, "response",
                                                     std::string("ERROR: ") + sanitized_error);
    failure_msg.action_type = core::ActionType::TASK_FAILED;
    failure_msg.msg_id = msg.msg_id;

    sendMessage(failure_msg);

    co_return response;
  }
}

void TaskAgent::sendResponseMessage(const core::KeystoneMessage& msg, const std::string& payload) {
  auto response_msg = core::KeystoneMessage::create(agent_id_, msg.sender_id, "response", payload);
  response_msg.msg_id = msg.msg_id;
  sendMessage(response_msg);
}

void TaskAgent::validateCommand(const std::string& command) {
  // FIX P0-001: Security validation with pattern matching for safe operations
  // Strategy: Whitelist specific safe patterns, reject everything else

  // 1. Check for empty command
  if (command.empty()) {
    throw std::runtime_error("Command cannot be empty");
  }

  // 2. PATTERN MATCHING: Allow specific safe command patterns

  // FIX: Compile regex patterns once (static) for performance
  // Pattern 1: Shell arithmetic - echo $((arithmetic expression))
  // Regex: ^echo \$\(\([-+*/0-9 ()]+\)\)$
  // Allows: echo $((5 + 3)), echo $((91 + 13)), echo $((10 * (5 + 2)))
  static const std::regex arithmetic_pattern(R"(^echo \$\(\([-+*/0-9 ()]+\)\)$)");
  if (std::regex_match(command, arithmetic_pattern)) {
    // Validate arithmetic expression doesn't contain command substitution
    // The regex already ensures only digits, operators, spaces, and parens
    return;  // SAFE: Arithmetic operation
  }

  // Pattern 2: Simple echo with safe characters (alphanumeric, spaces, basic
  // punctuation) Regex: ^echo [-a-zA-Z0-9 .,!?'"]+$
  static const std::regex simple_echo_pattern(R"(^echo [-a-zA-Z0-9 .,!?'"]+$)");
  if (std::regex_match(command, simple_echo_pattern)) {
    return;  // SAFE: Simple text echo
  }

  // Pattern 3: sleep with max duration (prevent DoS via long sleeps)
  static const std::regex sleep_pattern(R"(^sleep ([0-9]+(?:\.[0-9]+)?)$)");
  std::smatch sleep_match;
  if (std::regex_match(command, sleep_match, sleep_pattern)) {
    double duration = std::stod(sleep_match[1].str());
    if (duration > 10.0) {
      throw std::runtime_error("sleep duration exceeds maximum allowed (10s)");
    }
    return;  // SAFE: short sleep
  }

  // Pattern 4: Whitelisted commands with no arguments
  std::istringstream iss(command);
  std::string base_command;
  iss >> base_command;

  // Check if it's a standalone whitelisted command (no dangerous args)
  if (ALLOWED_COMMANDS.find(base_command) != ALLOWED_COMMANDS.end()) {
    // For whitelisted commands, check for dangerous characters in arguments
    std::string args = command.substr(base_command.length());

    // Allow whitelisted command with safe arguments
    const std::string very_dangerous = ";|&`$<>!{}[]";  // Command injection chars
    if (args.find_first_of(very_dangerous) == std::string::npos &&
        args.find("..") == std::string::npos) {  // No directory traversal
      return;                                    // SAFE: Whitelisted command with safe arguments
    }
  }

  // 3. DEFAULT: REJECT - Command doesn't match any safe pattern
  std::stringstream ss;
  ss << "Command does not match any allowed patterns.\n";
  ss << "Allowed patterns:\n";
  ss << "  1. Arithmetic: echo $((expression))\n";
  ss << "  2. Simple echo: echo <text>\n";
  ss << "  3. sleep <N> where N <= 10\n";
  ss << "  4. Whitelisted commands: ";
  bool first = true;
  for (const auto& cmd : ALLOWED_COMMANDS) {
    if (!first) ss << ", ";
    ss << cmd;
    first = false;
  }
  ss << "\nYour command: " << command;
  throw std::runtime_error(ss.str());
}

std::string TaskAgent::executeBash(const std::string& command) {
  // FIX P1-03: Validate command BEFORE execution to prevent injection
  validateCommand(command);

  std::array<char, 4096> buffer;  // Increased from 128 to handle larger outputs
  std::string result;

  // Use explicit bash shell to support arithmetic expansion $((...))
  // popen() default uses /bin/sh which may not support bash-specific syntax
  //
  // Security note: We escape the command by replacing " with \" to prevent
  // injection However, validateCommand() already ensures only safe patterns are
  // allowed
  std::string escaped_command = command;
  size_t pos = 0;
  while ((pos = escaped_command.find('"', pos)) != std::string::npos) {
    escaped_command.replace(pos, 1, "\\\"");
    pos += 2;  // Move past the escaped quote
  }

  std::string bash_command = "/bin/bash -c \"" + escaped_command + "\"";

  // RAII pipe handle - automatically closes on exception or return
  PipeHandle pipe(popen(bash_command.c_str(), "r"));
  if (!pipe) {
    throw std::runtime_error("popen() failed");
  }

  // Read command output
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }

  // Manual close to get status (release() transfers ownership)
  int32_t status = pclose(pipe.release());
  if (status != 0) {
    std::stringstream ss;
    ss << "Command exited with status " << status;
    throw std::runtime_error(ss.str());
  }

  // Remove trailing whitespace (newlines, spaces, tabs, carriage returns)
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
    result.pop_back();
  }

  return result;
}

#ifdef ENABLE_GRPC
void TaskAgent::initializeGrpc(const std::string& coordinator_address,
                               const std::string& registry_address, const std::string& agent_type,
                               uint8_t level) {
  agent_type_ = agent_type;
  agent_level_ = level;

  // Create gRPC clients
  network::GrpcClientConfig coordinator_config;
  coordinator_config.server_address = coordinator_address;
  coordinator_client_ = std::make_unique<network::HMASCoordinatorClient>(coordinator_config);

  network::GrpcClientConfig registry_config;
  registry_config.server_address = registry_address;
  registry_client_ = std::make_unique<network::ServiceRegistryClient>(registry_config);

  // Register with ServiceRegistry
  hmas::AgentRegistration registration;
  registration.set_agent_id(agent_id_);
  registration.set_agent_type(agent_type_);
  registration.set_level(level);
  registration.add_capabilities("bash_execution");
  registration.set_max_concurrent_tasks(1);

  try {
    auto response = registry_client_->registerAgent(registration);
    if (response.success()) {
      // Start heartbeat thread
      startHeartbeat();
    } else {
      throw std::runtime_error("Failed to register with ServiceRegistry: " + response.message());
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("gRPC registration failed: " + std::string(e.what()));
  }
}

void TaskAgent::processYamlTask(const std::string& yaml_spec) {
  // Parse YAML task specification
  auto spec_opt = network::YamlParser::parseTaskSpec(yaml_spec);
  if (!spec_opt) {
    throw std::runtime_error("Failed to parse YAML task specification");
  }

  auto spec = *spec_opt;

  // Execute bash command from payload
  std::string result;
  try {
    result = executeBash(spec.payload.command);
    spec.status.phase = "COMPLETED";
    spec.status.result = result;
  } catch (const std::exception& e) {
    spec.status.phase = "FAILED";
    spec.status.error = e.what();
    result = std::string("ERROR: ") + e.what();
  }

  // Update status timestamps
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  char time_buf[100];
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t_now));
  spec.status.completion_time = std::string(time_buf);

  if (!spec.status.start_time) {
    spec.status.start_time = std::string(time_buf);
  }

  // Generate result YAML
  std::string result_yaml = network::YamlParser::generateTaskSpec(spec);

  // Submit result back to parent via gRPC
  if (coordinator_client_ && spec.metadata.parent_task_id) {
    hmas::TaskResult task_result;
    task_result.set_task_id(spec.metadata.task_id);
    task_result.set_result_yaml(result_yaml);
    task_result.set_success(spec.status.phase == "COMPLETED");
    if (spec.status.error) {
      task_result.set_error_message(*spec.status.error);
    }

    try {
      coordinator_client_->submitResult(task_result);
    } catch (const std::exception& e) {
      // Log error but don't fail the task
      concurrency::Logger::error("Failed to submit result via gRPC: {}", e.what());
    }
  }
}

void TaskAgent::startHeartbeat() {
  if (heartbeat_running_) {
    return;  // Already running
  }

  heartbeat_running_ = true;
  heartbeat_thread_ = std::thread(&TaskAgent::heartbeatLoop, this);
}

void TaskAgent::stopHeartbeat() {
  if (!heartbeat_running_) {
    return;
  }

  heartbeat_running_ = false;
  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }
}

void TaskAgent::heartbeatLoop() {
  while (heartbeat_running_) {
    try {
      if (registry_client_) {
        registry_client_->heartbeat(agent_id_, 0.0f, 0.0f, 0);
      }
    } catch (const std::exception& e) {
      // Log error but continue heartbeating
      concurrency::Logger::error("Heartbeat failed: {}", e.what());
    }

    // Sleep for 1 second
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void TaskAgent::shutdown() {
  // Stop heartbeat
  stopHeartbeat();

  // Unregister from ServiceRegistry
  if (registry_client_) {
    try {
      registry_client_->unregisterAgent(agent_id_, "Shutdown requested");
    } catch (const std::exception& e) {
      concurrency::Logger::error("Failed to unregister agent: {}", e.what());
    }
  }

  // Clear gRPC clients
  coordinator_client_.reset();
  registry_client_.reset();
}
#endif

}  // namespace agents
}  // namespace keystone
