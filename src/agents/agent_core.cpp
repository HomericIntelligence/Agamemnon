#include "agents/agent_core.hpp"

#include <stdexcept>

#include "concurrency/logger.hpp"
#include "core/config.hpp"  // FIX m3: Centralized configuration
// FIX ISP (Issue #46): Include IMessageRouter instead of MessageBus
#include "core/i_message_router.hpp"
#include "core/metrics.hpp"

namespace keystone {
namespace agents {

AgentCore::AgentCore(const std::string& agent_id) : agent_id_(agent_id) {
  // FIX C1: Initialize time-based fairness timer (thread-safe atomic)
  // FIX P1-003: Initialize to 0 (sentinel) to skip fairness on first HIGH
  // message This ensures fairness only kicks in BETWEEN messages, not before
  // first message
  last_low_priority_check_ns_.store(0, std::memory_order_relaxed);

  // Issue #23: Initialize per-agent fairness interval to default from Config
  low_priority_check_interval_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            core::Config::AGENT_LOW_PRIORITY_CHECK_INTERVAL)
                                            .count(),
                                        std::memory_order_relaxed);

  // Phase C: Priority queues initialized by default constructors
}

void AgentCore::sendMessage(const core::KeystoneMessage& msg) {
  if (!message_bus_) {
    throw std::runtime_error("Message bus not set for agent: " + agent_id_);
  }

  message_bus_->routeMessage(msg);
}

void AgentCore::receiveMessage(const core::KeystoneMessage& msg) {
  // FIX C4: Backpressure - check queue size limit before accepting message
  // THREAD-SAFE: Separate atomic check from side effects to prevent race
  // FIX P3-01: Note - using size_approx() for performance (lock-free)
  // This is intentionally a soft limit for backpressure. The approximate count
  // may be off by ±10 messages due to concurrent access, which is acceptable
  // for this use case. Backpressure is a protective mechanism, not a hard
  // guarantee.
  size_t total_depth = high_priority_inbox_.size_approx() + normal_priority_inbox_.size_approx() +
                       low_priority_inbox_.size_approx();

  bool should_apply_backpressure = (total_depth >= core::Config::AGENT_MAX_QUEUE_SIZE);
  bool was_backpressure_applied = backpressure_applied_.load(std::memory_order_relaxed);

  if (should_apply_backpressure) {
    // Apply backpressure: reject message to prevent memory exhaustion
    if (!was_backpressure_applied) {
      // Only one thread wins the race to set and log
      bool expected = false;
      if (backpressure_applied_.compare_exchange_strong(expected, true,
                                                        std::memory_order_relaxed)) {
        // Log warning on first occurrence
        concurrency::Logger::warn(
            "[BACKPRESSURE] Agent {} inbox full ({} messages), rejecting new "
            "messages",
            agent_id_, total_depth);
      }
    }

    // For now, drop the message (could also throw exception or send NACK)
    // In production, sender should retry with exponential backoff
    return;
  }

  // Clear backpressure flag if queue is below low watermark (hysteresis)
  size_t low_watermark = static_cast<size_t>(core::Config::AGENT_MAX_QUEUE_SIZE *
                                             core::Config::AGENT_QUEUE_LOW_WATERMARK_PERCENT);
  if (total_depth < low_watermark && was_backpressure_applied) {
    // Only one thread wins the race to clear and log
    bool expected = true;
    if (backpressure_applied_.compare_exchange_strong(expected, false, std::memory_order_relaxed)) {
      concurrency::Logger::info(
          "[BACKPRESSURE] Agent {} inbox recovered ({} messages), accepting "
          "messages again",
          agent_id_, total_depth);
    }
  }

  // Phase C: Route to appropriate priority queue (lock-free)
  switch (msg.priority) {
    case core::Priority::HIGH:
      high_priority_inbox_.enqueue(msg);
      break;
    case core::Priority::NORMAL:
      normal_priority_inbox_.enqueue(msg);
      break;
    case core::Priority::LOW:
      low_priority_inbox_.enqueue(msg);
      break;
    default:
      // FIX: Invalid priority - route to NORMAL queue as fallback
      normal_priority_inbox_.enqueue(msg);
      break;
  }
}

std::optional<core::KeystoneMessage> AgentCore::getMessage() {
  // FIX P1-003: Time-based priority fairness to prevent LOW priority starvation
  // FIX SAFE-002: Fixed race condition with proper memory ordering
  // Issue #23: Now uses per-agent configurable interval instead of global
  // constant Strategy: Track HIGH messages processed; every interval_ns of HIGH
  // processing, force-check NORMAL/LOW queues to ensure fairness THREAD-SAFE:
  // Uses acquire/release memory ordering for synchronization

  core::KeystoneMessage msg;
  auto now = std::chrono::steady_clock::now();
  auto now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

  // FIX SAFE-002: Use acquire ordering to ensure visibility of writes from
  // other threads Old: memory_order_relaxed (no synchronization guarantee) New:
  // memory_order_acquire (ensures writes from setLowPriorityCheckInterval are
  // visible)
  int64_t last_check_ns = last_low_priority_check_ns_.load(std::memory_order_acquire);
  int64_t interval_ns = low_priority_check_interval_ns_.load(std::memory_order_acquire);

  // Try standard priority order first: HIGH -> NORMAL -> LOW
  if (high_priority_inbox_.try_dequeue(msg)) {
    // After processing HIGH message, check if fairness interval elapsed
    // This ensures fairness kicks in BETWEEN messages, not before first message
    // Skip fairness check if last_check_ns == 0 (no HIGH messages processed
    // yet)
    if (last_check_ns != 0 && now_ns - last_check_ns >= interval_ns) {
      // Fairness interval elapsed while processing HIGH messages
      // Try to get NORMAL/LOW message to process instead
      core::KeystoneMessage fairness_msg;

      // Try NORMAL first (priority over LOW during fairness)
      if (normal_priority_inbox_.try_dequeue(fairness_msg)) {
        // Got NORMAL message - put HIGH back and return NORMAL
        high_priority_inbox_.enqueue(msg);
        last_low_priority_check_ns_.store(now_ns, std::memory_order_release);
        updateQueueDepthMetrics();
        return fairness_msg;
      }

      // Then try LOW
      if (low_priority_inbox_.try_dequeue(fairness_msg)) {
        // Got LOW message - put HIGH back and return LOW
        high_priority_inbox_.enqueue(msg);
        last_low_priority_check_ns_.store(now_ns, std::memory_order_release);
        updateQueueDepthMetrics();
        return fairness_msg;
      }

      // FIX SAFE-002: No NORMAL/LOW available - keep the HIGH message we
      // already have Don't re-enqueue and re-dequeue (racy pattern), just fall
      // through
    }

    // Reset fairness timer when processing HIGH message
    // FIX SAFE-002: Use release ordering to ensure this write is visible to
    // other threads
    last_low_priority_check_ns_.store(now_ns, std::memory_order_release);
    updateQueueDepthMetrics();
    return msg;
  }

  if (normal_priority_inbox_.try_dequeue(msg)) {
    last_low_priority_check_ns_.store(now_ns, std::memory_order_release);
    updateQueueDepthMetrics();
    return msg;
  }

  if (low_priority_inbox_.try_dequeue(msg)) {
    last_low_priority_check_ns_.store(now_ns, std::memory_order_release);
    updateQueueDepthMetrics();
    return msg;
  }

  return std::nullopt;
}

void AgentCore::updateQueueDepthMetrics() {
  // FIX: Calculate total queue depth across all priority levels
  size_t total_depth = high_priority_inbox_.size_approx() + normal_priority_inbox_.size_approx() +
                       low_priority_inbox_.size_approx();

  core::Metrics::getInstance().recordQueueDepth(agent_id_, total_depth);
}

// FIX ISP (Issue #46): Changed from MessageBus* to IMessageRouter*
void AgentCore::setMessageBus(core::IMessageRouter* router) { message_bus_ = router; }

// Phase 1.2 (Issue #52): Task cancellation support
void AgentCore::requestCancellation(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_tasks_.insert(task_id);
}

bool AgentCore::isCancelled(const std::string& task_id) const {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  return cancelled_tasks_.find(task_id) != cancelled_tasks_.end();
}

void AgentCore::clearCancellation(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_tasks_.erase(task_id);
}

}  // namespace agents
}  // namespace keystone
