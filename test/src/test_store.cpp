#include "projectagamemnon/store.hpp"

#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Helpers ──────────────────────────────────────────────────────────────────

TEST(HelpersTest, GenerateUuidFormat) {
  std::string uuid = generate_uuid();
  // xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  std::regex re(R"([0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})");
  EXPECT_TRUE(std::regex_match(uuid, re)) << "UUID: " << uuid;
}

TEST(HelpersTest, GenerateUuidUnique) { EXPECT_NE(generate_uuid(), generate_uuid()); }

TEST(HelpersTest, NowIso8601Format) {
  std::string ts = now_iso8601();
  // Should look like 2024-01-15T12:34:56Z
  std::regex re(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
  EXPECT_TRUE(std::regex_match(ts, re)) << "Timestamp: " << ts;
}

// ── Agents ───────────────────────────────────────────────────────────────────

class StoreAgentTest : public ::testing::Test {
 protected:
  Store store;
};

TEST_F(StoreAgentTest, CreateAgentReturnsIdAndAgent) {
  json result = store.create_agent({{"name", "alpha"}, {"role", "worker"}});
  EXPECT_TRUE(result.contains("id"));
  EXPECT_TRUE(result.contains("agent"));
  EXPECT_EQ(result["agent"]["name"], "alpha");
  EXPECT_EQ(result["agent"]["status"], "offline");
}

TEST_F(StoreAgentTest, GetAgentById) {
  json result = store.create_agent({{"name", "beta"}});
  std::string id = result["id"];
  json agent = store.get_agent(id);
  EXPECT_FALSE(agent.is_null());
  EXPECT_EQ(agent["id"], id);
}

TEST_F(StoreAgentTest, GetAgentByIdNotFound) {
  json agent = store.get_agent("nonexistent-id");
  EXPECT_TRUE(agent.is_null());
}

TEST_F(StoreAgentTest, GetAgentByName) {
  store.create_agent({{"name", "gamma"}});
  json agent = store.get_agent_by_name("gamma");
  EXPECT_FALSE(agent.is_null());
  EXPECT_EQ(agent["name"], "gamma");
}

TEST_F(StoreAgentTest, GetAgentByNameNotFound) {
  json agent = store.get_agent_by_name("does-not-exist");
  EXPECT_TRUE(agent.is_null());
}

TEST_F(StoreAgentTest, ListAgentsEmpty) {
  json result = store.list_agents();
  EXPECT_TRUE(result.contains("agents"));
  EXPECT_EQ(result["agents"].size(), 0u);
}

TEST_F(StoreAgentTest, ListAgentsMultiple) {
  store.create_agent({{"name", "a1"}});
  store.create_agent({{"name", "a2"}});
  json result = store.list_agents();
  EXPECT_EQ(result["agents"].size(), 2u);
}

TEST_F(StoreAgentTest, UpdateAgent) {
  json result = store.create_agent({{"name", "delta"}});
  std::string id = result["id"];
  json updated = store.update_agent(id, {{"label", "my-label"}});
  EXPECT_FALSE(updated.is_null());
  EXPECT_EQ(updated["label"], "my-label");
}

TEST_F(StoreAgentTest, UpdateAgentNotFound) {
  json updated = store.update_agent("bad-id", {{"label", "x"}});
  EXPECT_TRUE(updated.is_null());
}

TEST_F(StoreAgentTest, UpdateAgentDoesNotOverwriteId) {
  json result = store.create_agent({{"name", "epsilon"}});
  std::string id = result["id"];
  store.update_agent(id, {{"id", "hacked"}});
  json agent = store.get_agent(id);
  EXPECT_EQ(agent["id"], id);
}

TEST_F(StoreAgentTest, DeleteAgent) {
  json result = store.create_agent({{"name", "zeta"}});
  std::string id = result["id"];
  EXPECT_TRUE(store.delete_agent(id));
  EXPECT_TRUE(store.get_agent(id).is_null());
}

TEST_F(StoreAgentTest, DeleteAgentNotFound) { EXPECT_FALSE(store.delete_agent("bad-id")); }

TEST_F(StoreAgentTest, StartAgent) {
  json result = store.create_agent({{"name", "eta"}});
  std::string id = result["id"];
  json status = store.start_agent(id);
  EXPECT_FALSE(status.is_null());
  EXPECT_EQ(status["status"], "online");
  EXPECT_EQ(store.get_agent(id)["status"], "online");
}

TEST_F(StoreAgentTest, StartAgentNotFound) {
  json result = store.start_agent("bad-id");
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreAgentTest, StopAgent) {
  json result = store.create_agent({{"name", "theta"}});
  std::string id = result["id"];
  store.start_agent(id);
  json status = store.stop_agent(id);
  EXPECT_FALSE(status.is_null());
  EXPECT_EQ(status["status"], "offline");
  EXPECT_EQ(store.get_agent(id)["status"], "offline");
}

TEST_F(StoreAgentTest, StopAgentNotFound) {
  json result = store.stop_agent("bad-id");
  EXPECT_TRUE(result.is_null());
}

// ── Teams ─────────────────────────────────────────────────────────────────────

class StoreTeamTest : public ::testing::Test {
 protected:
  Store store;
};

TEST_F(StoreTeamTest, CreateTeam) {
  json result = store.create_team({{"name", "team-alpha"}});
  EXPECT_TRUE(result.contains("team"));
  EXPECT_EQ(result["team"]["name"], "team-alpha");
}

TEST_F(StoreTeamTest, GetTeam) {
  json result = store.create_team({{"name", "team-beta"}});
  std::string id = result["team"]["id"];
  json team = store.get_team(id);
  EXPECT_FALSE(team.is_null());
  EXPECT_EQ(team["name"], "team-beta");
}

TEST_F(StoreTeamTest, GetTeamNotFound) {
  json team = store.get_team("bad-id");
  EXPECT_TRUE(team.is_null());
}

TEST_F(StoreTeamTest, ListTeamsEmpty) {
  json result = store.list_teams();
  EXPECT_TRUE(result.contains("teams"));
  EXPECT_EQ(result["teams"].size(), 0u);
}

TEST_F(StoreTeamTest, ListTeamsMultiple) {
  store.create_team({{"name", "t1"}});
  store.create_team({{"name", "t2"}});
  json result = store.list_teams();
  EXPECT_EQ(result["teams"].size(), 2u);
}

TEST_F(StoreTeamTest, UpdateTeamName) {
  json result = store.create_team({{"name", "old-name"}});
  std::string id = result["team"]["id"];
  json updated = store.update_team(id, {{"name", "new-name"}});
  EXPECT_EQ(updated["name"], "new-name");
}

TEST_F(StoreTeamTest, UpdateTeamAgentIds) {
  json result = store.create_team({{"name", "t"}});
  std::string id = result["team"]["id"];
  json ids = json::array({"agent-1", "agent-2"});
  store.update_team(id, {{"agentIds", ids}});
  EXPECT_EQ(store.get_team(id)["agentIds"], ids);
}

TEST_F(StoreTeamTest, UpdateTeamAgentIdsSnakeCase) {
  json result = store.create_team({{"name", "t"}});
  std::string id = result["team"]["id"];
  json ids = json::array({"agent-x"});
  store.update_team(id, {{"agent_ids", ids}});
  EXPECT_EQ(store.get_team(id)["agentIds"], ids);
}

TEST_F(StoreTeamTest, UpdateTeamNotFound) {
  json updated = store.update_team("bad-id", {{"name", "x"}});
  EXPECT_TRUE(updated.is_null());
}

TEST_F(StoreTeamTest, DeleteTeam) {
  json result = store.create_team({{"name", "bye"}});
  std::string id = result["team"]["id"];
  EXPECT_TRUE(store.delete_team(id));
  EXPECT_TRUE(store.get_team(id).is_null());
}

TEST_F(StoreTeamTest, DeleteTeamNotFound) { EXPECT_FALSE(store.delete_team("bad-id")); }

TEST_F(StoreTeamTest, CreateTeamWithAgentIds) {
  json result = store.create_team({{"name", "t"}, {"agentIds", json::array({"a1", "a2"})}});
  EXPECT_EQ(result["team"]["agentIds"].size(), 2u);
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

class StoreTaskTest : public ::testing::Test {
 protected:
  Store store;
  std::string team_id;

  void SetUp() override {
    json t = store.create_team({{"name", "task-team"}});
    team_id = t["team"]["id"];
  }
};

TEST_F(StoreTaskTest, CreateTask) {
  json result = store.create_task(team_id, {{"subject", "do something"}});
  EXPECT_TRUE(result.contains("task"));
  EXPECT_EQ(result["task"]["subject"], "do something");
  EXPECT_EQ(result["task"]["status"], "pending");
  EXPECT_EQ(result["task"]["teamId"], team_id);
}

TEST_F(StoreTaskTest, GetTask) {
  json result = store.create_task(team_id, {{"subject", "get me"}});
  std::string task_id = result["task"]["id"];
  json task = store.get_task(team_id, task_id);
  EXPECT_FALSE(task.is_null());
  EXPECT_EQ(task["id"], task_id);
}

TEST_F(StoreTaskTest, GetTaskWrongTeam) {
  json result = store.create_task(team_id, {{"subject", "x"}});
  std::string task_id = result["task"]["id"];
  json task = store.get_task("other-team", task_id);
  EXPECT_TRUE(task.is_null());
}

TEST_F(StoreTaskTest, GetTaskEmptyTeamId) {
  json result = store.create_task(team_id, {{"subject", "x"}});
  std::string task_id = result["task"]["id"];
  // #222: empty team_id is not a valid wildcard — must return null to prevent
  // cross-team access. Callers must provide the owning team_id.
  json task = store.get_task("", task_id);
  EXPECT_TRUE(task.is_null());
}

TEST_F(StoreTaskTest, GetTaskNotFound) {
  json task = store.get_task(team_id, "bad-id");
  EXPECT_TRUE(task.is_null());
}

TEST_F(StoreTaskTest, ListTasksForTeamEmpty) {
  json result = store.list_tasks_for_team(team_id);
  EXPECT_TRUE(result.contains("tasks"));
  EXPECT_EQ(result["tasks"].size(), 0u);
}

TEST_F(StoreTaskTest, ListTasksForTeam) {
  store.create_task(team_id, {{"subject", "t1"}});
  store.create_task(team_id, {{"subject", "t2"}});
  json result = store.list_tasks_for_team(team_id);
  EXPECT_EQ(result["tasks"].size(), 2u);
}

TEST_F(StoreTaskTest, ListAllTasks) {
  std::string team2_id = store.create_team({{"name", "t2"}})["team"]["id"];
  store.create_task(team_id, {{"subject", "a"}});
  store.create_task(team2_id, {{"subject", "b"}});
  json result = store.list_all_tasks();
  EXPECT_EQ(result["tasks"].size(), 2u);
}

TEST_F(StoreTaskTest, UpdateTask) {
  json result = store.create_task(team_id, {{"subject", "orig"}});
  std::string task_id = result["task"]["id"];
  json updated = store.update_task(team_id, task_id, {{"subject", "updated"}});
  EXPECT_EQ(updated["subject"], "updated");
}

TEST_F(StoreTaskTest, UpdateTaskNotFound) {
  json updated = store.update_task(team_id, "bad-id", {{"subject", "x"}});
  EXPECT_TRUE(updated.is_null());
}

TEST_F(StoreTaskTest, UpdateTaskSetsCompletedAt) {
  json result = store.create_task(team_id, {{"subject", "x"}});
  std::string task_id = result["task"]["id"];
  json updated = store.update_task(team_id, task_id, {{"status", "completed"}});
  EXPECT_EQ(updated["status"], "completed");
  EXPECT_FALSE(updated["completedAt"].is_null());
}

TEST_F(StoreTaskTest, MarkTaskCompleted) {
  json result = store.create_task(team_id, {{"subject", "mark me"}});
  std::string task_id = result["task"]["id"];
  store.mark_task_completed(task_id);
  json task = store.get_task(team_id, task_id);
  EXPECT_EQ(task["status"], "completed");
  EXPECT_FALSE(task["completedAt"].is_null());
}

TEST_F(StoreTaskTest, MarkTaskCompletedNonexistentNoThrow) {
  EXPECT_NO_THROW(store.mark_task_completed("bad-id"));
}

// ── Chaos faults ──────────────────────────────────────────────────────────────

class StoreFaultTest : public ::testing::Test {
 protected:
  Store store;
};

TEST_F(StoreFaultTest, ListFaultsEmpty) {
  json result = store.list_faults();
  EXPECT_TRUE(result.contains("faults"));
  EXPECT_EQ(result["faults"].size(), 0u);
}

TEST_F(StoreFaultTest, CreateFault) {
  json result = store.create_fault("latency");
  EXPECT_TRUE(result.contains("fault"));
  EXPECT_EQ(result["fault"]["type"], "latency");
  EXPECT_TRUE(result["fault"]["active"]);
}

TEST_F(StoreFaultTest, ListFaultsAfterCreate) {
  store.create_fault("packet-loss");
  store.create_fault("timeout");
  json result = store.list_faults();
  EXPECT_EQ(result["faults"].size(), 2u);
}

TEST_F(StoreFaultTest, RemoveFault) {
  json result = store.create_fault("crash");
  std::string id = result["fault"]["id"];
  EXPECT_TRUE(store.remove_fault(id));
  EXPECT_EQ(store.list_faults()["faults"].size(), 0u);
}

TEST_F(StoreFaultTest, RemoveFaultNotFound) { EXPECT_FALSE(store.remove_fault("bad-id")); }

// ── HMAS tasks ────────────────────────────────────────────────────────────────

class StoreHmasTest : public ::testing::Test {
 protected:
  Store store;
};

TEST_F(StoreHmasTest, GetHmasTaskReturnsOptional) {
  HmasTask task;
  task.id = "hmas-1";
  task.brief_id = "brief-1";
  task.layer = HmasLayer::L1_ComponentLead;
  task.state = TaskState::Pending;
  store.create_hmas_task(task);

  auto result = store.get_hmas_task("hmas-1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->id, "hmas-1");
  EXPECT_EQ(result->state, TaskState::Pending);
}

TEST_F(StoreHmasTest, GetHmasTaskNotFound) {
  auto result = store.get_hmas_task("nonexistent");
  EXPECT_FALSE(result.has_value());
}

TEST_F(StoreHmasTest, UpdateHmasTaskStateAndRecordEscalation) {
  HmasTask task;
  task.id = "hmas-2";
  task.brief_id = "brief-2";
  task.layer = HmasLayer::L2_ModuleLead;
  task.state = TaskState::Pending;
  store.create_hmas_task(task);

  EscalationRecord rec;
  rec.task_id = "hmas-2";
  rec.reason = "timeout";
  rec.escalated_at = now_iso8601();
  rec.from_layer = HmasLayer::L2_ModuleLead;

  bool ok = store.update_hmas_task_state_and_record_escalation("hmas-2", TaskState::Escalated, rec);
  EXPECT_TRUE(ok);

  auto updated = store.get_hmas_task("hmas-2");
  ASSERT_TRUE(updated.has_value());
  EXPECT_EQ(updated->state, TaskState::Escalated);
  ASSERT_EQ(updated->escalations.size(), 1u);
  EXPECT_EQ(updated->escalations[0].reason, "timeout");
}

// #155 — concurrent stress test: 4 threads simultaneously get and update the same HmasTask.
TEST_F(StoreHmasTest, HmasTaskConcurrentGetAndUpdate) {
  HmasTask task;
  task.id = "stress-1";
  task.brief_id = "brief-stress";
  task.layer = HmasLayer::L3_TaskAgent;
  task.state = TaskState::Pending;
  store.create_hmas_task(task);

  constexpr int kThreads = 4;
  constexpr int kIterations = 100;
  std::vector<std::thread> threads;
  threads.reserve(kThreads * 2);

  // 4 reader threads
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        auto got = store.get_hmas_task("stress-1");
        // The task always exists; we just verify no data race or crash.
        (void)got;
      }
    });
  }

  // 4 writer threads cycling through states
  const TaskState states[] = {TaskState::InProgress, TaskState::Delegated, TaskState::Pending,
                              TaskState::Completed};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < kIterations; ++i) {
        store.update_hmas_task_state("stress-1", states[t % 4]);
      }
    });
  }

  for (auto& th : threads) th.join();

  // After all updates, the task must still be accessible with no crash.
  auto final_task = store.get_hmas_task("stress-1");
  EXPECT_TRUE(final_task.has_value());
}

}  // namespace projectagamemnon::test
