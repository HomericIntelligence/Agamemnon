#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agents/async_agent.hpp"
#include "core/failure_injector.hpp"

#ifdef ENABLE_GRPC
#include "network/grpc_client.hpp"
#include "network/yaml_parser.hpp"
#endif

namespace keystone {
namespace agents {

/**
 * @brief Level 3 Task Agent
 *
 * Executes concrete tasks including bash commands.
 * Returns results to the commanding agent.
 */
class TaskAgent : public AsyncAgent {
 public:
  /**
   * @brief Construct a new Task Agent
   *
   * @param agent_id Unique identifier for this agent
   */
  explicit TaskAgent(const std::string& agent_id);

  /**
   * @brief Process incoming command messages asynchronously
   *
   * FIX C3: Changed to async (returns Task<Response>)
   *
   * @param msg Message containing command to execute
   * @return concurrency::Task<core::Response> Async task with execution result
   */
  concurrency::Task<core::Response> processMessage(const core::KeystoneMessage& msg) override;

  /**
   * @brief Get a snapshot of command execution history
   *
   * @return std::vector<std::pair<std::string, std::string>> Copy of history
   */
  std::vector<std::pair<std::string, std::string>> getCommandHistory() const {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return command_log_;
  }

  // =========================================================================
  // Phase 5: Chaos Engineering — Failure State API
  // =========================================================================

  bool isFailed() const { return failed_.load(std::memory_order_acquire); }

  std::string getFailureReason() const {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    return failure_reason_;
  }

  void markAsFailed(const std::string& reason) {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    failure_reason_ = reason;
    failed_.store(true, std::memory_order_release);
  }

  void recover() {
    std::lock_guard<std::mutex> lock(failure_mutex_);
    failure_reason_.clear();
    failed_.store(false, std::memory_order_release);
    failure_injector_ = nullptr;
  }

  void setFailureInjector(core::FailureInjector* injector) { failure_injector_ = injector; }

  bool shouldFail() {
    if (failure_injector_ == nullptr) {
      return false;
    }
    return failure_injector_->shouldAgentFail(agent_id_);
  }

#ifdef ENABLE_GRPC
  /**
   * @brief Initialize gRPC clients and register with ServiceRegistry
   *
   * @param coordinator_address HMASCoordinator server address
   * @param registry_address ServiceRegistry server address
   * @param agent_type Agent type (default: "TaskAgent")
   * @param level Agent level (default: 3)
   */
  void initializeGrpc(const std::string& coordinator_address, const std::string& registry_address,
                      const std::string& agent_type = "TaskAgent", uint8_t level = 3);

  /**
   * @brief Process incoming YAML task specification
   *
   * @param yaml_spec YAML task specification
   */
  void processYamlTask(const std::string& yaml_spec);

  /**
   * @brief Start heartbeat thread (sends heartbeat every 1s)
   */
  void startHeartbeat();

  /**
   * @brief Stop heartbeat thread
   */
  void stopHeartbeat();

  /**
   * @brief Shutdown agent and unregister from ServiceRegistry
   */
  void shutdown();
#endif

 private:
  /**
   * @brief RAII wrapper for popen/pclose
   */
  struct PipeDeleter {
    void operator()(FILE* pipe) const {
      if (pipe) {
        pclose(pipe);
      }
    }
  };
  using PipeHandle = std::unique_ptr<FILE, PipeDeleter>;

  /**
   * @brief Build and send a response message back to the original sender
   *
   * @param msg Original incoming message (provides sender_id and msg_id)
   * @param payload Text payload for the response message
   */
  void sendResponseMessage(const core::KeystoneMessage& msg, const std::string& payload);

  /**
   * @brief Validate command for security (FIX P1-03: Command injection
   * prevention)
   *
   * @param command Command to validate
   * @throws std::runtime_error if command contains unsafe characters or is not
   * whitelisted
   */
  void validateCommand(const std::string& command);

  /**
   * @brief Execute a bash command with security validation
   *
   * FIX P1-03: Now validates command before execution to prevent injection
   * attacks.
   *
   * @param command The bash command to execute
   * @return std::string The stdout output
   * @throws std::runtime_error if command fails or is invalid
   */
  std::string executeBash(const std::string& command);

  std::vector<std::pair<std::string, std::string>> command_log_;
  mutable std::mutex log_mutex_;

  // Phase 5: Chaos Engineering failure state
  std::atomic<bool> failed_{false};
  mutable std::mutex failure_mutex_;
  std::string failure_reason_;
  core::FailureInjector* failure_injector_{nullptr};

#ifdef ENABLE_GRPC
  /**
   * @brief Heartbeat loop (runs in separate thread)
   */
  void heartbeatLoop();

  // gRPC clients
  std::unique_ptr<network::HMASCoordinatorClient> coordinator_client_;
  std::unique_ptr<network::ServiceRegistryClient> registry_client_;

  // Heartbeat thread
  std::thread heartbeat_thread_;
  std::atomic<bool> heartbeat_running_{false};

  // Agent metadata
  std::string agent_type_{"TaskAgent"};
  uint8_t agent_level_{3};
#endif
};

}  // namespace agents
}  // namespace keystone
