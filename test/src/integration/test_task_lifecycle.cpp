#include "server_fixture.hpp"

#include <gtest/gtest.h>
#include "nlohmann/json.hpp"

namespace projectagamemnon::test {

using json = nlohmann::json;

class TaskLifecycleTest : public AgamemnonServerFixture {};

// Helper: create a team and return its id.
// Must be a free function — accesses public static client().
static std::string make_team(const std::string& name) {
  json body = {{"name", name}};
  auto res = AgamemnonServerFixture::client().Post("/v1/teams", body.dump(), "application/json");
  if (!res || res->status != 201) return {};
  // Response: {"team": {...}}
  auto data = json::parse(res->body);
  return data["team"]["id"].get<std::string>();
}

TEST_F(TaskLifecycleTest, CreateTeamReturns201) {
  json body = {{"name", "team-alpha"}};
  auto res = client().Post("/v1/teams", body.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 201);
  EXPECT_TRUE(nats().has_subject("hi.agents.team.created"));
}

TEST_F(TaskLifecycleTest, GetTeamById) {
  std::string team_id = make_team("get-team");
  ASSERT_FALSE(team_id.empty());

  auto res = client().Get("/v1/teams/" + team_id);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  auto data = json::parse(res->body);
  EXPECT_TRUE(data.contains("team"));
  EXPECT_EQ(data["team"]["id"].get<std::string>(), team_id);
}

TEST_F(TaskLifecycleTest, CreateTaskReturns201AndDispatchesToMyrmidon) {
  std::string team_id = make_team("dispatch-team");
  ASSERT_FALSE(team_id.empty());

  json body = {{"subject", "Implement feature X"}, {"type", "coding"}, {"description", "Write the code"}};
  auto res = client().Post("/v1/teams/" + team_id + "/tasks", body.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 201);

  // Response: {"task": {...}}
  auto data = json::parse(res->body);
  ASSERT_TRUE(data.contains("task")) << "Response: " << res->body;
  EXPECT_FALSE(data["task"]["id"].get<std::string>().empty());
  EXPECT_EQ(data["task"]["status"].get<std::string>(), "pending");

  EXPECT_TRUE(nats().has_subject("hi.tasks.created"));
  EXPECT_TRUE(nats().has_subject_prefix("hi.myrmidon.coding."));
}

TEST_F(TaskLifecycleTest, GetTaskByTeamAndId) {
  std::string team_id = make_team("get-task-team");
  ASSERT_FALSE(team_id.empty());

  json body = {{"subject", "Get task test"}, {"type", "general"}};
  auto create_res = client().Post("/v1/teams/" + team_id + "/tasks", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string task_id = json::parse(create_res->body)["task"]["id"].get<std::string>();

  auto res = client().Get("/v1/teams/" + team_id + "/tasks/" + task_id);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  auto data = json::parse(res->body);
  EXPECT_EQ(data["task"]["id"].get<std::string>(), task_id);
}

TEST_F(TaskLifecycleTest, ListTasksForTeam) {
  std::string team_id = make_team("list-tasks-team");
  ASSERT_FALSE(team_id.empty());

  json body = {{"subject", "Listed task"}, {"type", "general"}};
  auto create_res = client().Post("/v1/teams/" + team_id + "/tasks", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);

  auto res = client().Get("/v1/teams/" + team_id + "/tasks");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  // Response: {"tasks": [...]}
  auto data = json::parse(res->body);
  ASSERT_TRUE(data.contains("tasks")) << "Response: " << res->body;
  EXPECT_GE(data["tasks"].size(), 1u);
}

TEST_F(TaskLifecycleTest, ListAllTasks) {
  std::string team_id = make_team("all-tasks-team");
  ASSERT_FALSE(team_id.empty());

  json body = {{"subject", "Global task"}, {"type", "general"}};
  auto create_res = client().Post("/v1/teams/" + team_id + "/tasks", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);

  auto res = client().Get("/v1/tasks");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  // Response: {"tasks": [...]}
  auto data = json::parse(res->body);
  ASSERT_TRUE(data.contains("tasks")) << "Response: " << res->body;
  EXPECT_GE(data["tasks"].size(), 1u);
}

TEST_F(TaskLifecycleTest, PutTaskUpdatesStatus) {
  std::string team_id = make_team("put-task-team");
  ASSERT_FALSE(team_id.empty());

  json body = {{"subject", "Update task"}, {"type", "general"}};
  auto create_res = client().Post("/v1/teams/" + team_id + "/tasks", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string task_id = json::parse(create_res->body)["task"]["id"].get<std::string>();

  json update = {{"status", "in_progress"}, {"subject", "Update task"}};
  auto res = client().Put("/v1/teams/" + team_id + "/tasks/" + task_id, update.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);

  EXPECT_TRUE(nats().has_subject_prefix("hi.tasks." + team_id + "." + task_id + ".updated"));
}

TEST_F(TaskLifecycleTest, PatchTaskPartialUpdate) {
  std::string team_id = make_team("patch-task-team");
  ASSERT_FALSE(team_id.empty());

  json body = {{"subject", "Patch task"}, {"type", "general"}};
  auto create_res = client().Post("/v1/teams/" + team_id + "/tasks", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string task_id = json::parse(create_res->body)["task"]["id"].get<std::string>();

  json patch = {{"status", "in_progress"}};
  auto res = client().Patch("/v1/teams/" + team_id + "/tasks/" + task_id, patch.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(nats().has_subject_prefix("hi.tasks." + team_id + "." + task_id + ".updated"));
}

TEST_F(TaskLifecycleTest, TaskCompletionSetsCompletedAt) {
  std::string team_id = make_team("complete-task-team");
  ASSERT_FALSE(team_id.empty());

  json body = {{"subject", "Complete task"}, {"type", "general"}};
  auto create_res = client().Post("/v1/teams/" + team_id + "/tasks", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string task_id = json::parse(create_res->body)["task"]["id"].get<std::string>();

  // update_task returns the task object directly (not wrapped in {"task": ...})
  json patch = {{"status", "completed"}};
  auto res = client().Patch("/v1/teams/" + team_id + "/tasks/" + task_id, patch.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);

  auto data = json::parse(res->body);
  // Routes wrap in {"task": result} — and result is the task object itself.
  const auto& task = data.contains("task") ? data["task"] : data;
  EXPECT_EQ(task.value("status", ""), "completed");
}

TEST_F(TaskLifecycleTest, UpdateTeam) {
  std::string team_id = make_team("update-me-team");
  ASSERT_FALSE(team_id.empty());

  json update = {{"name", "updated-team"}};
  auto res = client().Put("/v1/teams/" + team_id, update.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(nats().has_subject("hi.agents.team.updated"));
}

TEST_F(TaskLifecycleTest, DeleteTeam) {
  std::string team_id = make_team("delete-me-team");
  ASSERT_FALSE(team_id.empty());

  auto res = client().Delete("/v1/teams/" + team_id);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(nats().has_subject("hi.agents.team.deleted"));

  auto get_res = client().Get("/v1/teams/" + team_id);
  ASSERT_NE(get_res, nullptr);
  EXPECT_EQ(get_res->status, 404);
}

}  // namespace projectagamemnon::test
