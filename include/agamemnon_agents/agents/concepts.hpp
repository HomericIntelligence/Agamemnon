#pragma once

#include <concepts>
#include <memory>
#include <string>

#include "concurrency/task.hpp"
#include "core/message.hpp"

// Forward declarations to avoid circular dependencies
namespace keystone {
namespace core {
// FIX ISP (Issue #46): Forward declare IMessageRouter instead of MessageBus
class IMessageRouter;
}  // namespace core
namespace concurrency {
class WorkStealingScheduler;
}
}  // namespace keystone

namespace keystone {
namespace agents {

/**
 * @brief Concept for entities with unique identifiers
 *
 * Requires:
 * - getAgentId() method returning string or convertible to string
 *
 * This is a foundational concept for all identifiable entities in the system.
 */
template <typename T>
concept Identifiable = requires(const T& entity) {
  { entity.getAgentId() } -> std::convertible_to<std::string>;
};

/**
 * @brief Concept for entities that can send messages
 *
 * Requires:
 * - sendMessage(KeystoneMessage) method
 *
 * This concept captures the ability to send messages via the message bus.
 */
template <typename T>
concept MessageSender = requires(T& sender, const core::KeystoneMessage& msg) {
  { sender.sendMessage(msg) } -> std::same_as<void>;
};

/**
 * @brief Concept for entities that can receive messages
 *
 * Requires:
 * - receiveMessage(KeystoneMessage) method
 *
 * This concept captures the ability to receive messages into an inbox.
 */
template <typename T>
concept MessageReceiver = requires(T& receiver, const core::KeystoneMessage& msg) {
  { receiver.receiveMessage(msg) } -> std::same_as<void>;
};

/**
 * @brief Concept for asynchronous message handlers
 *
 * Requires:
 * - processMessage(KeystoneMessage) method returning Task<Response>
 *
 * This is the core processing interface for async agents using C++20
 * coroutines.
 */
template <typename T>
concept AsyncMessageHandler = requires(T& handler, const core::KeystoneMessage& msg) {
  { handler.processMessage(msg) } -> std::same_as<concurrency::Task<core::Response>>;
};

/**
 * @brief Complete agent concept (async version)
 *
 * Combines all required agent capabilities:
 * - Identity (unique agent ID)
 * - Message sending (via message bus)
 * - Message receiving (into inbox)
 * - Async message processing (coroutine-based)
 *
 * This concept can be used in templates to ensure compile-time verification
 * that types implement the full agent interface.
 *
 * Example usage:
 * @code
 * template<Agent A>
 * void registerAgent(MessageBus& bus, std::shared_ptr<A> agent) {
 *     bus.registerAgent(agent->getAgentId(), agent);
 * }
 * @endcode
 *
 * Benefits:
 * - Compile-time interface verification
 * - Clear error messages when interface is incomplete
 * - Self-documenting code (concept is the contract)
 * - Enables generic agent algorithms
 */
template <typename T>
concept Agent = Identifiable<T> && MessageSender<T> && MessageReceiver<T> && AsyncMessageHandler<T>;

/**
 * @brief Concept for agents that support scheduler integration
 *
 * Requires:
 * - setScheduler(WorkStealingScheduler*) method
 *
 * This concept is for agents that can be assigned a work-stealing scheduler
 * for concurrent task execution.
 */
template <typename T>
concept SchedulerAware =
    requires(T& agent, keystone::concurrency::WorkStealingScheduler* scheduler) {
      { agent.setScheduler(scheduler) } -> std::same_as<void>;
    };

/**
 * @brief Concept for agents that support message routing integration
 *
 * FIX ISP (Issue #46): Changed from MessageBus* to IMessageRouter*
 * Agents only need routing capability, not full registry/scheduler access.
 *
 * Requires:
 * - setMessageBus(IMessageRouter*) method
 *
 * This concept is for agents that can be connected to a message router.
 */
template <typename T>
concept MessageBusAware = requires(T& agent, keystone::core::IMessageRouter* router) {
  { agent.setMessageBus(router) } -> std::same_as<void>;
};

/**
 * @brief Complete concept for fully-integrated agents
 *
 * Combines:
 * - Core agent capabilities (Agent)
 * - Scheduler integration (SchedulerAware)
 * - Message bus integration (MessageBusAware)
 *
 * This is the most comprehensive agent concept, suitable for use in
 * generic code that requires full agent functionality.
 */
template <typename T>
concept IntegratedAgent = Agent<T> && SchedulerAware<T> && MessageBusAware<T>;

/**
 * @brief Concept for shared pointer to an agent
 *
 * This is a convenience concept for code that works with shared_ptr<Agent>.
 * Used by MessageBus and other components that require shared ownership.
 */
template <typename T>
concept AgentPtr = requires(T ptr) {
  requires std::same_as<T, std::shared_ptr<typename T::element_type>>;
  requires Agent<typename T::element_type>;
};

}  // namespace agents
}  // namespace keystone
