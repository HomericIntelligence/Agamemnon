#include "projectagamemnon/store.hpp"

#include <regex>
#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

class StoreEdgeCases : public ::testing::Test {
 protected:
  Store store_;
};

// ── Empty / missing IDs ───────────────────────────────────────────────────────

TEST_F(StoreEdgeCases, GetAgentEmptyId) {
  json result = store_.get_agent("");
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, GetTeamEmptyId) {
  json result = store_.get_team("");
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, GetTaskEmptyIds) {
  json result = store_.get_task("", "");
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, GetTaskEmptyTaskId) {
  auto team_result = store_.create_team({{"name", "alpha"}});
  std::string team_id = team_result["team"]["id"];
  json result = store_.get_task(team_id, "");
  EXPECT_TRUE(result.is_null());
}

// ── Delete non-existent ───────────────────────────────────────────────────────

TEST_F(StoreEdgeCases, DeleteNonExistentAgent) {
  EXPECT_FALSE(store_.delete_agent("does-not-exist"));
}

TEST_F(StoreEdgeCases, DeleteNonExistentTeam) {
  EXPECT_FALSE(store_.delete_team("does-not-exist"));
}

TEST_F(StoreEdgeCases, DeleteNonExistentFault) {
  EXPECT_FALSE(store_.remove_fault("does-not-exist"));
}

// ── Create with missing / blank fields ────────────────────────────────────────

TEST_F(StoreEdgeCases, CreateAgentMissingFields) {
  json result = store_.create_agent(json::object());
  ASSERT_TRUE(result.contains("id"));
  ASSERT_TRUE(result.contains("agent"));
  EXPECT_FALSE(result["id"].get<std::string>().empty());
  EXPECT_EQ(result["agent"]["name"], "unnamed");
  EXPECT_EQ(result["agent"]["role"], "worker");
  EXPECT_EQ(result["agent"]["status"], "offline");
}

TEST_F(StoreEdgeCases, CreateAgentBlankName) {
  json result = store_.create_agent({{"name", ""}});
  ASSERT_TRUE(result.contains("agent"));
  EXPECT_EQ(result["agent"]["name"], "");
}

TEST_F(StoreEdgeCases, CreateTeamMissingFields) {
  json result = store_.create_team(json::object());
  ASSERT_TRUE(result.contains("team"));
  EXPECT_EQ(result["team"]["name"], "unnamed-team");
  EXPECT_TRUE(result["team"]["agentIds"].is_array());
}

TEST_F(StoreEdgeCases, CreateTaskMissingSubject) {
  auto team_result = store_.create_team({{"name", "t"}});
  std::string team_id = team_result["team"]["id"];
  json result = store_.create_task(team_id, json::object());
  ASSERT_TRUE(result.contains("task"));
  EXPECT_EQ(result["task"]["subject"], "");
  EXPECT_EQ(result["task"]["status"], "pending");
}

// ── Update non-existent ───────────────────────────────────────────────────────

TEST_F(StoreEdgeCases, UpdateNonExistentAgent) {
  json result = store_.update_agent("ghost", {{"name", "x"}});
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, UpdateNonExistentTeam) {
  json result = store_.update_team("ghost", {{"name", "x"}});
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, UpdateNonExistentTask) {
  json result = store_.update_task("ghost-team", "ghost-task", {});
  EXPECT_TRUE(result.is_null());
}

// ── start/stop non-existent ───────────────────────────────────────────────────

TEST_F(StoreEdgeCases, StartNonExistentAgent) {
  json result = store_.start_agent("ghost");
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, StopNonExistentAgent) {
  json result = store_.stop_agent("ghost");
  EXPECT_TRUE(result.is_null());
}

// ── mark_task_completed on unknown ID ─────────────────────────────────────────

TEST_F(StoreEdgeCases, MarkTaskCompletedUnknownId) {
  // Must not throw or crash.
  EXPECT_NO_THROW(store_.mark_task_completed("ghost"));
}

// ── List on empty store ───────────────────────────────────────────────────────

TEST_F(StoreEdgeCases, ListAgentsEmptyStore) {
  json result = store_.list_agents();
  ASSERT_TRUE(result.contains("agents"));
  EXPECT_TRUE(result["agents"].is_array());
  EXPECT_EQ(result["agents"].size(), 0u);
}

TEST_F(StoreEdgeCases, ListTeamsEmptyStore) {
  json result = store_.list_teams();
  ASSERT_TRUE(result.contains("teams"));
  EXPECT_TRUE(result["teams"].is_array());
  EXPECT_EQ(result["teams"].size(), 0u);
}

TEST_F(StoreEdgeCases, ListAllTasksEmptyStore) {
  json result = store_.list_all_tasks();
  ASSERT_TRUE(result.contains("tasks"));
  EXPECT_TRUE(result["tasks"].is_array());
  EXPECT_EQ(result["tasks"].size(), 0u);
}

TEST_F(StoreEdgeCases, ListFaultsEmptyStore) {
  json result = store_.list_faults();
  ASSERT_TRUE(result.contains("faults"));
  EXPECT_TRUE(result["faults"].is_array());
  EXPECT_EQ(result["faults"].size(), 0u);
}

TEST_F(StoreEdgeCases, ListTasksForUnknownTeam) {
  json result = store_.list_tasks_for_team("no-such-team");
  ASSERT_TRUE(result.contains("tasks"));
  EXPECT_TRUE(result["tasks"].is_array());
  EXPECT_EQ(result["tasks"].size(), 0u);
}

// ── Large payloads ─────────────────────────────────────────────────────────────

TEST_F(StoreEdgeCases, LargePayloadAgent) {
  std::string big(1024 * 1024, 'x');
  json result = store_.create_agent({{"taskDescription", big}});
  ASSERT_TRUE(result.contains("agent"));
  EXPECT_EQ(result["agent"]["taskDescription"].get<std::string>(), big);
}

TEST_F(StoreEdgeCases, LargePayloadTask) {
  auto team_result = store_.create_team({{"name", "t"}});
  std::string team_id = team_result["team"]["id"];
  std::string big(1024 * 1024, 'y');
  json result = store_.create_task(team_id, {{"description", big}});
  ASSERT_TRUE(result.contains("task"));
  EXPECT_EQ(result["task"]["description"].get<std::string>(), big);
}

// ── UUID uniqueness and format ────────────────────────────────────────────────

TEST_F(StoreEdgeCases, UUIDUniqueness) {
  constexpr int N = 100;
  std::unordered_set<std::string> ids;
  ids.reserve(N);
  for (int i = 0; i < N; ++i) ids.insert(generate_uuid());
  EXPECT_EQ(static_cast<int>(ids.size()), N);
}

TEST_F(StoreEdgeCases, UUIDFormat) {
  std::string uuid = generate_uuid();
  // UUID v4: xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx
  std::regex re(R"([0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})");
  EXPECT_TRUE(std::regex_match(uuid, re)) << "UUID was: " << uuid;
}

// ── get_agent_by_name edge cases ──────────────────────────────────────────────

TEST_F(StoreEdgeCases, GetAgentByNameNotFound) {
  json result = store_.get_agent_by_name("no-such-name");
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, GetAgentByNameEmptyName) {
  // Empty search on empty store
  EXPECT_TRUE(store_.get_agent_by_name("").is_null());
  // After creating an agent with default name "unnamed", empty search still null
  store_.create_agent(json::object());
  EXPECT_TRUE(store_.get_agent_by_name("").is_null());
}

// ── Null-json guards (#209) ───────────────────────────────────────────────────

TEST_F(StoreEdgeCases, UpdateTaskNullJsonReturnsNull) {
  auto team_result = store_.create_team({{"name", "guard-team"}});
  std::string team_id = team_result["team"]["id"];
  auto task_result = store_.create_task(team_id, {{"subject", "s"}});
  std::string task_id = task_result["task"]["id"];

  // nlohmann::json{} is a null json value — must not throw and must return null.
  EXPECT_NO_THROW({
    json result = store_.update_task(team_id, task_id, nlohmann::json{});
    EXPECT_TRUE(result.is_null());
  });
}

TEST_F(StoreEdgeCases, UpdateTaskNullJsonArray) {
  auto team_result = store_.create_team({{"name", "guard-team2"}});
  std::string team_id = team_result["team"]["id"];
  auto task_result = store_.create_task(team_id, {{"subject", "s"}});
  std::string task_id = task_result["task"]["id"];

  // An array is not an object — must return null without throwing.
  EXPECT_NO_THROW({
    json result = store_.update_task(team_id, task_id, json::array());
    EXPECT_TRUE(result.is_null());
  });
}

TEST_F(StoreEdgeCases, UpdateAgentNullJsonReturnsNull) {
  auto agent_result = store_.create_agent({{"name", "guard-agent"}});
  std::string id = agent_result["id"];

  EXPECT_NO_THROW({
    json result = store_.update_agent(id, nlohmann::json{});
    EXPECT_TRUE(result.is_null());
  });
}

// ── Task team_id mismatch ─────────────────────────────────────────────────────

TEST_F(StoreEdgeCases, GetTaskWrongTeamId) {
  auto team_result = store_.create_team({{"name", "t"}});
  std::string team_id = team_result["team"]["id"];
  auto task_result = store_.create_task(team_id, {{"subject", "s"}});
  std::string task_id = task_result["task"]["id"];

  // Correct team_id → found
  json found = store_.get_task(team_id, task_id);
  EXPECT_FALSE(found.is_null());

  // Wrong team_id → not found
  json not_found = store_.get_task("wrong-team", task_id);
  EXPECT_TRUE(not_found.is_null());
}

// #222 — team_id scope enforcement on update_task
TEST_F(StoreEdgeCases, UpdateTaskEmptyTeamIdReturnsNull) {
  auto team_result = store_.create_team({{"name", "t"}});
  std::string team_id = team_result["team"]["id"];
  auto task_result = store_.create_task(team_id, {{"subject", "s"}});
  std::string task_id = task_result["task"]["id"];

  // Empty team_id must return null — cannot update without scoping to a team.
  json result = store_.update_task("", task_id, {{"subject", "new"}});
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, UpdateTaskWrongTeamIdReturnsNull) {
  auto team_result = store_.create_team({{"name", "t"}});
  std::string team_id = team_result["team"]["id"];
  auto task_result = store_.create_task(team_id, {{"subject", "s"}});
  std::string task_id = task_result["task"]["id"];

  json result = store_.update_task("wrong-team", task_id, {{"subject", "new"}});
  EXPECT_TRUE(result.is_null());
}

TEST_F(StoreEdgeCases, GetTaskCorrectTeamIdReturnsTask) {
  auto team_result = store_.create_team({{"name", "t"}});
  std::string team_id = team_result["team"]["id"];
  auto task_result = store_.create_task(team_id, {{"subject", "s"}});
  std::string task_id = task_result["task"]["id"];

  // Correct team_id must still find the task.
  json result = store_.get_task(team_id, task_id);
  EXPECT_FALSE(result.is_null());
  EXPECT_EQ(result["id"], task_id);
}

TEST_F(StoreEdgeCases, GetTaskEmptyTeamIdEnforcesScope) {
  auto team_result = store_.create_team({{"name", "t"}});
  std::string team_id = team_result["team"]["id"];
  auto task_result = store_.create_task(team_id, {{"subject", "s"}});
  std::string task_id = task_result["task"]["id"];

  // #222: empty team_id must return null — no longer a wildcard bypass.
  json found = store_.get_task("", task_id);
  EXPECT_TRUE(found.is_null());
}

}  // namespace projectagamemnon::test
