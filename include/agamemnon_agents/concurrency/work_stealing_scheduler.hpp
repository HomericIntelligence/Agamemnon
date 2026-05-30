#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "concurrency/logger.hpp"
#include "concurrency/pull_or_steal.hpp"
#include "concurrency/work_stealing_queue.hpp"

namespace keystone {
namespace concurrency {

/**
 * @brief WorkStealingScheduler - Orchestrates work-stealing across worker
 * threads
 *
 * This scheduler manages a fixed pool of worker threads, each with its own
 * work-stealing queue. Workers try to pop from their own queue first (LIFO),
 * and steal from other workers when idle (FIFO).
 *
 * Architecture:
 * - Each worker has a dedicated queue (cache-friendly)
 * - Workers run a loop: pull from own queue or steal from others
 * - Round-robin work submission for load balancing
 * - Graceful shutdown with work completion
 *
 * Usage:
 *   WorkStealingScheduler scheduler(4);  // 4 workers
 *   scheduler.start();
 *
 *   scheduler.submit([]() { });  // Submit function
 *   scheduler.submit(coroutine_handle);  // Submit coroutine
 *
 *   scheduler.shutdown();
 *
 * Integration with Agents:
 * - Each agent coroutine submitted to scheduler
 * - LogContext set per worker thread
 * - Messages routed to appropriate worker queues
 */
class WorkStealingScheduler {
 public:
  /**
   * @brief Construct a work-stealing scheduler
   *
   * @param num_workers Number of worker threads (default: hardware_concurrency)
   * @param enable_cpu_affinity Pin worker threads to CPU cores (default: false)
   *
   * Phase D: CPU affinity improves cache locality by preventing thread
   * migration. When enabled, worker i is pinned to CPU core (i % num_cores).
   */
  explicit WorkStealingScheduler(size_t num_workers = std::thread::hardware_concurrency(),
                                 bool enable_cpu_affinity = false);

  /**
   * @brief Destructor - ensures graceful shutdown
   */
  ~WorkStealingScheduler();

  // Non-copyable, non-movable (manages threads)
  WorkStealingScheduler(const WorkStealingScheduler&) = delete;
  WorkStealingScheduler& operator=(const WorkStealingScheduler&) = delete;
  WorkStealingScheduler(WorkStealingScheduler&&) = delete;
  WorkStealingScheduler& operator=(WorkStealingScheduler&&) = delete;

  /**
   * @brief Start all worker threads
   *
   * Must be called before submitting work.
   */
  void start();

  /**
   * @brief Submit a function for execution
   *
   * @param func Function to execute
   */
  void submit(std::function<void()> func);

  /**
   * @brief Submit a coroutine handle for execution
   *
   * @param handle Coroutine handle to resume
   */
  void submit(std::coroutine_handle<> handle);

  /**
   * @brief Submit work to a specific worker queue
   *
   * Useful for affinity-based scheduling (e.g., agent-to-worker mapping)
   *
   * @param worker_index Target worker index
   * @param func Function to execute
   */
  void submitTo(size_t worker_index, std::function<void()> func);

  /**
   * @brief Submit coroutine to a specific worker queue
   *
   * @param worker_index Target worker index
   * @param handle Coroutine handle to resume
   */
  void submitTo(size_t worker_index, std::coroutine_handle<> handle);

  /**
   * @brief Request graceful shutdown
   *
   * Sets shutdown flag and waits for all workers to complete current work.
   * Blocks until all threads have exited.
   */
  void shutdown();

  /**
   * @brief Check if scheduler is running
   */
  bool isRunning() const;

  /**
   * @brief Get number of worker threads
   */
  size_t getNumWorkers() const;

  /**
   * @brief Get approximate total work items across all queues
   *
   * Note: Approximate due to lock-free nature
   */
  size_t getApproximateWorkCount() const;

  /**
   * @brief Try to steal work from a random worker queue (for NUMA simulation)
   *
   * This method is used by SimulatedNUMANode to implement cross-node work
   * stealing. It attempts to steal from a random worker's queue.
   *
   * @return std::optional<std::function<void()>> Stolen work item, or nullopt
   * if no work available
   */
  std::optional<std::function<void()>> tryStealWork();

 private:
  /**
   * @brief Worker thread main loop
   *
   * Each worker:
   * 1. Sets thread-local LogContext
   * 2. (Phase D) Sets CPU affinity if enabled
   * 3. Loops: co_await PullOrSteal
   * 4. Executes work items
   * 5. Exits on shutdown
   *
   * @param worker_index This worker's index
   */
  void workerLoop(size_t worker_index);

  /**
   * @brief Try to steal work with 3-phase backoff strategy
   *
   * Stream C1: 3-phase backoff to reduce idle CPU usage from ~5% to <2%
   *
   * Phase 1 (SPIN): 0-100 iterations, tight loop, ultra-low latency
   * Phase 2 (YIELD): 101-1000 iterations, yield CPU to other threads
   * Phase 3 (SLEEP): 1001+ iterations, sleep with wake-up notification
   *
   * @param worker_index This worker's index
   * @return Work item if found, nullopt otherwise
   */
  std::optional<WorkItem> tryStealWithBackoff(size_t worker_index);

  /**
   * @brief Try own queue then steal from all other workers once
   *
   * Shared inner loop body for all three backoff phases.
   *
   * @param worker_index This worker's index
   * @param phase_label Log label for the calling phase ("SPIN", "YIELD",
   * "SLEEP")
   * @return Work item if found, nullopt otherwise
   */
  std::optional<WorkItem> tryStealOnce(size_t worker_index, const char* phase_label);

  /**
   * @brief Get next worker index for round-robin submission
   */
  size_t getNextWorkerIndex();

  /**
   * @brief Set CPU affinity for current thread (Phase D)
   *
   * Pins the calling thread to a specific CPU core to improve cache locality.
   * On Linux, uses pthread_setaffinity_np(). On other platforms, no-op.
   *
   * @param worker_index Worker index (maps to CPU core via modulo)
   */
  void setCPUAffinity(size_t worker_index);

  // Maximum number of worker threads (prevents DoS via excessive thread
  // creation)
  static constexpr size_t MAX_WORKER_THREADS = 256;

  // Stream C1: 3-phase backoff thresholds
  static constexpr size_t SPIN_ITERATIONS = 100;
  static constexpr size_t YIELD_ITERATIONS = 1000;
  static constexpr auto SLEEP_DURATION = std::chrono::milliseconds(1);

  size_t num_workers_;
  bool enable_cpu_affinity_;  // Phase D: Enable CPU affinity
  std::vector<std::unique_ptr<WorkStealingQueue>> worker_queues_;
  std::vector<WorkStealingQueue*> queue_pointers_;  // For PullOrSteal
  std::vector<std::thread> workers_;

  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> running_{false};
  std::atomic<size_t> next_worker_index_{0};  // Round-robin counter

  // FIX Issue #18: Condition variable for fast shutdown notification
  std::condition_variable shutdown_cv_;
  std::mutex shutdown_mutex_;
};

}  // namespace concurrency
}  // namespace keystone
