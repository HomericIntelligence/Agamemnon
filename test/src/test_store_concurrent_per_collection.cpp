// #184: per-collection shared_mutex tests.
//
// Two responsibilities:
//   1. Independence tests — assert forward progress on one collection's
//      readers while another collection is being mutated. These encode the
//      post-split contract (no cross-collection critical sections exist).
//   2. Contention measurement — timed gtests recording ops/sec for
//      cross-collection vs intra-collection workloads so the "profiling
//      confirms it's the bottleneck" precondition from #184 is measurable.
//      Skipped under TSan where instrumentation distorts timings.
#include "agamemnon/store.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace agamemnon::test {

#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
constexpr bool kUnderTSan = true;
#else
constexpr bool kUnderTSan = false;
#endif

// ── Cross-collection independence ────────────────────────────────────────────

TEST(StorePerCollection, AgentWritesDoNotBlockTaskReads) {
  Store s;
  std::atomic<int> task_reads{0};
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      (void)s.list_all_tasks();
      task_reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  for (int i = 0; i < 1000; ++i) {
    s.create_agent({{"name", "a" + std::to_string(i)}});
  }
  stop.store(true, std::memory_order_relaxed);
  reader.join();
  EXPECT_GT(task_reads.load(), 0);
}

TEST(StorePerCollection, TeamWritesDoNotBlockFaultReads) {
  Store s;
  std::atomic<int> fault_reads{0};
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      (void)s.list_faults();
      fault_reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  for (int i = 0; i < 1000; ++i) {
    s.create_team({{"name", "t" + std::to_string(i)}});
  }
  stop.store(true, std::memory_order_relaxed);
  reader.join();
  EXPECT_GT(fault_reads.load(), 0);
}

TEST(StorePerCollection, TaskWritesDoNotBlockHmasReads) {
  Store s;
  std::atomic<int> hmas_reads{0};
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      auto out = s.list_hmas_tasks_by_layer(HmasLayer::kL0);
      EXPECT_TRUE(out.empty());
      hmas_reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  for (int i = 0; i < 500; ++i) {
    (void)s.create_task("team-scope", {{"subject", "task-" + std::to_string(i)}});
  }
  stop.store(true, std::memory_order_relaxed);
  reader.join();
  EXPECT_GT(hmas_reads.load(), 0);
}

// ── Contention measurement (#184 profiling evidence) ─────────────────────────

TEST(StorePerCollection, ContentionMeasurementCrossCollection) {
  if constexpr (kUnderTSan) {
    GTEST_SKIP() << "Timings distorted under TSan";
  }
  Store s;
  constexpr int kReaders = 8;
  constexpr int kWriters = 8;
  constexpr auto kDuration = std::chrono::milliseconds(500);
  std::atomic<long> reads{0};
  std::atomic<long> writes{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kReaders + kWriters));
  for (int i = 0; i < kReaders; ++i) {
    threads.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        (void)s.list_all_tasks();
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (int i = 0; i < kWriters; ++i) {
    threads.emplace_back([&, i] {
      int n = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        (void)s.create_agent(
            {{"name", "w" + std::to_string(i) + "_" + std::to_string(n++)}});
        writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::this_thread::sleep_for(kDuration);
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) t.join();
  const auto secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(kDuration).count();
  std::cerr << "[per-collection] cross-collection reads=" << reads.load()
            << " (" << static_cast<long>(static_cast<double>(reads.load()) / secs)
            << "/s)"
            << " writes=" << writes.load()
            << " (" << static_cast<long>(static_cast<double>(writes.load()) / secs)
            << "/s)\n";
  EXPECT_GT(reads.load(), 0);
  EXPECT_GT(writes.load(), 0);
}

TEST(StorePerCollection, ContentionMeasurementIntraCollection) {
  if constexpr (kUnderTSan) {
    GTEST_SKIP() << "Timings distorted under TSan";
  }
  Store s;
  for (int i = 0; i < 100; ++i) {
    (void)s.create_agent({{"name", "seed-" + std::to_string(i)}});
  }
  constexpr int kReaders = 16;
  constexpr auto kDuration = std::chrono::milliseconds(500);
  std::atomic<long> reads{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kReaders));
  for (int i = 0; i < kReaders; ++i) {
    threads.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        (void)s.list_agents();
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::this_thread::sleep_for(kDuration);
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) t.join();
  const auto secs =
      std::chrono::duration_cast<std::chrono::duration<double>>(kDuration).count();
  std::cerr << "[per-collection] intra-collection reads=" << reads.load()
            << " (" << static_cast<long>(static_cast<double>(reads.load()) / secs)
            << "/s)\n";
  EXPECT_GT(reads.load(), 0);
}

}  // namespace agamemnon::test
