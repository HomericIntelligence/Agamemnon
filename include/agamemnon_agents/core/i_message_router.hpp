#pragma once

#include "message.hpp"

namespace keystone {
namespace core {

/**
 * @brief Interface for message routing
 *
 * Separates message routing logic from agent registry and scheduler
 * integration, following the Interface Segregation Principle.
 *
 * Responsibilities:
 * - Route messages to appropriate agents by ID
 * - Handle routing failures gracefully
 *
 * This interface enables:
 * - Independent testing of routing logic
 * - Different routing strategies (synchronous, asynchronous, priority-based)
 * - Clearer separation of concerns in messaging code
 * - Hot-path optimization without affecting registry
 */
class IMessageRouter {
 public:
  virtual ~IMessageRouter() = default;

  /**
   * @brief Route a message to the appropriate agent
   *
   * Behavior depends on the implementation:
   * - Synchronous: Direct delivery via agent->receiveMessage()
   * - Asynchronous: Delivery via scheduler->submit()
   * - Priority-based: Queue by priority, deliver in order
   *
   * @param msg Message to route (uses msg.receiver_id for routing)
   * @return true if message was delivered/submitted, false if receiver not
   * found
   */
  virtual bool routeMessage(const KeystoneMessage& msg) = 0;
};

}  // namespace core
}  // namespace keystone
