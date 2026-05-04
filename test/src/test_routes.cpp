#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"
#include "nlohmann/json.hpp"

#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

namespace projectagamemnon::test {

using json = nlohmann::json;

// Port chosen to avoid conflicts with production default (8080).
static constexpr int kTestPort = 18080;
static constexpr const char* kHost = "127.0.0.1";

class RoutesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    register_routes(server_, store_, nats_);
    server_thread_ = std::thread([this] { server_.listen(kHost, kTestPort); });

    // Poll until the server is accepting connections (max 2 s).
    for (int i = 0; i < 200; ++i) {
      if (server_.is_running()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void TearDown() override {
    server_.stop();
    if (server_thread_.joinable()) server_thread_.join();
  }

  // Convenience helpers
  httplib::Result Get(const std::string& path) { return client_.Get(path); }

  httplib::Result Post(const std::string& path, const json& body) {
    return client_.Post(path, body.dump(), "application/json");
  }

  httplib::Result Patch(const std::string& path, const json& body) {
    return client_.Patch(path, body.dump(), "application/json");
  }

  httplib::Result Put(const std::string& path, const json& body) {
    return client_.Put(path, body.dump(), "application/json");
  }

  httplib::Result Delete(const std::string& path) { return client_.Delete(path); }

  Store store_;
  NatsClient nats_{"nats://127.0.0.1:14222"};  // never connected — all publishes are no-ops
  httplib::Server server_;
  std::thread server_thread_;
  httplib::Client client_{kHost, kTestPort};
};

// ── Health / version ──────────────────────────────────────────────────────────

TEST_F(RoutesTest, HealthRoot) {
  auto res = Get("/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
  EXPECT_EQ(body["service"], "ProjectAgamemnon");
}

TEST_F(RoutesTest, HealthV1) {
  auto res = Get("/v1/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"], "ok");
}

TEST_F(RoutesTest, Version) {
  auto res = Get("/v1/version");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["version"], "0.1.0");
  EXPECT_EQ(body["name"], "ProjectAgamemnon");
}

TEST_F(RoutesTest, WorkflowsEmpty) {
  auto res = Get("/v1/workflows");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["workflows"].empty());
}

// ── Agents ────────────────────────────────────────────────────────────────────

TEST_F(RoutesTest, ListAgentsEmpty) {
  auto res = Get("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["agents"].empty());
}

TEST_F(RoutesTest, CreateAgent) {
  auto res = Post("/v1/agents", {{"name", "bob"}, {"role", "worker"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("id"));
  EXPECT_TRUE(body.contains("agent"));
  EXPECT_EQ(body["agent"]["name"], "bob");
}

TEST_F(RoutesTest, CreateAgentInvalidJson) {
  auto res = client_.Post("/v1/agents", "not-json{{{", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(RoutesTest, GetAgentFound) {
  auto create_res = Post("/v1/agents", {{"name", "get-me"}});
  std::string id = json::parse(create_res->body)["id"];

  auto res = Get("/v1/agents/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agent"]["id"], id);
}

TEST_F(RoutesTest, GetAgentNotFound) {
  auto res = Get("/v1/agents/no-such-id");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, GetAgentByName) {
  Post("/v1/agents", {{"name", "named-bob"}});
  auto res = Get("/v1/agents/by-name/named-bob");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agent"]["name"], "named-bob");
}

TEST_F(RoutesTest, GetAgentByNameNotFound) {
  auto res = Get("/v1/agents/by-name/nobody");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, PatchAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "before"}})->body)["id"];
  auto res = Patch("/v1/agents/" + id, {{"name", "after"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agent"]["name"], "after");
}

TEST_F(RoutesTest, PatchAgentNotFound) {
  auto res = Patch("/v1/agents/missing", {{"name", "x"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, DeleteAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "del-me"}})->body)["id"];
  auto res = Delete("/v1/agents/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["deleted"], id);
  // Verify gone
  EXPECT_EQ(Get("/v1/agents/" + id)->status, 404);
}

TEST_F(RoutesTest, DeleteAgentNotFound) {
  auto res = Delete("/v1/agents/ghost");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, StartAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "starter"}})->body)["id"];
  auto res = Post("/v1/agents/" + id + "/start", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"], "online");
}

TEST_F(RoutesTest, StartAgentNotFound) {
  auto res = Post("/v1/agents/nobody/start", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, StopAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "stopper"}})->body)["id"];
  Post("/v1/agents/" + id + "/start", json::object());
  auto res = Post("/v1/agents/" + id + "/stop", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"], "offline");
}

TEST_F(RoutesTest, StopAgentNotFound) {
  auto res = Post("/v1/agents/nobody/stop", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, CreateDockerAgent) {
  auto res = Post("/v1/agents/docker", {{"name", "dock-agent"}, {"host", "docker"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("id"));
  EXPECT_EQ(body["agent"]["name"], "dock-agent");
}

// ── Teams ─────────────────────────────────────────────────────────────────────

TEST_F(RoutesTest, ListTeamsEmpty) {
  auto res = Get("/v1/teams");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["teams"].empty());
}

TEST_F(RoutesTest, CreateTeam) {
  auto res = Post("/v1/teams", {{"name", "alpha"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("team"));
  EXPECT_EQ(body["team"]["name"], "alpha");
}

TEST_F(RoutesTest, GetTeamFound) {
  std::string id = json::parse(Post("/v1/teams", {{"name", "get-team"}})->body)["team"]["id"];
  auto res = Get("/v1/teams/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["team"]["id"], id);
}

TEST_F(RoutesTest, GetTeamNotFound) {
  EXPECT_EQ(Get("/v1/teams/no-id")->status, 404);
}

TEST_F(RoutesTest, UpdateTeam) {
  std::string id = json::parse(Post("/v1/teams", {{"name", "old"}})->body)["team"]["id"];
  auto res = Put("/v1/teams/" + id, {{"name", "new"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["team"]["name"], "new");
}

TEST_F(RoutesTest, UpdateTeamNotFound) {
  EXPECT_EQ(Put("/v1/teams/ghost", {{"name", "x"}})->status, 404);
}

TEST_F(RoutesTest, DeleteTeam) {
  std::string id = json::parse(Post("/v1/teams", {{"name", "bye"}})->body)["team"]["id"];
  auto res = Delete("/v1/teams/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["deleted"], id);
  EXPECT_EQ(Get("/v1/teams/" + id)->status, 404);
}

TEST_F(RoutesTest, DeleteTeamNotFound) {
  EXPECT_EQ(Delete("/v1/teams/ghost")->status, 404);
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

TEST_F(RoutesTest, ListAllTasksEmpty) {
  auto res = Get("/v1/tasks");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["tasks"].empty());
}

TEST_F(RoutesTest, CreateTask) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "team"}})->body)["team"]["id"];
  auto res = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "build"}, {"type", "build"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("task"));
  EXPECT_EQ(body["task"]["status"], "pending");
  EXPECT_EQ(body["task"]["teamId"], team_id);
}

TEST_F(RoutesTest, ListTasksForTeam) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "t"}})->body)["team"]["id"];
  Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s1"}});
  Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s2"}});
  auto res = Get("/v1/teams/" + team_id + "/tasks");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["tasks"].size(), 2u);
}

TEST_F(RoutesTest, GetTask) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "t"}})->body)["team"]["id"];
  std::string task_id =
      json::parse(Post("/v1/teams/" + team_id + "/tasks", {{"subject", "x"}})->body)["task"]["id"];
  auto res = Get("/v1/teams/" + team_id + "/tasks/" + task_id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["task"]["id"], task_id);
}

TEST_F(RoutesTest, GetTaskNotFound) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "t"}})->body)["team"]["id"];
  EXPECT_EQ(Get("/v1/teams/" + team_id + "/tasks/missing")->status, 404);
}

TEST_F(RoutesTest, PatchTask) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "t"}})->body)["team"]["id"];
  std::string task_id =
      json::parse(Post("/v1/teams/" + team_id + "/tasks", {{"subject", "old"}})->body)["task"]["id"];
  auto res = Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"subject", "new"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["task"]["subject"], "new");
}

TEST_F(RoutesTest, PatchTaskNotFound) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "t"}})->body)["team"]["id"];
  EXPECT_EQ(Patch("/v1/teams/" + team_id + "/tasks/nope", {{"subject", "x"}})->status, 404);
}

TEST_F(RoutesTest, PutTask) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "t"}})->body)["team"]["id"];
  std::string task_id =
      json::parse(Post("/v1/teams/" + team_id + "/tasks", {{"subject", "work"}})->body)["task"]["id"];
  auto res = Put("/v1/teams/" + team_id + "/tasks/" + task_id, {{"status", "in_progress"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["task"]["status"], "in_progress");
}

TEST_F(RoutesTest, PutTaskNotFound) {
  std::string team_id =
      json::parse(Post("/v1/teams", {{"name", "t"}})->body)["team"]["id"];
  EXPECT_EQ(Put("/v1/teams/" + team_id + "/tasks/nope", {})->status, 404);
}

// ── Chaos ─────────────────────────────────────────────────────────────────────

TEST_F(RoutesTest, ListChaosEmpty) {
  auto res = Get("/v1/chaos");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["faults"].empty());
}

TEST_F(RoutesTest, CreateFault) {
  auto res = Post("/v1/chaos/latency", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("fault"));
  EXPECT_EQ(body["fault"]["type"], "latency");
  EXPECT_TRUE(body["fault"]["active"].get<bool>());
}

TEST_F(RoutesTest, DeleteFault) {
  std::string id = json::parse(Post("/v1/chaos/error-rate", json::object())->body)["fault"]["id"];
  auto res = Delete("/v1/chaos/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["deleted"], id);
}

TEST_F(RoutesTest, DeleteFaultNotFound) {
  EXPECT_EQ(Delete("/v1/chaos/missing")->status, 404);
}

}  // namespace projectagamemnon::test
