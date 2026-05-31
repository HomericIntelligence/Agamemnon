/**
 * @file work_stealing_queue.cpp
 * @brief Implementation of WorkStealingQueue using moodycamel::ConcurrentQueue.
 *
 * Uses the architecture-mandated concurrentqueue (MPMC lock-free) as the
 * backing store. pop() and steal() both dequeue from the front; naming
 * reflects work-stealing intent rather than distinct algorithms.
 */

#include "concurrency/work_stealing_queue.hpp"

#include "concurrency/logger.hpp"

namespace keystone {
namespace concurrency {

WorkStealingQueue::WorkStealingQueue(size_t initial_capacity) : queue_(initial_capacity) {}

void WorkStealingQueue::push(WorkItem item) {
  // FIX #284: Capture correlation ID on submission thread
  if (item.correlation_id.empty()) {
    item.correlation_id = LogContext::getCorrelationId();
  }
  queue_.enqueue(std::move(item));
}

std::optional<WorkItem> WorkStealingQueue::pop() {
  WorkItem item = WorkItem::makeFunction(nullptr);
  if (queue_.try_dequeue(item)) {
    return item;
  }
  return std::nullopt;
}

std::optional<WorkItem> WorkStealingQueue::steal() {
  WorkItem item = WorkItem::makeFunction(nullptr);
  if (queue_.try_dequeue(item)) {
    // FIX #284: Restore correlation ID on worker thread before execution
    if (!item.correlation_id.empty()) {
      LogContext::setCorrelationId(item.correlation_id);
    }
    return item;
  }
  return std::nullopt;
}

size_t WorkStealingQueue::size_approx() const { return queue_.size_approx(); }

bool WorkStealingQueue::empty() const { return queue_.size_approx() == 0; }

}  // namespace concurrency
}  // namespace keystone
