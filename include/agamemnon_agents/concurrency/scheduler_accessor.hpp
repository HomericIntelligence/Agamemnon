#pragma once

#include <thread>

namespace keystone {
namespace concurrency {

// Forward declaration
class WorkStealingScheduler;

/**
 * @brief Thread-local scheduler accessor for Task<T> integration
 *
 * Provides a way for Task<T> coroutines to access the current thread's
 * WorkStealingScheduler without requiring a global singleton.
 *
 * Usage:
 *   // In worker thread:
 *   setCurrentScheduler(scheduler);
 *
 *   // In Task<T>::await_suspend():
 *   auto scheduler = getCurrentScheduler();
 *   if (scheduler) {
 *       scheduler->submit(handle);
 *   }
 *
 * This approach:
 * - Allows multiple schedulers to coexist
 * - Automatically set by WorkStealingScheduler worker threads
 * - Provides backward compatibility (nullptr = synchronous fallback)
 * - No global state or singleton pattern
 */

/**
 * @brief Get the current thread's WorkStealingScheduler
 *
 * @return Pointer to current thread's scheduler, or nullptr if not set
 *
 * Thread-safe: Each thread has its own value.
 */
WorkStealingScheduler* getCurrentScheduler() noexcept;

/**
 * @brief Set the current thread's WorkStealingScheduler
 *
 * Called by WorkStealingScheduler::workerLoop() to register the scheduler
 * for the current worker thread.
 *
 * @param scheduler Pointer to scheduler (or nullptr to clear)
 *
 * Thread-safe: Each thread has its own value.
 */
void setCurrentScheduler(WorkStealingScheduler* scheduler) noexcept;

}  // namespace concurrency
}  // namespace keystone
