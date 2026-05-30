#pragma once

#include <memory>
#include <string>

#include "agent_core.hpp"
#include "concurrency/task.hpp"
#include "concurrency/work_stealing_scheduler.hpp"
#include "core/message.hpp"

namespace keystone {
namespace agents {

/**
 * @brief Asynchronous agent base class (unified)
 *
 * FIX C3: Single base class for all agents (async by default).
 * All agents inherit from this class and implement async processMessage().
 *
 * This replaces the dual BaseAgent (sync) / AsyncBaseAgent (async) hierarchy
 * with a single async-by-default base class, enabling polymorphic collections
 * and runtime execution model flexibility.
 */
class AsyncAgent : public AgentCore, public std::enable_shared_from_this<AsyncAgent> {
 public:
  /**
   * @brief Construct a new Async Agent
   *
   * @param agent_id Unique identifier for this agent
   */
  explicit AsyncAgent(const std::string& agent_id);

  ~AsyncAgent() override = default;

  /**
   * @brief Process an incoming message asynchronously
   *
   * FIX C3: Changed from Response to Task<Response> to make async default.
   * All concrete agents must implement this method and use coroutines.
   *
   * @param msg The message to process
   * @return concurrency::Task<core::Response> Async task that resolves to
   * response
   */
  virtual concurrency::Task<core::Response> processMessage(const core::KeystoneMessage& msg) = 0;

  /**
   * @brief Receive a message — auto-processes via scheduler when one is stored
   *
   * When setScheduler() has been called with a non-null scheduler, submits
   * processMessage() to the scheduler immediately (push/auto-processing model).
   * Without a stored scheduler, falls back to AgentCore inbox (pull model).
   *
   * @param msg Message to receive
   */
  void receiveMessage(const core::KeystoneMessage& msg) override;

 protected:
  /**
   * @brief Handle a cancellation request message
   *
   * Phase 1.2 (Issue #52): Helper method for processing CANCEL_TASK messages.
   * Agents should call this in their processMessage() implementation when
   * receiving CANCEL_TASK action type.
   *
   * @param msg The cancellation message
   * @return core::Response Acknowledgement response
   */
  core::Response handleCancellation(const core::KeystoneMessage& msg);
};

}  // namespace agents
}  // namespace keystone
