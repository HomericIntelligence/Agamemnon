/**
 * @file test_pull_or_steal.cpp
 * @brief Unit tests for keystone::concurrency::PullOrSteal awaitables
 *
 * Exercises the ported src/concurrency/pull_or_steal.cpp under the coverage
 * harness. PullOrSteal / PullOrStealWithTimeout are C++20 awaitables; their
 * await_ready / await_suspend / await_resume methods are plain member
 * functions and can be driven directly without a real coroutine frame by
 * passing std::noop_coroutine() as the handle.
 */

#include <atomic>
#include <chrono>
#include <coroutine>
#include <vector>

#include "concurrency/pull_or_steal.hpp"
#include "concurrency/work_stealing_queue.hpp"
#include <gtest/gtest.h>

using namespace keystone::concurrency;

namespace {

WorkItem makeCounting(std::atomic<int>& counter) {
  return WorkItem::makeFunction([&counter]() { counter.fetch_add(1); });
}

// ---------------------------------------------------------------------------
// PullOrSteal
// ---------------------------------------------------------------------------

TEST(PullOrStealTest, AwaitReadyPopsFromOwnQueue) {
  WorkStealingQueue own;
  std::vector<WorkStealingQueue*> all{&own};
  std::atomic<bool> shutdown{false};
  std::atomic<int> counter{0};

  own.push(makeCounting(counter));

  PullOrSteal awaitable(own, all, 0, shutdown);
  // Work is locally available -> await_ready returns true (no suspension).
  EXPECT_TRUE(awaitable.await_ready());

  auto result = awaitable.await_resume();
  ASSERT_TRUE(result.has_value());
  result->execute();
  EXPECT_EQ(counter.load(), 1);
}

TEST(PullOrStealTest, AwaitReadyStealsFromAnotherQueue) {
  WorkStealingQueue own;
  WorkStealingQueue victim;
  std::vector<WorkStealingQueue*> all{&own, &victim};
  std::atomic<bool> shutdown{false};
  std::atomic<int> counter{0};

  // Own queue empty, victim has work -> await_ready must steal it.
  victim.push(makeCounting(counter));

  PullOrSteal awaitable(own, all, 0, shutdown);
  EXPECT_TRUE(awaitable.await_ready());

  auto result = awaitable.await_resume();
  ASSERT_TRUE(result.has_value());
  result->execute();
  EXPECT_EQ(counter.load(), 1);
}

TEST(PullOrStealTest, AwaitReadyFalseWhenAllQueuesEmpty) {
  WorkStealingQueue own;
  WorkStealingQueue other;
  std::vector<WorkStealingQueue*> all{&own, &other};
  std::atomic<bool> shutdown{false};

  PullOrSteal awaitable(own, all, 0, shutdown);
  // No work anywhere -> coroutine would suspend.
  EXPECT_FALSE(awaitable.await_ready());
}

TEST(PullOrStealTest, AwaitSuspendRetriesAndFindsLateWork) {
  WorkStealingQueue own;
  std::vector<WorkStealingQueue*> all{&own};
  std::atomic<bool> shutdown{false};
  std::atomic<int> counter{0};

  PullOrSteal awaitable(own, all, 0, shutdown);
  EXPECT_FALSE(awaitable.await_ready());

  // Push work after await_ready saw an empty queue; await_suspend retries.
  own.push(makeCounting(counter));
  awaitable.await_suspend(std::noop_coroutine());

  auto result = awaitable.await_resume();
  ASSERT_TRUE(result.has_value());
  result->execute();
  EXPECT_EQ(counter.load(), 1);
}

TEST(PullOrStealTest, AwaitResumeReturnsNulloptOnShutdown) {
  WorkStealingQueue own;
  std::vector<WorkStealingQueue*> all{&own};
  std::atomic<bool> shutdown{false};
  std::atomic<int> counter{0};

  own.push(makeCounting(counter));
  PullOrSteal awaitable(own, all, 0, shutdown);
  EXPECT_TRUE(awaitable.await_ready());

  // Shutdown requested after work was found -> await_resume yields nullopt.
  shutdown.store(true);
  EXPECT_FALSE(awaitable.await_resume().has_value());
}

TEST(PullOrStealTest, NullVictimQueueIsSkippedDuringSteal) {
  WorkStealingQueue own;
  WorkStealingQueue real_victim;
  // Mix in a null pointer to exercise the null-guard in trySteal().
  std::vector<WorkStealingQueue*> all{&own, nullptr, &real_victim};
  std::atomic<bool> shutdown{false};
  std::atomic<int> counter{0};

  real_victim.push(makeCounting(counter));

  PullOrSteal awaitable(own, all, 0, shutdown);
  EXPECT_TRUE(awaitable.await_ready());
  auto result = awaitable.await_resume();
  ASSERT_TRUE(result.has_value());
  result->execute();
  EXPECT_EQ(counter.load(), 1);
}

// ---------------------------------------------------------------------------
// PullOrStealWithTimeout
// ---------------------------------------------------------------------------

TEST(PullOrStealWithTimeoutTest, AwaitReadyPopsLocalWork) {
  WorkStealingQueue own;
  std::vector<WorkStealingQueue*> all{&own};
  std::atomic<bool> shutdown{false};
  std::atomic<int> counter{0};

  own.push(makeCounting(counter));
  PullOrStealWithTimeout awaitable(own, all, 0, shutdown, std::chrono::milliseconds(50));
  EXPECT_TRUE(awaitable.await_ready());

  auto result = awaitable.await_resume();
  ASSERT_TRUE(result.has_value());
  result->execute();
  EXPECT_EQ(counter.load(), 1);
}

TEST(PullOrStealWithTimeoutTest, ExpiredTimeoutYieldsNoWork) {
  WorkStealingQueue own;
  std::vector<WorkStealingQueue*> all{&own};
  std::atomic<bool> shutdown{false};

  // Zero timeout: by the time await_suspend runs, the deadline has passed.
  PullOrStealWithTimeout awaitable(own, all, 0, shutdown, std::chrono::milliseconds(0));
  EXPECT_FALSE(awaitable.await_ready());

  awaitable.await_suspend(std::noop_coroutine());
  EXPECT_FALSE(awaitable.await_resume().has_value());
}

TEST(PullOrStealWithTimeoutTest, SuspendFindsWorkBeforeDeadline) {
  WorkStealingQueue own;
  std::vector<WorkStealingQueue*> all{&own};
  std::atomic<bool> shutdown{false};
  std::atomic<int> counter{0};

  PullOrStealWithTimeout awaitable(own, all, 0, shutdown, std::chrono::milliseconds(100));
  EXPECT_FALSE(awaitable.await_ready());

  own.push(makeCounting(counter));
  awaitable.await_suspend(std::noop_coroutine());

  auto result = awaitable.await_resume();
  ASSERT_TRUE(result.has_value());
  result->execute();
  EXPECT_EQ(counter.load(), 1);
}

TEST(PullOrStealWithTimeoutTest, ShutdownYieldsNullopt) {
  WorkStealingQueue own;
  std::vector<WorkStealingQueue*> all{&own};
  std::atomic<bool> shutdown{true};
  std::atomic<int> counter{0};

  own.push(makeCounting(counter));
  PullOrStealWithTimeout awaitable(own, all, 0, shutdown, std::chrono::milliseconds(50));
  awaitable.await_ready();
  EXPECT_FALSE(awaitable.await_resume().has_value());
}

}  // namespace
