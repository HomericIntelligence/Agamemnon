#include "projectagamemnon/store.hpp"

#include <algorithm>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Helpers ──────────────────────────────────────────────────────────────────

TEST(HelpersTest, GenerateUuidFormat) {
  const std::string uuid = generate_uuid();
  ASSERT_EQ(uuid.size(), 36u);
  const std::regex pattern(
      R"([0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})");
  EXPECT_TRUE(std::regex_match(uuid, pattern));
}

TEST(HelpersTest, GenerateUuidUnique) {
  std::set<std::string> ids;
  for (int i = 0; i < 1000; ++i) ids.insert(generate_uuid());
  EXPECT_EQ(ids.size(), 1000u);
}

TEST(HelpersTest, NowIso8601Format) {
  const std::string ts = now_iso8601();
  EXPECT_FALSE(ts.empty());
  const std::regex pattern(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
  EXPECT_TRUE(std::regex_match(ts, pattern));
}

// ── Fixture ──────────────────────────────────────────────────────────────────

class StoreTest : public ::testing::Test {
 protected:
  Store store_;
};

// ── Agent CRUD ───────────────────────────────────────────────────────────────

TEST_F(StoreTest, CreateAgentFullBody) {
  json body = {{"name", "agent1"},          {"role", "architect"},
               {"host", "worker-1"},        {"label", "lbl"},
               {"program", "prog"},         {"workingDirectory", "/tmp"},
               {"programArgs", {"--flag"}}, {"tags", {"tagA"}}};
  json result = store_.create_agent(body);
  ASSERT_FALSE(result.is_null());
  ASSERT_TRUE(result.contains("id"));
  ASSERT_TRUE(result.contains("agent"));
  const auto& agent = result["agent"];
  EXPECT_EQ(agent["name"], "agent1");
  EXPECT_EQ(agent["role"], "architect");
  EXPECT_EQ(agent["host"], "worker-1");
  EXPECT_EQ(agent["status"], "offline");
  EXPECT_FALSE(agent["createdAt"].get<std::string>().empty());
}

TEST_F(StoreTest, CreateAgentDefaults) {
  json result = store_.create_agent(json::object());
  const auto& agent = result["agent"];
  EXPECT_EQ(agent["name"], "unnamed");
  EXPECT_EQ(agent["role"], "worker");
  EXPECT_EQ(agent["host"], "local");
  EXPECT_EQ(agent["status"], "offline");
}

TEST_F(StoreTest, GetAgentFound) {
  json result = store_.create_agent({{"name", "fetch-me"}});
  std::string id = result["id"];
  json agent = store_.get_agent(id);
  ASSERT_FALSE(agent.is_null());
  EXPECT_EQ(agent["id"], id);
  EXPECT_EQ(agent["name"], "fetch-me");
}

TEST_F(StoreTest, GetAgentNotFound) {
  json agent = store_.get_agent("nonexistent-id");
  EXPECT_TRUE(agent.is_null());
}

TEST_F(StoreTest, GetAgentByNameFound) {
  store_.create_agent({{"name", "named-agent"}});
  json agent = store_.get_agent_by_name("named-agent");
  ASSERT_FALSE(agent.is_null());
  EXPECT_EQ(agent["name"], "named-agent");
}

TEST_F(StoreTest, GetAgentByNameNotFound) {
  json agent = store_.get_agent_by_name("ghost");
  EXPECT_TRUE(agent.is_null());
}

TEST_F(StoreTest, ListAgentsCount) {
  store_.create_agent({{"name", "a1"}});
  store_.create_agent({{"name", "a2"}});
  store_.create_agent({{"name", "a3"}});
  json result = store_.list_agents();
  ASSERT_TRUE(result.contains("agents"));
  EXPECT_EQ(result["agents"].size(), 3u);
}

TEST_F(StoreTest, ListAgentsEmpty) {
  json result = store_.list_agents();
  ASSERT_TRUE(result.contains("agents"));
  EXPECT_TRUE(result["agents"].empty());
}

TEST_F(StoreTest, UpdateAgentMergesFields) {
  std::string id = store_.create_agent({{"name", "original"}, {"role", "worker"}})["id"];
  json updated = store_.update_agent(id, {{"name", "renamed"}, {"role", "lead"}});
  ASSERT_FALSE(updated.is_null());
  EXPECT_EQ(updated["name"], "renamed");
  EXPECT_EQ(updated["role"], "lead");
  EXPECT_EQ(updated["id"], id);  // id preserved
}

TEST_F(StoreTest, UpdateAgentPreservesId) {
  std::string id = store_.create_agent({{"name", "stable"}})["id"];
  store_.update_agent(id, {{"id", "hacked-id"}});
  json agent = store_.get_agent(id);
  EXPECT_EQ(agent["id"], id);
}

TEST_F(StoreTest, UpdateAgentNotFound) {
  json result = store_.update_agent("no-such-id", {{"name", "x"}});
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreTest, DeleteAgentSuccess) {
  std::string id = store_.create_agent({{"name", "doomed"}})["id"];
  EXPECT_TRUE(store_.delete_agent(id));
  EXPECT_TRUE(store_.get_agent(id).is_null());
}

TEST_F(StoreTest, DeleteAgentNotFound) { EXPECT_FALSE(store_.delete_agent("phantom")); }

TEST_F(StoreTest, StartAgentSetsOnline) {
  std::string id = store_.create_agent({{"name", "sleeper"}})["id"];
  json result = store_.start_agent(id);
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result["status"], "online");
  EXPECT_EQ(result["id"], id);
  EXPECT_EQ(store_.get_agent(id)["status"], "online");
}

TEST_F(StoreTest, StartAgentNotFound) { EXPECT_TRUE(store_.start_agent("ghost").is_null()); }

TEST_F(StoreTest, StopAgentSetsOffline) {
  std::string id = store_.create_agent({{"name", "runner"}})["id"];
  store_.start_agent(id);
  json result = store_.stop_agent(id);
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result["status"], "offline");
  EXPECT_EQ(store_.get_agent(id)["status"], "offline");
}

TEST_F(StoreTest, StopAgentNotFound) { EXPECT_TRUE(store_.stop_agent("ghost").is_null()); }

// ── Team CRUD ────────────────────────────────────────────────────────────────

TEST_F(StoreTest, CreateTeamPopulatesFields) {
  json result = store_.create_team({{"name", "alpha"}, {"agentIds", {"id1", "id2"}}});
  ASSERT_TRUE(result.contains("team"));
  const auto& team = result["team"];
  EXPECT_FALSE(team["id"].get<std::string>().empty());
  EXPECT_EQ(team["name"], "alpha");
  EXPECT_EQ(team["agentIds"].size(), 2u);
  EXPECT_FALSE(team["createdAt"].get<std::string>().empty());
}

TEST_F(StoreTest, CreateTeamWithAgentIdsKey) {
  json result = store_.create_team({{"name", "beta"}, {"agent_ids", {"x", "y"}}});
  EXPECT_EQ(result["team"]["agentIds"].size(), 2u);
}

TEST_F(StoreTest, GetTeamFound) {
  std::string id = store_.create_team({{"name", "gamma"}})["team"]["id"];
  json team = store_.get_team(id);
  ASSERT_FALSE(team.is_null());
  EXPECT_EQ(team["id"], id);
}

TEST_F(StoreTest, GetTeamNotFound) { EXPECT_TRUE(store_.get_team("no-team").is_null()); }

TEST_F(StoreTest, ListTeams) {
  store_.create_team({{"name", "t1"}});
  store_.create_team({{"name", "t2"}});
  json result = store_.list_teams();
  ASSERT_TRUE(result.contains("teams"));
  EXPECT_EQ(result["teams"].size(), 2u);
}

TEST_F(StoreTest, UpdateTeamName) {
  std::string id = store_.create_team({{"name", "old-name"}})["team"]["id"];
  json result = store_.update_team(id, {{"name", "new-name"}});
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result["name"], "new-name");
}

TEST_F(StoreTest, UpdateTeamAgentIds) {
  std::string id = store_.create_team({{"name", "team"}})["team"]["id"];
  store_.update_team(id, {{"agentIds", {"a", "b", "c"}}});
  json team = store_.get_team(id);
  EXPECT_EQ(team["agentIds"].size(), 3u);
}

TEST_F(StoreTest, UpdateTeamNotFound) {
  EXPECT_TRUE(store_.update_team("missing", {{"name", "x"}}).is_null());
}

TEST_F(StoreTest, DeleteTeamSuccess) {
  std::string id = store_.create_team({{"name", "temp"}})["team"]["id"];
  EXPECT_TRUE(store_.delete_team(id));
  EXPECT_TRUE(store_.get_team(id).is_null());
}

TEST_F(StoreTest, DeleteTeamNotFound) { EXPECT_FALSE(store_.delete_team("gone")); }

// ── Task CRUD ────────────────────────────────────────────────────────────────

TEST_F(StoreTest, CreateTaskDefaultStatus) {
  std::string team_id = store_.create_team({{"name", "t"}})["team"]["id"];
  json result = store_.create_task(team_id, {{"subject", "do work"}, {"type", "build"}});
  ASSERT_TRUE(result.contains("task"));
  const auto& task = result["task"];
  EXPECT_EQ(task["status"], "pending");
  EXPECT_EQ(task["teamId"], team_id);
  EXPECT_TRUE(task["completedAt"].is_null());
  EXPECT_EQ(task["subject"], "do work");
  EXPECT_EQ(task["type"], "build");
}

TEST_F(StoreTest, GetTaskByTeamAndId) {
  std::string team_id = store_.create_team({{"name", "t"}})["team"]["id"];
  std::string task_id = store_.create_task(team_id, {{"subject", "x"}})["task"]["id"];
  json task = store_.get_task(team_id, task_id);
  ASSERT_FALSE(task.is_null());
  EXPECT_EQ(task["id"], task_id);
}

TEST_F(StoreTest, GetTaskTeamIdMismatch) {
  std::string team_id = store_.create_team({{"name", "t"}})["team"]["id"];
  std::string task_id = store_.create_task(team_id, json::object())["task"]["id"];
  json task = store_.get_task("wrong-team", task_id);
  EXPECT_TRUE(task.is_null());
}

TEST_F(StoreTest, GetTaskNotFound) { EXPECT_TRUE(store_.get_task("team", "no-task").is_null()); }

TEST_F(StoreTest, UpdateTaskMergesFields) {
  std::string team_id = store_.create_team({{"name", "t"}})["team"]["id"];
  std::string task_id = store_.create_task(team_id, {{"subject", "old"}})["task"]["id"];
  json result =
      store_.update_task(team_id, task_id, {{"subject", "new"}, {"status", "in_progress"}});
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result["subject"], "new");
  EXPECT_EQ(result["status"], "in_progress");
  EXPECT_EQ(result["id"], task_id);
}

TEST_F(StoreTest, UpdateTaskCompletionSetsCompletedAt) {
  std::string team_id = store_.create_team({{"name", "t"}})["team"]["id"];
  std::string task_id = store_.create_task(team_id, json::object())["task"]["id"];
  json result = store_.update_task(team_id, task_id, {{"status", "completed"}});
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result["status"], "completed");
  EXPECT_FALSE(result["completedAt"].is_null());
}

TEST_F(StoreTest, UpdateTaskNotFound) {
  EXPECT_TRUE(store_.update_task("t", "missing", json::object()).is_null());
}

TEST_F(StoreTest, ListTasksForTeam) {
  std::string team_id = store_.create_team({{"name", "t"}})["team"]["id"];
  store_.create_task(team_id, {{"subject", "t1"}});
  store_.create_task(team_id, {{"subject", "t2"}});
  json result = store_.list_tasks_for_team(team_id);
  ASSERT_TRUE(result.contains("tasks"));
  EXPECT_EQ(result["tasks"].size(), 2u);
}

TEST_F(StoreTest, ListTasksForTeamEmpty) {
  json result = store_.list_tasks_for_team("nonexistent-team");
  ASSERT_TRUE(result.contains("tasks"));
  EXPECT_TRUE(result["tasks"].empty());
}

TEST_F(StoreTest, ListAllTasksAcrossTeams) {
  std::string t1 = store_.create_team({{"name", "t1"}})["team"]["id"];
  std::string t2 = store_.create_team({{"name", "t2"}})["team"]["id"];
  store_.create_task(t1, json::object());
  store_.create_task(t2, json::object());
  store_.create_task(t2, json::object());
  json result = store_.list_all_tasks();
  ASSERT_TRUE(result.contains("tasks"));
  EXPECT_EQ(result["tasks"].size(), 3u);
}

TEST_F(StoreTest, MarkTaskCompleted) {
  std::string team_id = store_.create_team({{"name", "t"}})["team"]["id"];
  std::string task_id = store_.create_task(team_id, json::object())["task"]["id"];
  store_.mark_task_completed(task_id);
  json task = store_.get_task(team_id, task_id);
  EXPECT_EQ(task["status"], "completed");
  EXPECT_FALSE(task["completedAt"].is_null());
}

TEST_F(StoreTest, MarkTaskCompletedUnknownIsNoop) {
  // Should not crash or throw
  EXPECT_NO_THROW(store_.mark_task_completed("no-such-task"));
}

// ── Chaos faults ─────────────────────────────────────────────────────────────

TEST_F(StoreTest, CreateFaultFields) {
  json result = store_.create_fault("latency");
  ASSERT_TRUE(result.contains("fault"));
  const auto& fault = result["fault"];
  EXPECT_EQ(fault["type"], "latency");
  EXPECT_TRUE(fault["active"].get<bool>());
  EXPECT_FALSE(fault["id"].get<std::string>().empty());
  EXPECT_FALSE(fault["createdAt"].get<std::string>().empty());
}

TEST_F(StoreTest, ListFaults) {
  store_.create_fault("latency");
  store_.create_fault("packet-loss");
  json result = store_.list_faults();
  ASSERT_TRUE(result.contains("faults"));
  EXPECT_EQ(result["faults"].size(), 2u);
}

TEST_F(StoreTest, RemoveFaultSuccess) {
  std::string id = store_.create_fault("error-rate")["fault"]["id"];
  EXPECT_TRUE(store_.remove_fault(id));
  EXPECT_EQ(store_.list_faults()["faults"].size(), 0u);
}

TEST_F(StoreTest, RemoveFaultNotFound) { EXPECT_FALSE(store_.remove_fault("not-there")); }

// ── Thread safety ─────────────────────────────────────────────────────────────

TEST_F(StoreTest, ConcurrentCreateAgentsAllPersist) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 100;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([this, i] {
      for (int j = 0; j < kPerThread; ++j) {
        store_.create_agent({{"name", "t" + std::to_string(i) + "_" + std::to_string(j)}});
      }
    });
  }
  for (auto& t : threads) t.join();

  json result = store_.list_agents();
  EXPECT_EQ(result["agents"].size(), static_cast<std::size_t>(kThreads * kPerThread));
}

}  // namespace projectagamemnon::test
