#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "concurrency/scheduler_accessor.hpp"
#include "concurrency/work_stealing_scheduler.hpp"

namespace keystone {
namespace concurrency {

/**
 * @brief Task<T> - A C++20 coroutine awaitable type for asynchronous operations
 *
 * This class represents an asynchronous task that can be co_awaited. It uses
 * C++20 coroutines to provide a clean async/await style API for agent
 * operations.
 *
 * Usage:
 *   Task<int> example() {
 *       co_return 42;
 *   }
 *
 *   Task<void> consumer() {
 *       int value = co_await example();
 *   }
 *
 * @tparam T The return type of the coroutine
 */
template <typename T = void>
class Task {
 public:
  /**
   * @brief Promise type for Task<T> coroutines
   *
   * This type is required by C++20 coroutines and defines how the coroutine
   * behaves: how it starts, suspends, returns values, and handles exceptions.
   */
  struct promise_type {
    std::optional<T> result;
    std::exception_ptr exception;
    std::coroutine_handle<> continuation;  // Stores awaiting coroutine

    /**
     * @brief Creates the Task object from the promise
     */
    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /**
     * @brief Coroutine starts suspended (lazy evaluation)
     */
    std::suspend_always initial_suspend() noexcept { return {}; }

    /**
     * @brief Coroutine suspends after completion and resumes continuation
     *
     * Uses symmetric transfer to efficiently chain coroutines without
     * consuming stack space.
     */
    auto final_suspend() noexcept {
      struct final_awaiter {
        std::coroutine_handle<> continuation;

        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(
            [[maybe_unused]] std::coroutine_handle<promise_type> h) noexcept {
          // If we have a continuation, resume it via symmetric transfer
          if (continuation) {
            return continuation;
          }
          // No continuation, suspend indefinitely
          return std::noop_coroutine();
        }

        void await_resume() noexcept {}
      };

      return final_awaiter{continuation};
    }

    /**
     * @brief Stores the return value
     */
    void return_value(T value) { result = std::move(value); }

    /**
     * @brief Captures exceptions thrown in the coroutine
     */
    void unhandled_exception() { exception = std::current_exception(); }
  };

  // Constructor from coroutine handle
  explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  // Move-only semantics
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  // Destructor
  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  /**
   * @brief Check if the coroutine has finished execution
   */
  bool done() const { return handle_ && handle_.done(); }

  /**
   * @brief Resume the coroutine execution
   */
  void resume() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
    }
  }

  /**
   * @brief Get the result value (throws if exception occurred)
   *
   * This method will resume the coroutine until completion if not already done.
   *
   * @return The result value
   * @throws std::runtime_error if coroutine not done
   * @throws Any exception thrown by the coroutine
   */
  T get() {
    // Resume until done
    while (handle_ && !handle_.done()) {
      handle_.resume();
    }

    if (!handle_) {
      throw std::runtime_error("Task: Invalid coroutine handle");
    }

    // Check for exception
    if (handle_.promise().exception) {
      std::rethrow_exception(handle_.promise().exception);
    }

    // Check for result
    if (!handle_.promise().result.has_value()) {
      throw std::runtime_error("Task: No result available");
    }

    return std::move(handle_.promise().result.value());
  }

  /**
   * @brief Awaitable interface - check if result is ready
   */
  bool await_ready() const noexcept { return handle_ && handle_.done(); }

  /**
   * @brief Awaitable interface - suspend and transfer control
   *
   * Scheduler-aware version that submits coroutine to WorkStealingScheduler
   * if available, otherwise uses symmetric transfer for synchronous execution.
   *
   * Scheduler Integration (Issue #19):
   * - If scheduler available: submit this task to scheduler and return noop
   * - If no scheduler: use symmetric transfer (backward compatible)
   *
   * The awaiting coroutine is stored as continuation and resumed
   * when this task completes via final_suspend.
   *
   * @param awaiting The coroutine that is awaiting this task
   * @return Handle to the coroutine to resume next
   */
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
    // Store the awaiting coroutine as our continuation
    // This is used in final_suspend regardless of execution mode
    handle_.promise().continuation = awaiting;

    // Check if a scheduler is available in the current thread
    auto scheduler = getCurrentScheduler();

    if (scheduler) {
      // Submit this coroutine to the scheduler for execution on a worker thread
      // Cast to std::coroutine_handle<> to disambiguate overload
      scheduler->submit(std::coroutine_handle<>(handle_));

      // Return noop - the scheduler will resume our coroutine
      // The continuation will be resumed by final_suspend when we complete
      return std::noop_coroutine();
    }

    // Fallback: no scheduler available, use synchronous symmetric transfer
    // The runtime will resume this handle, which will eventually
    // resume the continuation via final_suspend
    return handle_;
  }

  /**
   * @brief Awaitable interface - get the result
   */
  T await_resume() { return get(); }

  /**
   * @brief Get the underlying coroutine handle
   *
   * ⚠️  DANGER: EXPERT-ONLY API - HIGH RISK OF USE-AFTER-FREE ⚠️
   *
   * This method exposes the raw coroutine handle, which is EXTREMELY UNSAFE:
   *
   * SAFETY VIOLATIONS:
   * 1. Task owns the handle and destroys it in ~Task()
   * 2. Returned handle is just a pointer - no ownership transfer
   * 3. If Task is destroyed/moved, handle becomes DANGLING
   * 4. Calling resume() on a dangling handle = UNDEFINED BEHAVIOR (segfault)
   * 5. No thread-safety - concurrent resume() from multiple threads = DATA RACE
   *
   * PROHIBITED USE CASES:
   * ❌ DO NOT extract handle and submit to WorkStealingScheduler manually
   * ❌ DO NOT store the handle beyond Task lifetime
   * ❌ DO NOT call resume() from worker threads
   * ❌ DO NOT call destroy() on the returned handle (Task owns it)
   *
   * CORRECT WAY to schedule coroutines:
   * ✅ Use co_await - Task::await_suspend() integrates with scheduler
   * automatically ✅ Let Task manage handle lifetime ✅ Use symmetric transfer
   * for efficient coroutine chaining
   *
   * Example WRONG usage (causes use-after-free):
   *   Task<void> task = myCoroutine();
   *   auto h = task.get_handle();
   *   scheduler.submit([h]() { h.resume(); });  // ❌ CRASH - race with ~Task()
   *
   * Example CORRECT usage:
   *   Task<void> myCoroutine() {
   *     co_await otherTask();  // ✅ Scheduler integration automatic
   *     co_return;
   *   }
   *
   * This API exists only for low-level debugging and introspection.
   * If you think you need this, you probably don't.
   *
   * @return Raw coroutine handle (DANGEROUS - see warnings above)
   */
  std::coroutine_handle<> get_handle() const { return handle_; }

 private:
  std::coroutine_handle<promise_type> handle_;
};

/**
 * @brief Specialization of Task for void return type
 */
template <>
class Task<void> {
 public:
  struct promise_type {
    std::exception_ptr exception;
    std::coroutine_handle<> continuation;  // Stores awaiting coroutine

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    /**
     * @brief Coroutine suspends after completion and resumes continuation
     *
     * Uses symmetric transfer to efficiently chain coroutines without
     * consuming stack space.
     */
    auto final_suspend() noexcept {
      struct final_awaiter {
        std::coroutine_handle<> continuation;

        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(
            [[maybe_unused]] std::coroutine_handle<promise_type> h) noexcept {
          // If we have a continuation, resume it via symmetric transfer
          if (continuation) {
            return continuation;
          }
          // No continuation, suspend indefinitely
          return std::noop_coroutine();
        }

        void await_resume() noexcept {}
      };

      return final_awaiter{continuation};
    }

    void return_void() noexcept {}

    void unhandled_exception() { exception = std::current_exception(); }
  };

  explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  bool done() const { return handle_ && handle_.done(); }

  void resume() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
    }
  }

  void get() {
    // Resume until done
    while (handle_ && !handle_.done()) {
      handle_.resume();
    }

    if (!handle_) {
      throw std::runtime_error("Task: Invalid coroutine handle");
    }

    // Check for exception
    if (handle_.promise().exception) {
      std::rethrow_exception(handle_.promise().exception);
    }
  }

  bool await_ready() const noexcept { return handle_ && handle_.done(); }

  /**
   * @brief Awaitable interface - suspend and transfer control
   *
   * Scheduler-aware version that submits coroutine to WorkStealingScheduler
   * if available, otherwise uses symmetric transfer for synchronous execution.
   *
   * Scheduler Integration (Issue #19):
   * - If scheduler available: submit this task to scheduler and return noop
   * - If no scheduler: use symmetric transfer (backward compatible)
   *
   * The awaiting coroutine is stored as continuation and resumed
   * when this task completes via final_suspend.
   *
   * @param awaiting The coroutine that is awaiting this task
   * @return Handle to the coroutine to resume next
   */
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
    // Store the awaiting coroutine as our continuation
    // This is used in final_suspend regardless of execution mode
    handle_.promise().continuation = awaiting;

    // Check if a scheduler is available in the current thread
    auto scheduler = getCurrentScheduler();

    if (scheduler) {
      // Submit this coroutine to the scheduler for execution on a worker thread
      // Cast to std::coroutine_handle<> to disambiguate overload
      scheduler->submit(std::coroutine_handle<>(handle_));

      // Return noop - the scheduler will resume our coroutine
      // The continuation will be resumed by final_suspend when we complete
      return std::noop_coroutine();
    }

    // Fallback: no scheduler available, use synchronous symmetric transfer
    // The runtime will resume this handle, which will eventually
    // resume the continuation via final_suspend
    return handle_;
  }

  void await_resume() { get(); }

  /**
   * @brief Get the underlying coroutine handle
   *
   * ⚠️  DANGER: EXPERT-ONLY API - HIGH RISK OF USE-AFTER-FREE ⚠️
   *
   * This method exposes the raw coroutine handle, which is EXTREMELY UNSAFE:
   *
   * SAFETY VIOLATIONS:
   * 1. Task owns the handle and destroys it in ~Task()
   * 2. Returned handle is just a pointer - no ownership transfer
   * 3. If Task is destroyed/moved, handle becomes DANGLING
   * 4. Calling resume() on a dangling handle = UNDEFINED BEHAVIOR (segfault)
   * 5. No thread-safety - concurrent resume() from multiple threads = DATA RACE
   *
   * PROHIBITED USE CASES:
   * ❌ DO NOT extract handle and submit to WorkStealingScheduler manually
   * ❌ DO NOT store the handle beyond Task lifetime
   * ❌ DO NOT call resume() from worker threads
   * ❌ DO NOT call destroy() on the returned handle (Task owns it)
   *
   * CORRECT WAY to schedule coroutines:
   * ✅ Use co_await - Task::await_suspend() integrates with scheduler
   * automatically ✅ Let Task manage handle lifetime ✅ Use symmetric transfer
   * for efficient coroutine chaining
   *
   * Example WRONG usage (causes use-after-free):
   *   Task<void> task = myCoroutine();
   *   auto h = task.get_handle();
   *   scheduler.submit([h]() { h.resume(); });  // ❌ CRASH - race with ~Task()
   *
   * Example CORRECT usage:
   *   Task<void> myCoroutine() {
   *     co_await otherTask();  // ✅ Scheduler integration automatic
   *     co_return;
   *   }
   *
   * This API exists only for low-level debugging and introspection.
   * If you think you need this, you probably don't.
   *
   * @return Raw coroutine handle (DANGEROUS - see warnings above)
   */
  std::coroutine_handle<> get_handle() const { return handle_; }

 private:
  std::coroutine_handle<promise_type> handle_;
};

}  // namespace concurrency
}  // namespace keystone
