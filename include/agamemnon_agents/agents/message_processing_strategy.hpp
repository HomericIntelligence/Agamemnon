#pragma once

#include "concurrency/task.hpp"
#include "core/message.hpp"

namespace keystone {
namespace agents {

/**
 * @brief Strategy interface for message processing domain logic
 *
 * ARCH-007: Separates domain logic from agent infrastructure.
 * Agents delegate message processing to strategies, following the
 * Strategy pattern for better testability and separation of concerns.
 *
 * Benefits:
 * - Domain logic isolated from infrastructure (inbox, routing, etc.)
 * - Easier to test domain logic in isolation
 * - Clearer single responsibility: agents manage infrastructure,
 *   strategies implement business logic
 * - Easy to swap processing strategies at runtime
 *
 * Example usage:
 * @code
 * class TaskAgent : public AsyncAgent {
 *   std::unique_ptr<MessageProcessingStrategy> strategy_;
 *
 *   Task<Response> processMessage(const KeystoneMessage& msg) override {
 *     co_return co_await strategy_->process(msg);
 *   }
 * };
 * @endcode
 */
class MessageProcessingStrategy {
 public:
  virtual ~MessageProcessingStrategy() = default;

  /**
   * @brief Process a message and return a response asynchronously
   *
   * Implementations contain the domain-specific logic for processing
   * messages (e.g., bash command execution, task delegation, result
   * synthesis, etc.).
   *
   * @param msg The message to process
   * @return concurrency::Task<core::Response> Async task resolving to response
   */
  virtual concurrency::Task<core::Response> process(const core::KeystoneMessage& msg) = 0;
};

}  // namespace agents
}  // namespace keystone
