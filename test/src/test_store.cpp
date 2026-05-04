#include "projectagamemnon/store.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Agents ────────────────────────────────────────────────────────────────────

TEST(StoreAgentTest, CreateAndGet) {
  Store s;
  json body = {{"name", "alpha"}, {"role", "worker"}};
  auto result = s.create_agent(body);
  std::string id = result["id"];
  ASSERT_FALSE(id.empty());

  auto agent = s.get_agent(id);
  EXPECT_EQ(agent["name"], "alpha");
  EXPECT_EQ(agent["role"], "worker");
  EXPECT_EQ(agent["status"], "offline");
}

TEST(StoreAgentTest, GetMissing) {
  Store s;
  auto agent = s.get_agent("nonexistent");
  EXPECT_TRUE(agent.is_null());
}

TEST(StoreAgentTest, GetByName) {
  Store s;
  s.create_agent({{"name", "beta"}});
  auto agent = s.get_agent_by_name("beta");
  EXPECT_EQ(agent["name"], "beta");
  EXPECT_TRUE(s.get_agent_by_name("missing").is_null());
}

TEST(StoreAgentTest, ListAgents) {
  Store s;
  s.create_agent({{"name", "a1"}});
  s.create_agent({{"name", "a2"}});
  auto result = s.list_agents();
  EXPECT_EQ(result["agents"].size(), 2u);
}

TEST(StoreAgentTest, UpdateAgent) {
  Store s;
  auto r = s.create_agent({{"name", "c"}});
  std::string id = r["id"];
  auto updated = s.update_agent(id, {{"role", "lead"}});
  EXPECT_EQ(updated["role"], "lead");
  EXPECT_EQ(updated["name"], "c");
}

TEST(StoreAgentTest, DeleteAgent) {
  Store s;
  auto r = s.create_agent({{"name", "d"}});
  std::string id = r["id"];
  EXPECT_TRUE(s.delete_agent(id));
  EXPECT_TRUE(s.get_agent(id).is_null());
  EXPECT_FALSE(s.delete_agent(id));
}

TEST(StoreAgentTest, StartStop) {
  Store s;
  auto r = s.create_agent({{"name", "e"}});
  std::string id = r["id"];

  auto started = s.start_agent(id);
  EXPECT_EQ(started["status"], "online");
  EXPECT_EQ(s.get_agent(id)["status"], "online");

  auto stopped = s.stop_agent(id);
  EXPECT_EQ(stopped["status"], "offline");
  EXPECT_EQ(s.get_agent(id)["status"], "offline");
}

// ── Teams ─────────────────────────────────────────────────────────────────────

TEST(StoreTeamTest, CreateAndGet) {
  Store s;
  auto r = s.create_team({{"name", "team-a"}});
  std::string id = r["team"]["id"];
  ASSERT_FALSE(id.empty());
  EXPECT_EQ(s.get_team(id)["name"], "team-a");
}

TEST(StoreTeamTest, ListTeams) {
  Store s;
  s.create_team({{"name", "t1"}});
  s.create_team({{"name", "t2"}});
  EXPECT_EQ(s.list_teams()["teams"].size(), 2u);
}

TEST(StoreTeamTest, UpdateTeam) {
  Store s;
  auto r = s.create_team({{"name", "old-name"}});
  std::string id = r["team"]["id"];
  s.update_team(id, {{"name", "new-name"}});
  EXPECT_EQ(s.get_team(id)["name"], "new-name");
}

TEST(StoreTeamTest, DeleteTeam) {
  Store s;
  auto r = s.create_team({{"name", "gone"}});
  std::string id = r["team"]["id"];
  EXPECT_TRUE(s.delete_team(id));
  EXPECT_TRUE(s.get_team(id).is_null());
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

TEST(StoreTaskTest, CreateAndGet) {
  Store s;
  auto team = s.create_team({{"name", "team"}});
  std::string tid = team["team"]["id"];

  auto r = s.create_task(tid, {{"subject", "do work"}});
  std::string task_id = r["task"]["id"];
  ASSERT_FALSE(task_id.empty());

  auto task = s.get_task(tid, task_id);
  EXPECT_EQ(task["subject"], "do work");
  EXPECT_EQ(task["status"], "pending");
}

TEST(StoreTaskTest, ListTasksForTeam) {
  Store s;
  auto team = s.create_team({{"name", "t"}});
  std::string tid = team["team"]["id"];
  s.create_task(tid, {{"subject", "s1"}});
  s.create_task(tid, {{"subject", "s2"}});
  EXPECT_EQ(s.list_tasks_for_team(tid)["tasks"].size(), 2u);
}

TEST(StoreTaskTest, ListAllTasks) {
  Store s;
  auto t1 = s.create_team({{"name", "t1"}})["team"]["id"];
  auto t2 = s.create_team({{"name", "t2"}})["team"]["id"];
  s.create_task(t1, {{"subject", "a"}});
  s.create_task(t2, {{"subject", "b"}});
  EXPECT_EQ(s.list_all_tasks()["tasks"].size(), 2u);
}

TEST(StoreTaskTest, MarkCompleted) {
  Store s;
  auto tid = s.create_team({{"name", "t"}})["team"]["id"];
  std::string task_id = s.create_task(tid, {{"subject", "x"}})["task"]["id"];
  s.mark_task_completed(task_id);
  auto task = s.get_task(tid, task_id);
  EXPECT_EQ(task["status"], "completed");
  EXPECT_FALSE(task["completedAt"].is_null());
}

// ── Chaos faults ──────────────────────────────────────────────────────────────

TEST(StoreFaultTest, CreateListRemove) {
  Store s;
  auto r = s.create_fault("latency");
  std::string id = r["fault"]["id"];
  ASSERT_FALSE(id.empty());

  EXPECT_EQ(s.list_faults()["faults"].size(), 1u);
  EXPECT_TRUE(s.remove_fault(id));
  EXPECT_EQ(s.list_faults()["faults"].size(), 0u);
  EXPECT_FALSE(s.remove_fault(id));
}

// ── Concurrency ───────────────────────────────────────────────────────────────

TEST(StoreConcurrencyTest, ConcurrentReadsDoNotDeadlock) {
  Store s;
  for (int i = 0; i < 10; ++i) {
    s.create_agent({{"name", "agent-" + std::to_string(i)}});
  }

  constexpr int kReaders = 16;
  constexpr int kIterations = 200;
  std::atomic<int> completed{0};

  auto reader = [&]() {
    for (int i = 0; i < kIterations; ++i) {
      auto result = s.list_agents();
      (void)result;
    }
    ++completed;
  };

  std::vector<std::thread> threads;
  threads.reserve(kReaders);
  for (int i = 0; i < kReaders; ++i) threads.emplace_back(reader);
  for (auto& t : threads) t.join();

  EXPECT_EQ(completed.load(), kReaders);
}

TEST(StoreConcurrencyTest, ConcurrentReadWriteNoRace) {
  Store s;
  std::atomic<bool> stop{false};
  std::atomic<int> write_count{0};

  auto writer = [&]() {
    int n = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      s.create_agent({{"name", "w-" + std::to_string(n++)}});
      ++write_count;
    }
  };

  auto reader = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto result = s.list_agents();
      (void)result;
    }
  };

  std::vector<std::thread> threads;
  threads.emplace_back(writer);
  for (int i = 0; i < 8; ++i) threads.emplace_back(reader);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_relaxed);
  for (auto& t : threads) t.join();

  EXPECT_GT(write_count.load(), 0);
}

}  // namespace projectagamemnon::test
