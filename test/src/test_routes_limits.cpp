#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace projectagamemnon::test {

using json = nlohmann::json;

// ── Fixture ───────────────────────────────────────────────────────────────────

class RoutesLimitsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    port_ = svr_.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port_, 0);
    register_routes(svr_, store_, nats_);
    thread_ = std::thread([this] { svr_.listen_after_bind(); });
  }

  void TearDown() override {
    svr_.stop();
    if (thread_.joinable()) thread_.join();
  }

  httplib::Client client() const {
    httplib::Client c("127.0.0.1", port_);
    c.set_connection_timeout(5);
    return c;
  }

  Store store_;
  NatsClient nats_{"nats://127.0.0.1:14222"};  // unreachable — publishes are no-ops
  httplib::Server svr_;
  int port_{0};
  std::thread thread_;
};

// ── Transport-layer body size limit ──────────────────────────────────────────

TEST_F(RoutesLimitsTest, OversizedBodyRejected) {
  // Body just over the 1 MB limit should be rejected with 413.
  std::string huge(static_cast<std::size_t>(1 << 20) + 1, 'A');
  auto c = client();
  auto res = c.Post("/v1/agents", huge, "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 413);
}

// ── Agent field limits ────────────────────────────────────────────────────────

TEST_F(RoutesLimitsTest, AgentNameTooLong) {
  json body{{"name", std::string(257, 'x')}, {"type", "worker"}};
  auto c = client();
  auto res = c.Post("/v1/agents", body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  EXPECT_NE(res->body.find("name"), std::string::npos);
}

TEST_F(RoutesLimitsTest, AgentNameAtLimit) {
  // Exactly 256 chars should be accepted (201).
  json body{{"name", std::string(256, 'x')}, {"type", "worker"}};
  auto c = client();
  auto res = c.Post("/v1/agents", body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
}

TEST_F(RoutesLimitsTest, AgentLabelTooLong) {
  json body{{"name", "ok"}, {"label", std::string(257, 'l')}, {"type", "worker"}};
  auto c = client();
  auto res = c.Post("/v1/agents", body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  EXPECT_NE(res->body.find("label"), std::string::npos);
}

TEST_F(RoutesLimitsTest, AgentProgramTooLong) {
  json body{{"name", "ok"}, {"program", std::string(1025, 'p')}, {"type", "worker"}};
  auto c = client();
  auto res = c.Post("/v1/agents", body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(RoutesLimitsTest, AgentTaskDescriptionTooLong) {
  json body{{"name", "ok"}, {"taskDescription", std::string(4097, 'd')}, {"type", "worker"}};
  auto c = client();
  auto res = c.Post("/v1/agents", body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  EXPECT_NE(res->body.find("taskDescription"), std::string::npos);
}

TEST_F(RoutesLimitsTest, DockerAgentNameTooLong) {
  json body{{"name", std::string(257, 'x')}, {"type", "docker"}};
  auto c = client();
  auto res = c.Post("/v1/agents/docker", body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

TEST_F(RoutesLimitsTest, PatchAgentNameTooLong) {
  // First create an agent to patch.
  json create_body{{"name", "patch-target"}, {"type", "worker"}};
  auto c = client();
  auto create_res = c.Post("/v1/agents", create_body.dump(), "application/json");
  ASSERT_TRUE(create_res);
  ASSERT_EQ(create_res->status, 201);

  std::string agent_id = json::parse(create_res->body)["agent"]["id"].get<std::string>();
  json patch_body{{"name", std::string(257, 'x')}};
  auto res = c.Patch("/v1/agents/" + agent_id, patch_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  EXPECT_NE(res->body.find("name"), std::string::npos);
}

TEST_F(RoutesLimitsTest, PatchAgentMissingNameSkipsCheck) {
  // PATCH without 'name' field must not produce a false positive.
  json create_body{{"name", "patch-no-name"}, {"type", "worker"}};
  auto c = client();
  auto create_res = c.Post("/v1/agents", create_body.dump(), "application/json");
  ASSERT_TRUE(create_res);
  ASSERT_EQ(create_res->status, 201);

  std::string agent_id = json::parse(create_res->body)["agent"]["id"].get<std::string>();
  json patch_body{{"status", "idle"}};
  auto res = c.Patch("/v1/agents/" + agent_id, patch_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

// ── Task field limits ─────────────────────────────────────────────────────────

TEST_F(RoutesLimitsTest, TaskSubjectTooLong) {
  // Create a team first.
  json team_body{{"name", "test-team"}};
  auto c = client();
  auto team_res = c.Post("/v1/teams", team_body.dump(), "application/json");
  ASSERT_TRUE(team_res);
  ASSERT_EQ(team_res->status, 201);
  std::string team_id = json::parse(team_res->body)["team"]["id"].get<std::string>();

  json task_body{{"subject", std::string(513, 's')}, {"description", "ok"}};
  auto res = c.Post("/v1/teams/" + team_id + "/tasks", task_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  EXPECT_NE(res->body.find("subject"), std::string::npos);
}

TEST_F(RoutesLimitsTest, TaskSubjectAtLimit) {
  json team_body{{"name", "test-team-2"}};
  auto c = client();
  auto team_res = c.Post("/v1/teams", team_body.dump(), "application/json");
  ASSERT_TRUE(team_res);
  ASSERT_EQ(team_res->status, 201);
  std::string team_id = json::parse(team_res->body)["team"]["id"].get<std::string>();

  json task_body{{"subject", std::string(512, 's')}, {"description", "ok"}};
  auto res = c.Post("/v1/teams/" + team_id + "/tasks", task_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);
}

TEST_F(RoutesLimitsTest, TaskDescriptionTooLong) {
  json team_body{{"name", "test-team-3"}};
  auto c = client();
  auto team_res = c.Post("/v1/teams", team_body.dump(), "application/json");
  ASSERT_TRUE(team_res);
  ASSERT_EQ(team_res->status, 201);
  std::string team_id = json::parse(team_res->body)["team"]["id"].get<std::string>();

  json task_body{{"subject", "ok"}, {"description", std::string(4097, 'd')}};
  auto res = c.Post("/v1/teams/" + team_id + "/tasks", task_body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  EXPECT_NE(res->body.find("description"), std::string::npos);
}

// ── Team name limits ──────────────────────────────────────────────────────────

TEST_F(RoutesLimitsTest, TeamNameTooLong) {
  json body{{"name", std::string(257, 'n')}};
  auto c = client();
  auto res = c.Post("/v1/teams", body.dump(), "application/json");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
  EXPECT_NE(res->body.find("name"), std::string::npos);
}

}  // namespace projectagamemnon::test
