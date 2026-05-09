#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace projectagamemnon::test {

using json = nlohmann::json;

class RoutesTest : public ::testing::Test {
 protected:
  Store store_;
  NatsClient nats_{"nats://127.0.0.1:4222"};  // disconnected — all publish() are no-ops
  httplib::Server server_;
  std::thread server_thread_;
  std::unique_ptr<httplib::Client> client_;

  void SetUp() override {
    register_routes(server_, store_, nats_);
    int port = server_.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    server_thread_ = std::thread([this] { server_.listen_after_bind(); });
    client_ = std::make_unique<httplib::Client>("127.0.0.1", port);
    client_->set_connection_timeout(5);
    client_->set_read_timeout(5);
    // Wait until the server is actually listening
    server_.wait_until_ready();
  }

  void TearDown() override {
    server_.stop();
    if (server_thread_.joinable()) server_thread_.join();
  }
};

// ── Unknown top-level routes ──────────────────────────────────────────────────

TEST_F(RoutesTest, UnknownTopLevelRoute) {
  auto res = client_->Get("/v1/doesnotexist");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, UnknownNonV1Route) {
  auto res = client_->Get("/api/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── GET /v1/health and /v1/version ────────────────────────────────────────────

TEST_F(RoutesTest, HealthEndpoint) {
  auto res = client_->Get("/v1/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  json body = json::parse(res->body);
  EXPECT_EQ(body["status"], "ok");
}

TEST_F(RoutesTest, VersionEndpoint) {
  auto res = client_->Get("/v1/version");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  json body = json::parse(res->body);
  EXPECT_TRUE(body.contains("version"));
}

// ── POST /v1/agents with empty body ───────────────────────────────────────────

TEST_F(RoutesTest, PostAgentEmptyBody) {
  auto res = client_->Post("/v1/agents", "{}", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  json body = json::parse(res->body);
  EXPECT_TRUE(body.contains("agent"));
  EXPECT_EQ(body["agent"]["name"], "unnamed");
}

TEST_F(RoutesTest, PostAgentEmptyBodyString) {
  // Completely empty body string → parsed as empty object by parse_body
  auto res = client_->Post("/v1/agents", "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
}

// ── POST /v1/agents with malformed JSON ───────────────────────────────────────

TEST_F(RoutesTest, PostAgentMalformedJSON) {
  auto res = client_->Post("/v1/agents", "{bad json", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  json body = json::parse(res->body);
  EXPECT_TRUE(body.contains("error"));
}

// ── DELETE non-existent agent ─────────────────────────────────────────────────

TEST_F(RoutesTest, DeleteNonExistentAgent) {
  auto res = client_->Delete("/v1/agents/ghost-id-00000");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── GET non-existent agent ────────────────────────────────────────────────────

TEST_F(RoutesTest, GetAgentNonExistent) {
  auto res = client_->Get("/v1/agents/ghost-id-99999");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── GET agent by name not found ───────────────────────────────────────────────

TEST_F(RoutesTest, GetAgentByNameNotFound) {
  auto res = client_->Get("/v1/agents/by-name/no-such-agent");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── POST /v1/agents with large payload ────────────────────────────────────────

TEST_F(RoutesTest, PostAgentLargePayload) {
  std::string big(1024 * 1024, 'z');
  json body = {{"name", "biggie"}, {"taskDescription", big}};
  auto res = client_->Post("/v1/agents", body.dump(), "application/json");
  ASSERT_TRUE(res);
  // Should succeed (no size limit configured) or at worst return 413
  EXPECT_TRUE(res->status == 201 || res->status == 413);
}

// ── GET /v1/teams/:id non-existent ────────────────────────────────────────────

TEST_F(RoutesTest, GetTeamNonExistent) {
  auto res = client_->Get("/v1/teams/ghost-team-id");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── DELETE non-existent team ──────────────────────────────────────────────────

TEST_F(RoutesTest, DeleteNonExistentTeam) {
  auto res = client_->Delete("/v1/teams/ghost-team-id");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── GET non-existent task ─────────────────────────────────────────────────────

TEST_F(RoutesTest, GetTaskNonExistent) {
  auto res = client_->Get("/v1/teams/ghost-team/tasks/ghost-task");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── POST task with large payload ──────────────────────────────────────────────

TEST_F(RoutesTest, PostTaskLargePayload) {
  // Create a team first
  auto team_res = client_->Post("/v1/teams", R"({"name":"load-team"})", "application/json");
  ASSERT_TRUE(team_res);
  ASSERT_EQ(team_res->status, 201);
  std::string team_id = json::parse(team_res->body)["team"]["id"];

  std::string big(1024 * 1024, 'w');
  json payload = {{"subject", "big-task"}, {"description", big}};
  auto res = client_->Post("/v1/teams/" + team_id + "/tasks", payload.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_TRUE(res->status == 201 || res->status == 413);
}

// ── POST /v1/chaos/:type with arbitrary type ──────────────────────────────────

TEST_F(RoutesTest, ChaosUnknownType) {
  // Any string is accepted as type
  auto res = client_->Post("/v1/chaos/unknowntype", "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
  json body = json::parse(res->body);
  EXPECT_EQ(body["fault"]["type"], "unknowntype");
}

TEST_F(RoutesTest, ChaosMalformedTypeWithSpecialChars) {
  // cpp-httplib regex [^/]+ matches everything except slash
  auto res = client_->Post("/v1/chaos/type-with-dashes_and.dots", "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
}

// ── DELETE /v1/chaos non-existent fault ───────────────────────────────────────

TEST_F(RoutesTest, DeleteNonExistentFault) {
  auto res = client_->Delete("/v1/chaos/no-such-fault");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── Wrong HTTP methods ────────────────────────────────────────────────────────

TEST_F(RoutesTest, DeleteAgentsListNotAllowed) {
  // DELETE /v1/agents (no ID) — no route registered for this, should 404
  auto res = client_->Delete("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_TRUE(res->status == 404 || res->status == 405);
}

TEST_F(RoutesTest, PutAgentsNotAllowed) {
  // PUT /v1/agents — no route registered
  auto res = client_->Put("/v1/agents", "{}", "application/json");
  ASSERT_TRUE(res);
  EXPECT_TRUE(res->status == 404 || res->status == 405);
}

// ── Round-trip: create then retrieve ─────────────────────────────────────────

TEST_F(RoutesTest, CreateAndGetAgent) {
  auto create_res = client_->Post("/v1/agents", R"({"name":"alice"})", "application/json");
  ASSERT_TRUE(create_res);
  ASSERT_EQ(create_res->status, 201);
  std::string agent_id = json::parse(create_res->body)["id"];

  auto get_res = client_->Get("/v1/agents/" + agent_id);
  ASSERT_TRUE(get_res);
  EXPECT_EQ(get_res->status, 200);
  EXPECT_EQ(json::parse(get_res->body)["agent"]["name"], "alice");
}

TEST_F(RoutesTest, CreateAndDeleteAgent) {
  auto create_res = client_->Post("/v1/agents", R"({"name":"bob"})", "application/json");
  ASSERT_TRUE(create_res);
  ASSERT_EQ(create_res->status, 201);
  std::string agent_id = json::parse(create_res->body)["id"];

  auto del_res = client_->Delete("/v1/agents/" + agent_id);
  ASSERT_TRUE(del_res);
  EXPECT_EQ(del_res->status, 200);

  // Second delete → 404
  auto del2_res = client_->Delete("/v1/agents/" + agent_id);
  ASSERT_TRUE(del2_res);
  EXPECT_EQ(del2_res->status, 404);
}

// ── PATCH agent with malformed JSON ───────────────────────────────────────────

TEST_F(RoutesTest, PatchAgentMalformedJSON) {
  auto create_res = client_->Post("/v1/agents", R"({"name":"charlie"})", "application/json");
  ASSERT_TRUE(create_res);
  std::string agent_id = json::parse(create_res->body)["id"];

  auto patch_res = client_->Patch("/v1/agents/" + agent_id, "{bad", "application/json");
  ASSERT_TRUE(patch_res);
  EXPECT_EQ(patch_res->status, 400);
}

// ── POST /v1/teams/:id/tasks with malformed JSON ──────────────────────────────

TEST_F(RoutesTest, PostTaskMalformedJSON) {
  auto team_res = client_->Post("/v1/teams", R"({"name":"t"})", "application/json");
  ASSERT_TRUE(team_res);
  std::string team_id = json::parse(team_res->body)["team"]["id"];

  auto res = client_->Post("/v1/teams/" + team_id + "/tasks", "{bad json", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

// ── start/stop non-existent agent via HTTP ────────────────────────────────────

TEST_F(RoutesTest, StartNonExistentAgentHttp) {
  auto res = client_->Post("/v1/agents/ghost-agent/start", "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

TEST_F(RoutesTest, StopNonExistentAgentHttp) {
  auto res = client_->Post("/v1/agents/ghost-agent/stop", "", "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// ── GET /v1/workflows ─────────────────────────────────────────────────────────
// Endpoint removed; coverage lives in test_main.cpp::RoutesRemovedTest.WorkflowsEndpointRemoved.

}  // namespace projectagamemnon::test
