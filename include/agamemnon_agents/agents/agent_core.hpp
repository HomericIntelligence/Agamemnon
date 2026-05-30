#pragma once

#include <atomic>
#include <chrono>  // FIX M2: For time-based priority fairness
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

#include "concurrentqueue.h"
#include "core/config.hpp"  // FIX m3: Centralized configuration
#include "core/message.hpp"

namespace keystone {

// Forward declarations from core and concurrency namespaces
// CRITICAL: These declarations must be INSIDE keystone namespace but OUTSIDE
// agents namespace to prevent C++ namespace collision. If placed before
// "namespace keystone {}", the compiler creates "keystone::core::MessageBus"
// but the actual class is defined as "keystone::core::MessageBus", causing a
// doubling bug where both "keystone::core::MessageBus" and
// "keystone::keystone::core::MessageBus" exist in the symbol table. By placing
// them here, we correctly forward-declare the classes in the keystone::core and
// keystone::concurrency namespaces while allowing agent_base.hpp to use them
// without including their headers. FIX ISP (Issue #46): Forward declare
// IMessageRouter instead of MessageBus
namespace core {
class IMessageRouter;
}
namespace concurrency {
class WorkStealingScheduler;
}
namespace agents {

/**
 * @brief Core base class for all agents (sync and async)
 *
 * Provides inbox management and message routing functionality.
 * Subclasses implement either sync or async message processing.
 *
 * Phase C Enhancement: Uses lock-free concurrent queue for inbox
 * to eliminate contention under high message load.
 */
class AgentCore {
 public:
  /**
   * @brief Construct a new Agent Core
   *
   * @param agent_id Unique identifier for this agent
   */
  explicit AgentCore(const std::string& agent_id);

  virtual ~AgentCore() = default;

  /**
   * @brief Send a message via the message bus
   *
   * @param msg Message to send
   * @throws std::runtime_error if message bus not set
   */
  void sendMessage(const core::KeystoneMessage& msg);

  /**
   * @brief Receive a message into the inbox (called by MessageBus)
   *
   * Lock-free operation - no mutex contention.
   *
   * @param msg Message to receive
   */
  virtual void receiveMessage(const core::KeystoneMessage& msg);

  /**
   * @brief Get the next message from inbox
   *
   * Lock-free operation - no mutex contention.
   *
   * @return std::optional<core::KeystoneMessage> Next message if available
   */
  std::optional<core::KeystoneMessage> getMessage();

  /**
   * @brief Get the agent ID
   *
   * @return const std::string& Agent identifier
   */
  const std::string& getAgentId() const { return agent_id_; }

  /**
   * @brief Set the message router for this agent
   *
   * FIX ISP (Issue #46): Changed from MessageBus* to IMessageRouter*
   * Agents only need routing capability, not full registry/scheduler access.
   *
   * @param router Pointer to message router (must outlive agent)
   */
  void setMessageBus(core::IMessageRouter* router);

  /**
   * @brief Set the work-stealing scheduler for this agent
   *
   * When a scheduler is stored on the agent, AsyncAgent::receiveMessage will
   * submit processMessage() directly to the scheduler rather than queuing to
   * the inbox (auto-processing / push model). Without a stored scheduler, the
   * pull model is used: messages go to the inbox and callers must call
   * getMessage() + processMessage() manually.
   *
   * @param scheduler Pointer to scheduler (must outlive agent), or nullptr to
   * disable
   */
  void setScheduler(concurrency::WorkStealingScheduler* scheduler) {
    scheduler_.store(scheduler, std::memory_order_release);
  }

  /**
   * @brief Get the stored scheduler for this agent
   *
   * @return WorkStealingScheduler* or nullptr
   */
  concurrency::WorkStealingScheduler* getScheduler() const {
    return scheduler_.load(std::memory_order_acquire);
  }

  /**
   * @brief Configure the low-priority message check interval
   *
   * Issue #23: Per-agent configurable fairness check interval.
   *
   * Controls how frequently this agent force-checks NORMAL/LOW priority queues
   * to prevent starvation under sustained HIGH priority load. Different agents
   * may have different latency requirements:
   * - Latency-sensitive: use shorter intervals (e.g., 10ms-50ms)
   * - Throughput-optimized: use longer intervals (e.g., 200ms-500ms)
   *
   * @param interval Check interval duration (default: 100ms from Config)
   *
   * Example:
   * @code
   * // Low-latency agent (interactive UI updates)
   * agent->setLowPriorityCheckInterval(std::chrono::milliseconds{10});
   *
   * // High-throughput batch processor
   * agent->setLowPriorityCheckInterval(std::chrono::milliseconds{500});
   * @endcode
   */
  void setLowPriorityCheckInterval(std::chrono::milliseconds interval) {
    // FIX SAFE-002: Use memory_order_release to ensure visibility to other
    // threads This ensures getMessage() threads see the updated interval value
    low_priority_check_interval_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count(),
        std::memory_order_release);
  }

  /**
   * @brief Get the current low-priority check interval
   *
   * @return std::chrono::milliseconds Current check interval
   */
  std::chrono::milliseconds getLowPriorityCheckInterval() const {
    // FIX SAFE-002: Use memory_order_acquire to see writes from
    // setLowPriorityCheckInterval
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds{low_priority_check_interval_ns_.load(std::memory_order_acquire)});
  }

  /**
   * @brief Request cancellation of a task
   *
   * Phase 1.2 (Issue #52): Mark a task for cancellation.
   * This is cooperative cancellation - the agent checks and responds.
   *
   * Thread-safe: Uses atomic operations for cancellation tracking.
   *
   * @param task_id Task identifier to cancel
   */
  void requestCancellation(const std::string& task_id);

  /**
   * @brief Check if a task has been cancelled
   *
   * Phase 1.2 (Issue #52): Agents should check this periodically during
   * long-running operations to respond to cancellation requests.
   *
   * Thread-safe: Uses atomic operations.
   *
   * @param task_id Task identifier to check
   * @return true if task has been cancelled, false otherwise
   */
  bool isCancelled(const std::string& task_id) const;

  /**
   * @brief Clear cancellation status for a task
   *
   * Phase 1.2 (Issue #52): Called when task completes or after
   * acknowledging cancellation.
   *
   * @param task_id Task identifier to clear
   */
  void clearCancellation(const std::string& task_id);

 protected:
  /**
   * @brief Update queue depth metrics
   *
   * Calculates total messages across all priority queues and reports to
   * Metrics. Called after each getMessage() operation.
   */
  void updateQueueDepthMetrics();
  std::string agent_id_;
  // FIX ISP (Issue #46): Use IMessageRouter* instead of MessageBus*
  core::IMessageRouter* message_bus_{nullptr};
  // Stored scheduler for auto-processing (AsyncAgent uses this in
  // receiveMessage)
  std::atomic<concurrency::WorkStealingScheduler*> scheduler_{nullptr};

  // Phase C: Priority-based lock-free inboxes
  // Messages are routed to queues by priority and processed HIGH -> NORMAL ->
  // LOW
  moodycamel::ConcurrentQueue<core::KeystoneMessage> high_priority_inbox_;
  moodycamel::ConcurrentQueue<core::KeystoneMessage> normal_priority_inbox_;
  moodycamel::ConcurrentQueue<core::KeystoneMessage> low_priority_inbox_;

  // FIX C1: Anti-starvation using time-based fairness (not count-based)
  // Force-check lower priorities every N milliseconds to prevent starvation
  // under sustained HIGH priority load
  // THREAD-SAFE: Using atomic to prevent data races from concurrent
  // getMessage() calls
  std::atomic<int64_t> last_low_priority_check_ns_;

  // Issue #23: Per-agent configurable fairness interval (defaults to Config
  // value) THREAD-SAFE: Atomic for lock-free read/write from concurrent
  // getMessage() and setter
  std::atomic<int64_t> low_priority_check_interval_ns_;

  // FIX M1: Backpressure - Queue size limits to prevent memory exhaustion
  std::atomic<bool> backpressure_applied_{false};  ///< Flag when backpressure is active

  // Phase 1.2 (Issue #52): Cancellation tracking
  // Thread-safe: Using mutex to protect set operations
  mutable std::mutex cancellation_mutex_;
  std::unordered_set<std::string> cancelled_tasks_;
};

}  // namespace agents
}  // namespace keystone
