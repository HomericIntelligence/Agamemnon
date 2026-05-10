#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "projectagamemnon/auth.hpp"
#include "projectagamemnon/metrics.hpp"
#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/orchestrator.hpp"
#include "projectagamemnon/rate_limiter.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace projectagamemnon::test {

using json = nlohmann::json;

class RoutesHappyPathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    register_routes(server_, store_, nats_, rate_limiter_, auth_, metrics_, orchestrator_);
    // Bind to an OS-assigned port to avoid cross-test port conflicts.
    int port = server_.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0) << "bind_to_any_port failed";
    server_thread_ = std::thread([this] { server_.listen_after_bind(); });
    client_ = std::make_unique<httplib::Client>("127.0.0.1", port);
    client_->set_connection_timeout(5);
    client_->set_read_timeout(5);
    server_.wait_until_ready();
  }

  void TearDown() override {
    server_.stop();
    if (server_thread_.joinable()) server_thread_.join();
  }

  // Convenience helpers
  httplib::Result Get(const std::string& path) { return client_->Get(path); }

  httplib::Result Post(const std::string& path, const json& body) {
    return client_->Post(path, body.dump(), "application/json");
  }

  httplib::Result Patch(const std::string& path, const json& body) {
    return client_->Patch(path, body.dump(), "application/json");
  }

  httplib::Result Put(const std::string& path, const json& body) {
    return client_->Put(path, body.dump(), "application/json");
  }

  httplib::Result Delete(const std::string& path) { return client_->Delete(path); }

  Store store_;
  NatsClient nats_{"nats://127.0.0.1:14222"};  // never connected — all publishes are no-ops
  RateLimiter rate_limiter_{1e9, 1e9};          // effectively unlimited for tests
  AuthMiddleware auth_{""};                     // empty key = allow all requests in tests
  MetricsRegistry metrics_;
  Orchestrator orchestrator_{store_, nats_};    // HMAS orchestrator
  httplib::Server server_;
  std::thread server_thread_;
  std::unique_ptr<httplib::Client> client_;
};

// ── Health / version ──────────────────────────────────────────────────────────

TEST_F(RoutesHappyPathTest, HealthRoot) {
  auto res = Get("/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
  EXPECT_EQ(body["service"], "ProjectAgamemnon");
}

TEST_F(RoutesHappyPathTest, HealthV1) {
  auto res = Get("/v1/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"], "ok");
}

TEST_F(RoutesHappyPathTest, Version) {
  auto res = Get("/v1/version");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["version"], "0.1.0");
  EXPECT_EQ(body["name"], "ProjectAgamemnon");
}

// ── Agents ────────────────────────────────────────────────────────────────────

TEST_F(RoutesHappyPathTest, ListAgentsEmpty) {
  auto res = Get("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["agents"].empty());
}

TEST_F(RoutesHappyPathTest, CreateAgent) {
  auto res = Post("/v1/agents", {{"name", "bob"}, {"role", "worker"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("id"));
  EXPECT_TRUE(body.contains("agent"));
  EXPECT_EQ(body["agent"]["name"], "bob");
}

TEST_F(RoutesHappyPathTest, GetAgentById) {
  json payload = {{"name", "route-agent"}};
  auto create = client_->Post("/v1/agents", payload.dump(), "application/json");
  ASSERT_NE(create, nullptr);
  std::string id = json::parse(create->body)["id"];

  auto res = Get("/v1/agents/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agent"]["id"], id);
}

TEST_F(RoutesHappyPathTest, GetAgentByIdNotFound) {
  auto res = Get("/v1/agents/no-such-id");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, GetAgentByName) {
  Post("/v1/agents", {{"name", "named-bob"}});
  auto res = Get("/v1/agents/by-name/named-bob");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agent"]["name"], "named-bob");
}

TEST_F(RoutesHappyPathTest, GetAgentByNameNotFound) {
  auto res = Get("/v1/agents/by-name/nobody");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, PatchAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "before"}})->body)["id"];
  auto res = Patch("/v1/agents/" + id, {{"name", "after"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agent"]["name"], "after");
}

TEST_F(RoutesHappyPathTest, PatchAgentNotFound) {
  auto res = Patch("/v1/agents/missing", {{"name", "x"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, DeleteAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "del-me"}})->body)["id"];
  auto res = Delete("/v1/agents/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["deleted"], id);
  // Verify gone
  EXPECT_EQ(Get("/v1/agents/" + id)->status, 404);
}

TEST_F(RoutesHappyPathTest, DeleteAgentNotFound) {
  auto res = Delete("/v1/agents/ghost");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, StartAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "starter"}})->body)["id"];
  auto res = Post("/v1/agents/" + id + "/start", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"], "online");
}

TEST_F(RoutesHappyPathTest, StartAgentNotFound) {
  auto res = Post("/v1/agents/nobody/start", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, StopAgent) {
  std::string id = json::parse(Post("/v1/agents", {{"name", "stopper"}})->body)["id"];
  Post("/v1/agents/" + id + "/start", json::object());
  auto res = Post("/v1/agents/" + id + "/stop", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"], "offline");
}

TEST_F(RoutesHappyPathTest, StopAgentNotFound) {
  auto res = Post("/v1/agents/nobody/stop", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, CreateDockerAgent) {
  auto res = Post("/v1/agents/docker", {{"name", "dock-agent"}, {"host", "docker"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("id"));
  EXPECT_EQ(body["agent"]["name"], "dock-agent");
}

// ── Teams ─────────────────────────────────────────────────────────────────────

TEST_F(RoutesHappyPathTest, ListTeamsEmpty) {
  auto res = Get("/v1/teams");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["teams"].empty());
}

TEST_F(RoutesHappyPathTest, CreateTeam) {
  auto res = Post("/v1/teams", {{"name", "alpha"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("team"));
  EXPECT_EQ(body["team"]["name"], "alpha");
}

TEST_F(RoutesHappyPathTest, GetTeamFound) {
  std::string id = json::parse(Post("/v1/teams", {{"name", "get-team"}})->body)["team"]["id"];
  auto res = Get("/v1/teams/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["team"]["id"], id);
}

TEST_F(RoutesHappyPathTest, GetTeamNotFound) { EXPECT_EQ(Get("/v1/teams/no-id")->status, 404); }

TEST_F(RoutesHappyPathTest, UpdateTeam) {
  std::string id = json::parse(Post("/v1/teams", {{"name", "old"}})->body)["team"]["id"];
  auto res = Put("/v1/teams/" + id, {{"name", "new"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["team"]["name"], "new");
}

TEST_F(RoutesHappyPathTest, UpdateTeamNotFound) {
  EXPECT_EQ(Put("/v1/teams/ghost", {{"name", "x"}})->status, 404);
}

TEST_F(RoutesHappyPathTest, DeleteTeam) {
  std::string id = json::parse(Post("/v1/teams", {{"name", "bye"}})->body)["team"]["id"];
  auto res = Delete("/v1/teams/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["deleted"], id);
  EXPECT_EQ(Get("/v1/teams/" + id)->status, 404);
}

TEST_F(RoutesHappyPathTest, DeleteTeamNotFound) {
  EXPECT_EQ(Delete("/v1/teams/ghost")->status, 404);
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

class RoutesTaskTest : public RoutesHappyPathTest {
 protected:
  std::string team_id;

  void SetUp() override {
    RoutesHappyPathTest::SetUp();
    auto create =
        client_->Post("/v1/teams", json{{"name", "task-team"}}.dump(), "application/json");
    team_id = json::parse(create->body)["team"]["id"];
  }
};

TEST_F(RoutesTaskTest, ListAllTasksEmpty) {
  auto res = Get("/v1/tasks");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["tasks"].empty());
}

TEST_F(RoutesTaskTest, CreateTask) {
  auto res =
      Post("/v1/teams/" + team_id + "/tasks", {{"subject", "build"}, {"type", "implementation"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("task"));
  EXPECT_EQ(body["task"]["status"], "pending");
  EXPECT_EQ(body["task"]["teamId"], team_id);
}

TEST_F(RoutesTaskTest, ListTasksForTeam) {
  Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s1"}});
  Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s2"}});
  auto res = Get("/v1/teams/" + team_id + "/tasks");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["tasks"].size(), 2u);
}

TEST_F(RoutesTaskTest, GetTask) {
  std::string task_id =
      json::parse(Post("/v1/teams/" + team_id + "/tasks", {{"subject", "x"}})->body)["task"]["id"];
  auto res = Get("/v1/teams/" + team_id + "/tasks/" + task_id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["task"]["id"], task_id);
}

TEST_F(RoutesTaskTest, GetTaskNotFound) {
  EXPECT_EQ(Get("/v1/teams/" + team_id + "/tasks/missing")->status, 404);
}

TEST_F(RoutesTaskTest, PatchTask) {
  std::string task_id = json::parse(
      Post("/v1/teams/" + team_id + "/tasks", {{"subject", "old"}})->body)["task"]["id"];
  auto res = Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"subject", "new"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["task"]["subject"], "new");
}

TEST_F(RoutesTaskTest, PatchTaskNotFound) {
  EXPECT_EQ(Patch("/v1/teams/" + team_id + "/tasks/nope", {{"subject", "x"}})->status, 404);
}

TEST_F(RoutesTaskTest, PutTask) {
  std::string task_id = json::parse(
      Post("/v1/teams/" + team_id + "/tasks", {{"subject", "work"}})->body)["task"]["id"];
  auto res = Put("/v1/teams/" + team_id + "/tasks/" + task_id, {{"status", "running"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["task"]["status"], "running");
}

TEST_F(RoutesTaskTest, PutTaskNotFound) {
  EXPECT_EQ(Put("/v1/teams/" + team_id + "/tasks/nope", {})->status, 404);
}

// ── Chaos ─────────────────────────────────────────────────────────────────────

TEST_F(RoutesHappyPathTest, ListChaosEmpty) {
  auto res = Get("/v1/chaos");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["faults"].empty());
}

TEST_F(RoutesHappyPathTest, CreateChaos) {
  auto res = Post("/v1/chaos/latency", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("fault"));
  EXPECT_EQ(body["fault"]["type"], "latency");
  EXPECT_TRUE(body["fault"]["active"].get<bool>());
}

TEST_F(RoutesHappyPathTest, DeleteChaos) {
  std::string id = json::parse(Post("/v1/chaos/error-rate", json::object())->body)["fault"]["id"];
  auto res = Delete("/v1/chaos/" + id);
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["deleted"], id);
}

TEST_F(RoutesHappyPathTest, DeleteChaosNotFound) {
  auto res = Delete("/v1/chaos/missing");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── Metrics endpoint ──────────────────────────────────────────────────────────

TEST_F(RoutesHappyPathTest, MetricsEndpointReturnsPrometheusText) {
  auto res = Get("/metrics");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  // Content-Type must start with "text/plain"
  auto ct = res->get_header_value("Content-Type");
  EXPECT_EQ(ct.substr(0, 10), "text/plain");
  // Body must contain standard Prometheus text-format markers
  EXPECT_NE(res->body.find("# HELP"), std::string::npos);
  EXPECT_NE(res->body.find("# TYPE"), std::string::npos);
}

TEST_F(RoutesHappyPathTest, MetricsBodyContainsProcessStartTime) {
  // hi_process_start_time_seconds and hi_build_info are always populated at
  // MetricsRegistry construction, so they appear even before any requests are made.
  auto res = Get("/metrics");
  ASSERT_TRUE(res);
  EXPECT_NE(res->body.find("hi_process_start_time_seconds"), std::string::npos);
  EXPECT_NE(res->body.find("hi_build_info"), std::string::npos);
}

// ── Dead-letter queue endpoints ───────────────────────────────────────────────

TEST_F(RoutesHappyPathTest, DeadLetterGetReturnsEmptyArray) {
  auto res = Get("/v1/dead-letter");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  ASSERT_TRUE(body.contains("dead_letter_queue"));
  EXPECT_TRUE(body["dead_letter_queue"].is_array());
}

TEST_F(RoutesHappyPathTest, DeadLetterDeleteReturnsCleared) {
  auto del_res = Delete("/v1/dead-letter");
  ASSERT_TRUE(del_res);
  EXPECT_EQ(del_res->status, 200);
  auto del_body = json::parse(del_res->body);
  EXPECT_TRUE(del_body.contains("cleared"));

  // After DELETE the GET should still return an empty array
  auto get_res = Get("/v1/dead-letter");
  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->status, 200);
  auto get_body = json::parse(get_res->body);
  EXPECT_TRUE(get_body["dead_letter_queue"].is_array());
}

// ── HMAS briefs ───────────────────────────────────────────────────────────────

TEST_F(RoutesHappyPathTest, CreateBriefMissingTitle) {
  auto res = Post("/v1/briefs", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(RoutesHappyPathTest, CreateBriefEmptyTitle) {
  auto res = Post("/v1/briefs", {{"title", ""}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(RoutesHappyPathTest, CreateBriefSuccess) {
  auto res = Post("/v1/briefs", {{"title", "test brief"}, {"description", "do the thing"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body.contains("brief_id"));
  EXPECT_TRUE(body.contains("tasks"));
}

TEST_F(RoutesHappyPathTest, GetBriefPlanNotFound) {
  auto res = Get("/v1/briefs/no-such-brief/plan");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, GetBriefPlanSuccess) {
  auto create = Post("/v1/briefs", {{"title", "plan brief"}, {"repos", json::array({"repo-a"})}});
  ASSERT_TRUE(create);
  ASSERT_EQ(create->status, 201);
  std::string brief_id = json::parse(create->body)["brief_id"];

  auto res = Get("/v1/briefs/" + brief_id + "/plan");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["brief_id"], brief_id);
  EXPECT_TRUE(body.contains("tasks"));
}

// ── HMAS task state/escalate/complete ────────────────────────────────────────

TEST_F(RoutesHappyPathTest, GetTaskStateNotFound) {
  auto res = Get("/v1/tasks/no-such-task/state");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, GetTaskStateSuccess) {
  // Submit a brief so HMAS tasks are created in the store.
  auto create = Post("/v1/briefs", {{"title", "state brief"}, {"repos", json::array({"repo-b"})}});
  ASSERT_TRUE(create);
  ASSERT_EQ(create->status, 201);
  auto plan = json::parse(create->body);
  // Grab the first task id from the plan.
  ASSERT_FALSE(plan["tasks"].empty());
  std::string task_id = plan["tasks"][0]["id"];

  auto res = Get("/v1/tasks/" + task_id + "/state");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["task_id"], task_id);
  EXPECT_TRUE(body.contains("state"));
  EXPECT_TRUE(body.contains("layer"));
}

TEST_F(RoutesHappyPathTest, EscalateTaskNotFound) {
  auto res = Post("/v1/tasks/no-such-task/escalate", {{"reason", "blocked"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesHappyPathTest, CompleteTaskSuccess) {
  // Submit a brief to get a real task id.
  auto create =
      Post("/v1/briefs", {{"title", "complete brief"}, {"repos", json::array({"repo-c"})}});
  ASSERT_TRUE(create);
  ASSERT_EQ(create->status, 201);
  std::string task_id = json::parse(create->body)["tasks"][0]["id"];

  auto res = Post("/v1/tasks/" + task_id + "/complete", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["task_id"], task_id);
  EXPECT_TRUE(body["completed"].get<bool>());
}

}  // namespace projectagamemnon::test
