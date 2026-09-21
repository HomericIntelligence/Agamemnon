// #184: per-collection shared_mutex tests.
//
// Two responsibilities:
//   1. Independence tests — pin one collection behind an exclusive lock held
//      by a helper thread (via Store's #184 testing seam) and assert that
//      operations on *other* collections still make progress within a bounded
//      deadline. These encode the post-split contract and FAIL against a
//      single coarse mutex, where the pinned collection's lock would block
//      every other operation on the Store.
//   2. Contention measurement — timed gtests recording ops/sec for
//      cross-collection vs intra-collection workloads so the "profiling
//      confirms it's the bottleneck" precondition from #184 is measurable.
//      Skipped under TSan where instrumentation distorts timings.
#include "agamemnon/fleet_issue.hpp"
#include "agamemnon/store.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "../fleet/conditional_authority.hpp"
#include <gtest/gtest.h>

namespace agamemnon::test {

using agamemmon_clock = std::chrono::steady_clock;

#if defined(__SANITIZE_THREAD__)
constexpr bool kUnderTSan = true;
#elif defined(__clang__) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kUnderTSan = true;
#else
constexpr bool kUnderTSan = false;
#endif
#else
constexpr bool kUnderTSan = false;
#endif

// Pins `pinned` behind an exclusive lock held by a helper thread until this
// function returns, runs `body` on another thread, and reports whether body
// completed within `deadline`. Against a single coarse mutex the body cannot
// acquire any collection lock while `pinned` is held, so it misses the
// deadline and the test fails instead of hanging.
bool progresses_while_pinned(Store& s, Store::Collection pinned, std::function<void()> body,
                             std::chrono::milliseconds deadline) {
  std::promise<void> entered;
  std::promise<void> release;
  auto pin_thread = std::async(std::launch::async, [&] {
    s.with_collection_locked(pinned, [&] {
      entered.set_value();
      release.get_future().wait();
    });
  });
  entered.get_future().wait();

  std::future<void> body_fut = std::async(std::launch::async, std::move(body));
  const bool done = body_fut.wait_for(deadline) == std::future_status::ready;

  release.set_value();
  pin_thread.get();
  body_fut.get();  // drain before Store teardown and propagate operation failures
  return done;
}

// ── Cross-collection independence ────────────────────────────────────────────

TEST(StorePerCollection, PinnedAgentsDoNotBlockTaskReads) {
  Store s;
  int reads = 0;
  constexpr int kIterations = 100;
  const bool ok = progresses_while_pinned(
      s, Store::Collection::kAgents,
      [&] {
        for (int i = 0; i < kIterations; ++i) {
          auto out = s.list_all_tasks();
          EXPECT_TRUE(out["tasks"].empty());
          ++reads;
        }
      },
      std::chrono::seconds{2});
  EXPECT_TRUE(ok) << "task reads blocked while agents_ was exclusively locked";
  EXPECT_EQ(reads, kIterations);
}

TEST(StorePerCollection, PinnedTeamsDoNotBlockFaultReads) {
  Store s;
  int reads = 0;
  constexpr int kIterations = 100;
  const bool ok = progresses_while_pinned(
      s, Store::Collection::kTeams,
      [&] {
        for (int i = 0; i < kIterations; ++i) {
          auto out = s.list_faults();
          EXPECT_TRUE(out["faults"].empty());
          ++reads;
        }
      },
      std::chrono::seconds{2});
  EXPECT_TRUE(ok) << "fault reads blocked while teams_ was exclusively locked";
  EXPECT_EQ(reads, kIterations);
}

TEST(StorePerCollection, PinnedTasksDoNotBlockHmasReads) {
  Store s;
  int reads = 0;
  constexpr int kIterations = 100;
  const bool ok = progresses_while_pinned(
      s, Store::Collection::kTasks,
      [&] {
        for (int i = 0; i < kIterations; ++i) {
          auto out = s.list_hmas_tasks_by_layer(HmasLayer::L0_ChiefArchitect);
          EXPECT_TRUE(out.empty());
          ++reads;
        }
      },
      std::chrono::seconds{2});
  EXPECT_TRUE(ok) << "hmas reads blocked while tasks_ was exclusively locked";
  EXPECT_EQ(reads, kIterations);
}

TEST(StorePerCollection, PinnedAgentsDoNotBlockDurableFleetClaim) {
  auto github = std::make_shared<MockGitHubClient>();
  Store store(github);
  HmasTask task;
  task.id = "fleet-leaf";
  task.layer = HmasLayer::L3_TaskAgent;
  task.state = TaskState::Pending;
  const json claim = {
      {"schema", "hi/fleet/claim/v1"}, {"agentId", "worker-agent"}, {"generation", 1}};
  bool claimed = false;
  const bool progressed = progresses_while_pinned(
      store, Store::Collection::kAgents,
      [&] {
        // Cold HMAS hydration, creation, claim, and legacy fencing must all use
        // the HMAS collection. The unrelated agent lock remains held throughout.
        store.create_hmas_task(task);
        claimed = store.reserve_hmas_fleet_claim(task.id, claim);
        EXPECT_THROW(store.update_hmas_task_state(task.id, TaskState::Completed),
                     std::runtime_error);
      },
      std::chrono::seconds{2});
  EXPECT_TRUE(progressed);
  ASSERT_TRUE(claimed);
  const auto canonical = store.get_hmas_task(task.id);
  ASSERT_TRUE(canonical.has_value());
  EXPECT_EQ(canonical->fleet_claim, claim);
  EXPECT_EQ(canonical->state, TaskState::Delegated);
  ASSERT_EQ(github->updated_bodies.size(), 1u);
  EXPECT_NE(github->updated_bodies.begin()->second.find("worker-agent"), std::string::npos);
}

TEST(StorePerCollection, PinnedTasksDoNotBlockDurableBriefRegistration) {
  auto github = std::make_shared<ConditionalAuthority>();
  auto configuration = std::make_shared<IssueImportConfiguration>();
  configuration->state_branch = "fixture-state";
  Store store(github, configuration);
  TaskBrief brief;
  brief.id = "fleet-brief";
  brief.title = "durable brief with independent collection";
  const bool progressed = progresses_while_pinned(
      store, Store::Collection::kTasks,
      [&] {
        store.ensure_durable_task_brief(brief);
        const auto registered = store.get_task_brief(brief.id);
        ASSERT_TRUE(registered.has_value());
        EXPECT_EQ(registered->title, brief.title);
      },
      std::chrono::seconds{2});
  EXPECT_TRUE(progressed);
  ASSERT_EQ(github->created_issues.size(), 1u);
  EXPECT_EQ(github->created_issues.begin()->second["label"], "agamemnon-brief");
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
        (void)s.create_agent({{"name", "w" + std::to_string(i) + "_" + std::to_string(n++)}});
        writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  const auto start = agamemmon_clock::now();
  std::this_thread::sleep_for(kDuration);
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) t.join();
  const auto secs = std::chrono::duration<double>(agamemmon_clock::now() - start).count();
  std::cerr << "[per-collection] cross-collection reads=" << reads.load() << " ("
            << static_cast<long>(static_cast<double>(reads.load()) / secs) << "/s)"
            << " writes=" << writes.load() << " ("
            << static_cast<long>(static_cast<double>(writes.load()) / secs) << "/s)\n";
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
  const auto start = agamemmon_clock::now();
  std::this_thread::sleep_for(kDuration);
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) t.join();
  const auto secs = std::chrono::duration<double>(agamemmon_clock::now() - start).count();
  std::cerr << "[per-collection] intra-collection reads=" << reads.load() << " ("
            << static_cast<long>(static_cast<double>(reads.load()) / secs) << "/s)\n";
  EXPECT_GT(reads.load(), 0);
}

}  // namespace agamemnon::test
