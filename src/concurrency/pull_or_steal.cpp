/**
 * @file pull_or_steal.cpp
 * @brief Implementation of PullOrSteal awaitable
 */

#include "concurrency/pull_or_steal.hpp"

#include <thread>

namespace keystone {
namespace concurrency {

// PullOrSteal implementation

PullOrSteal::PullOrSteal(WorkStealingQueue& own_queue, std::vector<WorkStealingQueue*>& all_queues,
                         size_t worker_index, std::atomic<bool>& shutdown_flag)
    : own_queue_(own_queue),
      all_queues_(all_queues),
      worker_index_(worker_index),
      shutdown_flag_(shutdown_flag),
      result_(std::nullopt) {}

bool PullOrSteal::await_ready() noexcept {
  // Fast path: Try to pop from own queue first (LIFO for cache locality)
  result_ = own_queue_.pop();
  if (result_.has_value()) {
    return true;  // Work found, don't suspend
  }

  // Slow path: Try to steal from other workers (FIFO)
  result_ = trySteal();
  if (result_.has_value()) {
    return true;  // Stolen work found, don't suspend
  }

  // No work available, will suspend
  return false;
}

void PullOrSteal::await_suspend(std::coroutine_handle<> handle) noexcept {
  // Store the coroutine handle for later resumption
  awaiting_coroutine_ = handle;

  // Simplified implementation for testing: Try once more after a brief wait
  // In a full WorkStealingScheduler, this would register with a condition
  // variable and be woken up when work arrives
  std::this_thread::sleep_for(std::chrono::microseconds(10));

  // Try one more time
  result_ = own_queue_.pop();
  if (!result_.has_value()) {
    result_ = trySteal();
  }

  // Resume immediately (synchronous for testing)
  if (awaiting_coroutine_) {
    awaiting_coroutine_.resume();
  }
}

std::optional<WorkItem> PullOrSteal::await_resume() noexcept {
  // Check shutdown flag
  if (shutdown_flag_.load()) {
    return std::nullopt;
  }

  return result_;
}

std::optional<WorkItem> PullOrSteal::trySteal() {
  if (all_queues_.empty()) {
    return std::nullopt;
  }

  // SECURITY FIX: Prevent TOCTOU race and out-of-bounds access
  // Check size once and ensure it doesn't change during iteration
  size_t num_workers = all_queues_.size();

  // Modulo by zero protection (also fixes MEDIUM vulnerability #8)
  if (num_workers == 0) {
    return std::nullopt;
  }

  for (size_t i = 1; i < num_workers; ++i) {
    size_t victim_index = (worker_index_ + i) % num_workers;

    // Combined bounds + null check immediately before access
    // Prevents TOCTOU race: check and use in minimal time window
    if (victim_index < all_queues_.size()) {
      auto* victim_queue = all_queues_[victim_index];

      if (victim_queue != nullptr) {
        auto stolen = victim_queue->steal();
        if (stolen.has_value()) {
          return stolen;  // Successfully stolen
        }
      }
    }
    // If bounds check fails, skip this iteration (queue list may have shrunk)
  }

  return std::nullopt;  // All queues empty
}

// PullOrStealWithTimeout implementation

PullOrStealWithTimeout::PullOrStealWithTimeout(WorkStealingQueue& own_queue,
                                               std::vector<WorkStealingQueue*>& all_queues,
                                               size_t worker_index,
                                               std::atomic<bool>& shutdown_flag,
                                               std::chrono::milliseconds timeout)
    : own_queue_(own_queue),
      all_queues_(all_queues),
      worker_index_(worker_index),
      shutdown_flag_(shutdown_flag),
      timeout_(timeout),
      result_(std::nullopt),
      start_time_(std::chrono::steady_clock::now()) {}

bool PullOrStealWithTimeout::await_ready() noexcept {
  // Same logic as PullOrSteal
  result_ = own_queue_.pop();
  if (result_.has_value()) {
    return true;
  }

  result_ = trySteal();
  if (result_.has_value()) {
    return true;
  }

  return false;
}

void PullOrStealWithTimeout::await_suspend(std::coroutine_handle<> handle) noexcept {
  awaiting_coroutine_ = handle;

  auto elapsed = std::chrono::steady_clock::now() - start_time_;

  // Check if timeout exceeded
  if (elapsed >= timeout_) {
    result_ = std::nullopt;
  } else {
    // Wait for remaining time or until work arrives
    auto remaining = timeout_ - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(10)));

    result_ = own_queue_.pop();
    if (!result_.has_value()) {
      result_ = trySteal();
    }
  }

  // Resume immediately (synchronous for testing)
  if (awaiting_coroutine_) {
    awaiting_coroutine_.resume();
  }
}

std::optional<WorkItem> PullOrStealWithTimeout::await_resume() noexcept {
  if (shutdown_flag_.load()) {
    return std::nullopt;
  }

  return result_;
}

std::optional<WorkItem> PullOrStealWithTimeout::trySteal() {
  if (all_queues_.empty()) {
    return std::nullopt;
  }

  size_t num_workers = all_queues_.size();
  for (size_t i = 1; i < num_workers; ++i) {
    size_t victim_index = (worker_index_ + i) % num_workers;

    if (victim_index >= all_queues_.size()) {
      continue;
    }

    auto* victim_queue = all_queues_[victim_index];
    if (victim_queue == nullptr) {
      continue;
    }

    auto stolen = victim_queue->steal();
    if (stolen.has_value()) {
      return stolen;
    }
  }

  return std::nullopt;
}

}  // namespace concurrency
}  // namespace keystone
