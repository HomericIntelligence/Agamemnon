#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <optional>
#include <vector>

#include "concurrency/work_stealing_queue.hpp"

namespace keystone {
namespace concurrency {

/**
 * @brief PullOrSteal - Custom awaitable for work-stealing scheduler
 *
 * This awaitable implements the core work-stealing algorithm:
 * 1. Try to pop from own queue (LIFO - good locality)
 * 2. If empty, try to steal from other workers' queues (FIFO)
 * 3. If all queues empty, suspend and wait for work
 *
 * Usage in agent coroutines:
 *   auto work = co_await PullOrSteal(my_queue, all_queues, my_index);
 *   if (work.has_value()) {
 *       work->execute();
 *   }
 *
 * Thread Safety:
 * - Each worker has its own queue and index
 * - Stealing operations are thread-safe (lock-free queues)
 * - Suspension/resume coordinated via atomic flag
 */
class PullOrSteal {
 public:
  /**
   * @brief Construct a PullOrSteal awaitable
   *
   * @param own_queue Reference to this worker's queue
   * @param all_queues All workers' queues (for stealing)
   * @param worker_index This worker's index
   * @param shutdown_flag Atomic flag for shutdown signaling
   */
  PullOrSteal(WorkStealingQueue& own_queue, std::vector<WorkStealingQueue*>& all_queues,
              size_t worker_index, std::atomic<bool>& shutdown_flag);

  /**
   * @brief Check if work is immediately available (awaitable protocol)
   *
   * First tries to pop from own queue, then attempts stealing.
   * Returns true if work was found, false if coroutine should suspend.
   */
  bool await_ready() noexcept;

  /**
   * @brief Suspend the coroutine (awaitable protocol)
   *
   * Called when await_ready() returns false.
   * Stores the coroutine handle for later resumption.
   *
   * @param handle The suspended coroutine's handle
   */
  void await_suspend(std::coroutine_handle<> handle) noexcept;

  /**
   * @brief Resume and return the work item (awaitable protocol)
   *
   * Returns the work item found, or std::nullopt if shutdown requested.
   */
  std::optional<WorkItem> await_resume() noexcept;

 private:
  /**
   * @brief Try to steal work from other workers
   *
   * Iterates through all other workers' queues and attempts to steal.
   * Uses round-robin starting from (worker_index + 1) to distribute load.
   *
   * @return Stolen work item, or std::nullopt if all queues empty
   */
  std::optional<WorkItem> trySteal();

  WorkStealingQueue& own_queue_;
  std::vector<WorkStealingQueue*>& all_queues_;
  size_t worker_index_;
  std::atomic<bool>& shutdown_flag_;

  std::optional<WorkItem> result_;
  std::coroutine_handle<> awaiting_coroutine_;
};

/**
 * @brief PullOrStealWithTimeout - Variant with timeout for testing
 *
 * Similar to PullOrSteal but returns nullopt after a timeout.
 * Useful for tests and graceful shutdown scenarios.
 */
class PullOrStealWithTimeout {
 public:
  PullOrStealWithTimeout(WorkStealingQueue& own_queue, std::vector<WorkStealingQueue*>& all_queues,
                         size_t worker_index, std::atomic<bool>& shutdown_flag,
                         std::chrono::milliseconds timeout);

  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<> handle) noexcept;
  std::optional<WorkItem> await_resume() noexcept;

 private:
  std::optional<WorkItem> trySteal();

  WorkStealingQueue& own_queue_;
  std::vector<WorkStealingQueue*>& all_queues_;
  size_t worker_index_;
  std::atomic<bool>& shutdown_flag_;
  std::chrono::milliseconds timeout_;

  std::optional<WorkItem> result_;
  std::coroutine_handle<> awaiting_coroutine_;
  std::chrono::steady_clock::time_point start_time_;
};

}  // namespace concurrency
}  // namespace keystone
