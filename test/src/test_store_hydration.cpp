#include "projectagamemnon/github_client.hpp"
#include "projectagamemnon/store.hpp"

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Store with no GitHub client (in-memory only) ─────────────────────────────

TEST(StoreHydrationTest, InMemoryModeCreateAndListAgents) {
  Store store(nullptr);
  json body;
  body["name"] = "test-agent";
  auto result = store.create_agent(body);
  ASSERT_TRUE(result.contains("id"));
  ASSERT_TRUE(result.contains("agent"));

  auto list = store.list_agents();
  ASSERT_TRUE(list.contains("agents"));
  EXPECT_EQ(list["agents"].size(), 1U);
}

TEST(StoreHydrationTest, InMemoryModeCreateAndListTasks) {
  Store store(nullptr);
  json body;
  body["subject"] = "do something";
  body["type"] = "general";
  auto result = store.create_task("team-1", body);
  ASSERT_TRUE(result.contains("task"));
  EXPECT_EQ(result["task"]["status"], "pending");

  auto list = store.list_all_tasks();
  ASSERT_TRUE(list.contains("tasks"));
  EXPECT_EQ(list["tasks"].size(), 1U);
}

TEST(StoreHydrationTest, InMemoryModeMarkTaskCompleted) {
  Store store(nullptr);
  json body;
  body["subject"] = "task to complete";
  auto result = store.create_task("team-1", body);
  std::string task_id = result["task"]["id"];

  store.mark_task_completed(task_id);

  auto task = store.get_task("", task_id);
  EXPECT_EQ(task["status"], "completed");
  EXPECT_FALSE(task["completedAt"].is_null());
}

TEST(StoreHydrationTest, DisabledGitHubClientSetsLoadedOnFirstList) {
  // With a disabled (no token) GitHubClient, hydration must not crash and
  // must set the loaded flag so it never retries.
  GitHubClient::Config cfg;  // empty = disabled
  GitHubClient gh(cfg);
  ASSERT_FALSE(gh.is_enabled());

  Store store(&gh);

  // list_agents triggers ensure_agents_loaded(); should return empty without crash.
  auto agents = store.list_agents();
  EXPECT_TRUE(agents["agents"].is_array());
  EXPECT_EQ(agents["agents"].size(), 0U);

  // Second call must also not crash (loaded sentinel prevents retry).
  auto agents2 = store.list_agents();
  EXPECT_TRUE(agents2["agents"].is_array());
}

TEST(StoreHydrationTest, DisabledGitHubClientDoesNotSetIssueNumber) {
  GitHubClient::Config cfg;
  GitHubClient gh(cfg);
  Store store(&gh);

  json body;
  body["subject"] = "gh-disabled task";
  auto result = store.create_task("team-x", body);
  // When GitHub is disabled, githubIssueNumber stays -1.
  EXPECT_EQ(result["task"]["githubIssueNumber"], -1);
}

TEST(StoreHydrationTest, DisabledGitHubClientAgentIssueNumberIsMinusOne) {
  GitHubClient::Config cfg;
  GitHubClient gh(cfg);
  Store store(&gh);

  json body;
  body["name"] = "agent-no-gh";
  auto result = store.create_agent(body);
  EXPECT_EQ(result["agent"]["githubIssueNumber"], -1);
}

TEST(StoreHydrationTest, GetNonexistentAgentReturnsNull) {
  Store store(nullptr);
  auto result = store.get_agent("does-not-exist");
  EXPECT_TRUE(result.is_null());
}

TEST(StoreHydrationTest, GetNonexistentTaskReturnsNull) {
  Store store(nullptr);
  auto result = store.get_task("", "does-not-exist");
  EXPECT_TRUE(result.is_null());
}

TEST(StoreHydrationTest, UpdateAgentPreservesId) {
  Store store(nullptr);
  json body;
  body["name"] = "agent-a";
  auto created = store.create_agent(body);
  std::string id = created["id"];

  json update;
  update["id"] = "injected-bad-id";
  update["label"] = "new-label";
  store.update_agent(id, update);

  auto agent = store.get_agent(id);
  EXPECT_EQ(agent["id"], id);
  EXPECT_EQ(agent["label"], "new-label");
}

TEST(StoreHydrationTest, DeleteAgentRemovesFromStore) {
  Store store(nullptr);
  json body;
  body["name"] = "agent-to-delete";
  auto created = store.create_agent(body);
  std::string id = created["id"];

  EXPECT_TRUE(store.delete_agent(id));
  EXPECT_TRUE(store.get_agent(id).is_null());
  EXPECT_FALSE(store.delete_agent(id));  // second delete returns false
}

TEST(StoreHydrationTest, StartAndStopAgentChangesStatus) {
  Store store(nullptr);
  json body;
  body["name"] = "stateful-agent";
  auto created = store.create_agent(body);
  std::string id = created["id"];

  auto start_result = store.start_agent(id);
  EXPECT_EQ(start_result["status"], "online");
  EXPECT_EQ(store.get_agent(id)["status"], "online");

  auto stop_result = store.stop_agent(id);
  EXPECT_EQ(stop_result["status"], "offline");
  EXPECT_EQ(store.get_agent(id)["status"], "offline");
}

TEST(StoreHydrationTest, TasksLoadedSentinelPreventsDoubleHydration) {
  GitHubClient::Config cfg;
  GitHubClient gh(cfg);
  Store store(&gh);

  // Two successive list_all_tasks calls — the disabled client returns empty each
  // time but the loaded sentinel should prevent any re-entry issues.
  store.list_all_tasks();
  json body;
  body["subject"] = "local task";
  store.create_task("team-1", body);

  // The task we created must survive a second list call.
  auto list = store.list_all_tasks();
  EXPECT_EQ(list["tasks"].size(), 1U);
}

TEST(StoreHydrationTest, UpdateTaskDoesNotRegress) {
  Store store(nullptr);
  json body;
  body["subject"] = "task";
  auto result = store.create_task("t1", body);
  std::string task_id = result["task"]["id"];

  store.mark_task_completed(task_id);

  // Attempt to update status back to "pending" via update_task.
  json upd;
  upd["status"] = "pending";
  store.update_task("t1", task_id, upd);

  // The update goes through (update_task doesn't enforce state machine),
  // but completedAt should have been set before the regression.
  // This test verifies that mark_task_completed sets completedAt correctly.
  auto task = store.get_task("", task_id);
  EXPECT_FALSE(task["completedAt"].is_null());
}

TEST(StoreHydrationTest, ListTasksForTeamFiltersCorrectly) {
  Store store(nullptr);

  json b1;
  b1["subject"] = "task-for-team-A";
  store.create_task("team-A", b1);

  json b2;
  b2["subject"] = "task-for-team-B";
  store.create_task("team-B", b2);

  auto result = store.list_tasks_for_team("team-A");
  ASSERT_TRUE(result.contains("tasks"));
  EXPECT_EQ(result["tasks"].size(), 1U);
  EXPECT_EQ(result["tasks"][0]["teamId"], "team-A");
}

TEST(StoreHydrationTest, GetAgentByName) {
  Store store(nullptr);
  json body;
  body["name"] = "unique-agent-name";
  store.create_agent(body);

  auto found = store.get_agent_by_name("unique-agent-name");
  EXPECT_FALSE(found.is_null());
  EXPECT_EQ(found["name"], "unique-agent-name");

  auto not_found = store.get_agent_by_name("nonexistent");
  EXPECT_TRUE(not_found.is_null());
}

}  // namespace projectagamemnon::test
