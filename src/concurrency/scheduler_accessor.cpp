/**
 * @file scheduler_accessor.cpp
 * @brief Implementation of thread-local scheduler accessor
 */

#include "concurrency/scheduler_accessor.hpp"

namespace keystone {
namespace concurrency {

// Thread-local storage for scheduler pointer
// Each thread (including worker threads) can have its own scheduler
static thread_local WorkStealingScheduler* current_scheduler = nullptr;

WorkStealingScheduler* getCurrentScheduler() noexcept { return current_scheduler; }

void setCurrentScheduler(WorkStealingScheduler* scheduler) noexcept {
  current_scheduler = scheduler;
}

}  // namespace concurrency
}  // namespace keystone
