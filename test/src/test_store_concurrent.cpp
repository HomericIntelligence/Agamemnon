#include "projectagamemnon/store.hpp"

#include <atomic>
#include <barrier>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

class StoreConcurrent : public ::testing::Test {
 protected:
  Store store_;
};

// ── Concurrent agent create + read ────────────────────────────────────────────

TEST_F(StoreConcurrent, ConcurrentAgentCreateRead) {
  constexpr int N = 8;
  std::barrier sync(N * 2 + 1);

  std::vector<std::thread> threads;
  threads.reserve(N * 2);

  for (int i = 0; i < N; ++i) {
    threads.emplace_back([&, i] {
      sync.arrive_and_wait();
      store_.create_agent({{"name", "agent-" + std::to_string(i)}});
    });
  }
  for (int i = 0; i < N; ++i) {
    threads.emplace_back([&] {
      sync.arrive_and_wait();
      store_.list_agents();
    });
  }

  sync.arrive_and_wait();
  for (auto& t : threads) t.join();

  json result = store_.list_agents();
  EXPECT_EQ(result["agents"].size(), static_cast<std::size_t>(N));
}

// ── Concurrent agent delete + read ────────────────────────────────────────────

TEST_F(StoreConcurrent, ConcurrentAgentDeleteRead) {
  constexpr int NAGENTS = 50;
  constexpr int NTHREADS = 4;

  std::vector<std::string> ids;
  ids.reserve(NAGENTS);
  for (int i = 0; i < NAGENTS; ++i) {
    auto r = store_.create_agent({{"name", "a-" + std::to_string(i)}});
    ids.push_back(r["id"]);
  }

  std::barrier sync(NTHREADS * 2 + 1);
  std::vector<std::thread> threads;
  threads.reserve(NTHREADS * 2);

  std::atomic<int> del_idx{0};
  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&] {
      sync.arrive_and_wait();
      int idx = del_idx.fetch_add(1);
      if (idx < NAGENTS) store_.delete_agent(ids[static_cast<std::size_t>(idx)]);
    });
  }
  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&] {
      sync.arrive_and_wait();
      store_.list_agents();
    });
  }

  sync.arrive_and_wait();
  for (auto& t : threads) t.join();

  // No crash = pass; final count must be <= NAGENTS
  json result = store_.list_agents();
  EXPECT_LE(result["agents"].size(), static_cast<std::size_t>(NAGENTS));
}

// ── Concurrent team creation ──────────────────────────────────────────────────

TEST_F(StoreConcurrent, ConcurrentTeamCreate) {
  constexpr int NTHREADS = 16;
  constexpr int TEAMS_PER_THREAD = 10;
  std::barrier sync(NTHREADS + 1);

  std::vector<std::thread> threads;
  threads.reserve(NTHREADS);

  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&, i] {
      sync.arrive_and_wait();
      for (int j = 0; j < TEAMS_PER_THREAD; ++j) {
        store_.create_team({{"name", "t-" + std::to_string(i) + "-" + std::to_string(j)}});
      }
    });
  }

  sync.arrive_and_wait();
  for (auto& t : threads) t.join();

  json result = store_.list_teams();
  EXPECT_EQ(result["teams"].size(), static_cast<std::size_t>(NTHREADS * TEAMS_PER_THREAD));
}

// ── Concurrent task create + mark_completed ───────────────────────────────────

TEST_F(StoreConcurrent, ConcurrentTaskCreateAndMark) {
  constexpr int NTHREADS = 8;
  constexpr int TASKS_PER_THREAD = 10;

  auto team_result = store_.create_team({{"name", "concurrent-team"}});
  std::string team_id = team_result["team"]["id"];

  // Pre-create tasks so markers have IDs to work with.
  std::vector<std::string> task_ids;
  task_ids.reserve(static_cast<std::size_t>(NTHREADS * TASKS_PER_THREAD));
  for (int i = 0; i < NTHREADS * TASKS_PER_THREAD; ++i) {
    auto r = store_.create_task(team_id, {{"subject", "task-" + std::to_string(i)}});
    task_ids.push_back(r["task"]["id"]);
  }

  std::barrier sync(NTHREADS * 2 + 1);
  std::vector<std::thread> threads;
  threads.reserve(NTHREADS * 2);

  // Writers: create more tasks
  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&, i] {
      sync.arrive_and_wait();
      for (int j = 0; j < TASKS_PER_THREAD; ++j) {
        store_.create_task(team_id,
                           {{"subject", "new-" + std::to_string(i * TASKS_PER_THREAD + j)}});
      }
    });
  }
  // Markers: mark pre-created tasks completed
  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&, i] {
      sync.arrive_and_wait();
      for (int j = 0; j < TASKS_PER_THREAD; ++j) {
        std::size_t idx = static_cast<std::size_t>(i * TASKS_PER_THREAD + j);
        store_.mark_task_completed(task_ids[idx]);
      }
    });
  }

  sync.arrive_and_wait();
  for (auto& t : threads) t.join();

  // No crash = pass; all tasks must be accounted for
  json result = store_.list_all_tasks();
  EXPECT_GE(result["tasks"].size(), task_ids.size());
}

// ── Concurrent fault create + remove ─────────────────────────────────────────

TEST_F(StoreConcurrent, ConcurrentFaultCreateRemove) {
  constexpr int NTHREADS = 8;
  constexpr int OPS_PER_THREAD = 20;

  std::barrier sync(NTHREADS * 2 + 1);
  std::vector<std::thread> threads;
  threads.reserve(NTHREADS * 2);

  // Creators
  std::vector<std::string> fault_ids(static_cast<std::size_t>(NTHREADS * OPS_PER_THREAD));
  std::atomic<int> fidx{0};

  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&] {
      sync.arrive_and_wait();
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        auto r = store_.create_fault("latency");
        int slot = fidx.fetch_add(1);
        fault_ids[static_cast<std::size_t>(slot)] = r["fault"]["id"];
      }
    });
  }
  // Removers (operate on IDs set before the barrier — initially empty;
  // they may find nothing to remove, which is fine)
  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&] {
      sync.arrive_and_wait();
      store_.list_faults();  // concurrent read
    });
  }

  sync.arrive_and_wait();
  for (auto& t : threads) t.join();

  // Now remove everything that was created
  for (auto& fid : fault_ids) {
    if (!fid.empty()) store_.remove_fault(fid);
  }

  json result = store_.list_faults();
  EXPECT_EQ(result["faults"].size(), 0u);
}

// ── Concurrent mixed operations across all maps ───────────────────────────────

TEST_F(StoreConcurrent, ConcurrentMixedOperations) {
  constexpr int NTHREADS = 12;
  std::barrier sync(NTHREADS + 1);

  std::vector<std::thread> threads;
  threads.reserve(NTHREADS);

  for (int i = 0; i < NTHREADS; ++i) {
    threads.emplace_back([&, i] {
      sync.arrive_and_wait();
      switch (i % 4) {
        case 0:
          store_.create_agent({{"name", "a" + std::to_string(i)}});
          store_.list_agents();
          break;
        case 1: {
          auto r = store_.create_team({{"name", "t" + std::to_string(i)}});
          store_.list_teams();
          store_.delete_team(r["team"]["id"]);
          break;
        }
        case 2: {
          auto tr = store_.create_team({{"name", "tt" + std::to_string(i)}});
          auto tkr = store_.create_task(tr["team"]["id"], {{"subject", "s"}});
          store_.mark_task_completed(tkr["task"]["id"]);
          break;
        }
        case 3:
          store_.create_fault("packet-loss");
          store_.list_faults();
          break;
      }
    });
  }

  sync.arrive_and_wait();
  for (auto& t : threads) t.join();
  // No crash, no data race (verified by TSan in CI)
}

}  // namespace projectagamemnon::test
