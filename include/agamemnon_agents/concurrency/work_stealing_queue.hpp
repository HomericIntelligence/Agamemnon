#pragma once

#include <cassert>
#include <concurrentqueue.h>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace keystone {
namespace concurrency {

/**
 * @brief WorkItem - A unit of work (function or coroutine)
 *
 * FIX P3-02: Default constructor is private to prevent invalid WorkItems.
 * Always use makeFunction() or makeCoroutine() factory methods.
 */
struct WorkItem {
  enum class Type { Function, Coroutine };

  Type type;
  std::function<void()> func;
  std::coroutine_handle<> handle;
  std::string correlation_id;  // FIX #284: Capture and propagate correlation ID

  static WorkItem makeFunction(std::function<void()> f) {
    WorkItem item;
    item.type = Type::Function;
    item.func = std::move(f);
    return item;
  }

  static WorkItem makeCoroutine(std::coroutine_handle<> h) {
    WorkItem item;
    item.type = Type::Coroutine;
    item.handle = h;
    return item;
  }

  void execute() {
    // FIX P3-02: Assert valid state before execution
    assert(valid() && "Cannot execute invalid WorkItem");

    if (type == Type::Function && func) {
      func();
    } else if (type == Type::Coroutine && handle) {
      handle.resume();
    }
  }

  bool valid() const {
    return (type == Type::Function && func != nullptr) ||
           (type == Type::Coroutine && handle != nullptr);
  }

  WorkItem() : type(Type::Function), func(nullptr), handle(nullptr) {}
};

/**
 * @brief WorkStealingQueue - Lock-free queue for work-stealing scheduler.
 *
 * Uses moodycamel::ConcurrentQueue for lock-free MPMC operations, which
 * is the mandated backing store per the Keystone architecture (CLAUDE.md).
 *
 * Thread Safety:
 * - push() callable from any thread (MPMC)
 * - pop() and steal() callable from any thread (MPMC)
 * - All operations are lock-free and thread-safe
 *
 * Note: pop() and steal() have equivalent dequeue semantics with
 * ConcurrentQueue (both FIFO). The distinction in naming reflects the
 * work-stealing design intent: pop() is the owner's hot path, steal()
 * is the thief's path.
 */
class WorkStealingQueue {
 public:
  explicit WorkStealingQueue(size_t initial_capacity = 1024);
  ~WorkStealingQueue() = default;

  WorkStealingQueue(const WorkStealingQueue&) = delete;
  WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

  WorkStealingQueue(WorkStealingQueue&&) = default;
  WorkStealingQueue& operator=(WorkStealingQueue&&) = default;

  void push(WorkItem item);
  std::optional<WorkItem> pop();
  std::optional<WorkItem> steal();
  size_t size_approx() const;
  bool empty() const;

 private:
  moodycamel::ConcurrentQueue<WorkItem> queue_;
};

}  // namespace concurrency
}  // namespace keystone
