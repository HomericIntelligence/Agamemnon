#include "agamemnon/auth.hpp"
#include "agamemnon/metrics.hpp"
#include "agamemnon/nats_client.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/rate_limiter.hpp"
#include "agamemnon/routes.hpp"
#include "agamemnon/store.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace agamemnon::test {

using json = nlohmann::json;

// ── Fixture ───────────────────────────────────────────────────────────────────

class ValidationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    nats_ = std::make_unique<NatsClient>("nats://127.0.0.1:4222");
    // NatsClient is intentionally not connected; publish() is a no-op when disconnected.
    orchestrator_ = std::make_unique<Orchestrator>(store_, *nats_);

    register_routes(server_, store_, *nats_, rate_limiter_, auth_, metrics_, *orchestrator_);

    port_ = server_.bind_to_any_port("127.0.0.1");
    server_thread_ = std::thread([this] { server_.listen_after_bind(); });

    // Give the server a moment to accept connections after bind.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  void TearDown() override {
    server_.stop();
    if (server_thread_.joinable()) server_thread_.join();
  }

  // POST helper — returns {status_code, body_json}
  std::pair<int, json> Post(const std::string& path, const json& body) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Post(path, body.dump(), "application/json");
    EXPECT_TRUE(res != nullptr) << "HTTP request failed for " << path;
    if (!res) return {-1, {}};
    return {res->status, json::parse(res->body, nullptr, false)};
  }

  // PATCH helper
  std::pair<int, json> Patch(const std::string& path, const json& body) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Patch(path, body.dump(), "application/json");
    EXPECT_TRUE(res != nullptr) << "HTTP request failed for " << path;
    if (!res) return {-1, {}};
    return {res->status, json::parse(res->body, nullptr, false)};
  }

  // PUT helper
  std::pair<int, json> Put(const std::string& path, const json& body) {
    httplib::Client cli("127.0.0.1", port_);
    auto res = cli.Put(path, body.dump(), "application/json");
    EXPECT_TRUE(res != nullptr) << "HTTP request failed for " << path;
    if (!res) return {-1, {}};
    return {res->status, json::parse(res->body, nullptr, false)};
  }

  // Convenience: create an agent and return its id
  std::string CreateAgent(const std::string& name = "test-agent") {
    auto [status, body] = Post("/v1/agents", {{"name", name}});
    EXPECT_EQ(status, 201);
    return body.value("id", "");
  }

  // Convenience: create a team and return its id
  std::string CreateTeam(const std::string& name = "test-team") {
    auto [status, body] = Post("/v1/teams", {{"name", name}});
    EXPECT_EQ(status, 201);
    return body["team"].value("id", "");
  }

  httplib::Server server_;
  Store store_;
  std::unique_ptr<NatsClient> nats_;
  RateLimiter rate_limiter_{1e9, 1e9};  // effectively unlimited for tests
  AuthMiddleware auth_{""};             // empty key — all requests pass auth in test
  MetricsRegistry metrics_;
  std::unique_ptr<Orchestrator> orchestrator_;
  std::thread server_thread_;
  int port_ = 0;
};

// ── Agent validation ──────────────────────────────────────────────────────────

TEST_F(ValidationTest, AgentRejectsEmptyName) {
  auto [status, body] = Post("/v1/agents", {{"name", ""}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, AgentRejectsWhitespaceOnlyName) {
  auto [status, body] = Post("/v1/agents", {{"name", "   "}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, AgentAcceptsValidName) {
  auto [status, body] = Post("/v1/agents", {{"name", "worker-1"}});
  EXPECT_EQ(status, 201);
}

TEST_F(ValidationTest, AgentDockerRejectsEmptyName) {
  // Issue #144: /v1/agents/docker removed; docker agents use POST /v1/agents with host=docker.
  auto [status, body] = Post("/v1/agents", {{"name", ""}, {"host", "docker"}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, AgentDockerAcceptsValidName) {
  auto [status, body] = Post("/v1/agents", {{"name", "docker-1"}, {"host", "docker"}});
  EXPECT_EQ(status, 201);
}

TEST_F(ValidationTest, AgentPatchRejectsEmptyName) {
  std::string id = CreateAgent();
  auto [status, body] = Patch("/v1/agents/" + id, {{"name", ""}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, AgentPatchRejectsInvalidStatus) {
  std::string id = CreateAgent();
  auto [status, body] = Patch("/v1/agents/" + id, {{"status", "flying"}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, AgentPatchAcceptsValidStatus) {
  std::string id = CreateAgent();
  auto [status, body] = Patch("/v1/agents/" + id, {{"status", "online"}});
  EXPECT_EQ(status, 200);
}

TEST_F(ValidationTest, AgentPatchAcceptsAllValidStatuses) {
  for (const std::string& s : {"offline", "online", "error"}) {
    std::string id = CreateAgent("agent-for-" + s);
    auto [status, body] = Patch("/v1/agents/" + id, {{"status", s}});
    EXPECT_EQ(status, 200) << "expected 200 for status=" << s;
  }
}

// ── Team validation ───────────────────────────────────────────────────────────

TEST_F(ValidationTest, TeamRejectsEmptyName) {
  auto [status, body] = Post("/v1/teams", {{"name", ""}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, TeamAcceptsValidName) {
  auto [status, body] = Post("/v1/teams", {{"name", "alpha-team"}});
  EXPECT_EQ(status, 201);
}

TEST_F(ValidationTest, TeamRejectsNonStringAgentId) {
  auto [status, body] = Post("/v1/teams", {{"name", "t1"}, {"agentIds", json::array({42})}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TeamAcceptsStringAgentIds) {
  auto [status, body] =
      Post("/v1/teams", {{"name", "t2"}, {"agentIds", json::array({"id-1", "id-2"})}});
  EXPECT_EQ(status, 201);
}

TEST_F(ValidationTest, TeamPutRejectsEmptyName) {
  std::string id = CreateTeam();
  auto [status, body] = Put("/v1/teams/" + id, {{"name", ""}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TeamPutRejectsNonStringAgentIdSnakeCase) {
  std::string id = CreateTeam();
  auto [status, body] = Put("/v1/teams/" + id, {{"name", "ok"}, {"agent_ids", json::array({99})}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TeamPutAcceptsValidUpdate) {
  std::string id = CreateTeam();
  auto [status, body] = Put("/v1/teams/" + id, {{"name", "new-name"}});
  EXPECT_EQ(status, 200);
}

// ── Task validation ───────────────────────────────────────────────────────────

TEST_F(ValidationTest, TaskRejectsEmptySubject) {
  std::string team_id = CreateTeam();
  auto [status, body] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", ""}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TaskRejectsInvalidType) {
  std::string team_id = CreateTeam();
  auto [status, body] =
      Post("/v1/teams/" + team_id + "/tasks", {{"subject", "do it"}, {"type", "unknown-type"}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TaskAcceptsValidType) {
  std::string team_id = CreateTeam();
  auto [status, body] =
      Post("/v1/teams/" + team_id + "/tasks", {{"subject", "do it"}, {"type", "research"}});
  EXPECT_EQ(status, 201);
}

TEST_F(ValidationTest, TaskAcceptsAllValidTypes) {
  std::string team_id = CreateTeam();
  for (const std::string& t : {"general", "research", "implementation", "review", "testing"}) {
    auto [status, body] =
        Post("/v1/teams/" + team_id + "/tasks", {{"subject", "task-" + t}, {"type", t}});
    EXPECT_EQ(status, 201) << "expected 201 for type=" << t;
  }
}

TEST_F(ValidationTest, TaskRejectsNonStringBlockedByElement) {
  std::string team_id = CreateTeam();
  auto [status, body] = Post("/v1/teams/" + team_id + "/tasks",
                             {{"subject", "s"}, {"blockedBy", json::array({123})}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TaskAcceptsStringBlockedByArray) {
  std::string team_id = CreateTeam();
  auto [status, body] = Post("/v1/teams/" + team_id + "/tasks",
                             {{"subject", "s"}, {"blockedBy", json::array({"task-id-1"})}});
  EXPECT_EQ(status, 201);
}

TEST_F(ValidationTest, TaskPutRejectsInvalidStatus) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] = Put("/v1/teams/" + team_id + "/tasks/" + task_id, {{"status", "flying"}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TaskPutAcceptsValidStatus) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] =
      Put("/v1/teams/" + team_id + "/tasks/" + task_id, {{"status", "completed"}});
  EXPECT_EQ(status, 200);
}

TEST_F(ValidationTest, TaskPutAcceptsAllValidStatuses) {
  std::string team_id = CreateTeam();
  for (const std::string& s : {"pending", "running", "completed", "failed", "blocked"}) {
    auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "task-" + s}});
    ASSERT_EQ(s1, 201);
    std::string task_id = b1["task"].value("id", "");
    auto [status, body] = Put("/v1/teams/" + team_id + "/tasks/" + task_id, {{"status", s}});
    EXPECT_EQ(status, 200) << "expected 200 for status=" << s;
  }
}

TEST_F(ValidationTest, TaskPatchRejectsInvalidStatus) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] =
      Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"status", "unknown"}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TaskPatchAcceptsValidStatus) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] =
      Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"status", "running"}});
  EXPECT_EQ(status, 200);
}

TEST_F(ValidationTest, TaskPatchRejectsInvalidType) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] = Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"type", "bogus"}});
  EXPECT_EQ(status, 400);
}

TEST_F(ValidationTest, TaskPatchRejectsNonStringBlockedByElement) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] =
      Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"blockedBy", json::array({true})}});
  EXPECT_EQ(status, 400);
}

// ── Non-string type validation ────────────────────────────────────────────────

TEST_F(ValidationTest, AgentRejectsNonStringLabel) {
  auto [status, body] = Post("/v1/agents", {{"name", "test"}, {"label", 123}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, AgentRejectsNonStringProgram) {
  auto [status, body] = Post("/v1/agents", {{"name", "test"}, {"program", json::object()}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, AgentRejectsNonStringTaskDescription) {
  auto [status, body] = Post("/v1/agents", {{"name", "test"}, {"taskDescription", json::array()}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, TaskRejectsNonStringSubject) {
  std::string team_id = CreateTeam();
  auto [status, body] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", 42}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, TaskRejectsNonStringDescription) {
  std::string team_id = CreateTeam();
  auto [status, body] = Post("/v1/teams/" + team_id + "/tasks",
                             {{"subject", "s"}, {"description", json::array({1, 2})}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, TaskPutRejectsNonStringSubject) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] = Put("/v1/teams/" + team_id + "/tasks/" + task_id, {{"subject", true}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, TaskPutRejectsNonStringDescription) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] = Put("/v1/teams/" + team_id + "/tasks/" + task_id, {{"description", 42}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, TaskPatchRejectsNonStringSubject) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] =
      Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"subject", json::object()}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, TaskPatchRejectsNonStringDescription) {
  std::string team_id = CreateTeam();
  auto [s1, b1] = Post("/v1/teams/" + team_id + "/tasks", {{"subject", "s"}});
  ASSERT_EQ(s1, 201);
  std::string task_id = b1["task"].value("id", "");

  auto [status, body] = Patch("/v1/teams/" + team_id + "/tasks/" + task_id, {{"description", 99}});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

// ── Chaos validation ──────────────────────────────────────────────────────────

TEST_F(ValidationTest, ChaosRejectsUnknownType) {
  auto [status, body] = Post("/v1/chaos/explode", {});
  EXPECT_EQ(status, 400);
  EXPECT_TRUE(body.contains("error"));
}

TEST_F(ValidationTest, ChaosAcceptsValidType) {
  auto [status, body] = Post("/v1/chaos/latency", {});
  EXPECT_EQ(status, 201);
}

TEST_F(ValidationTest, ChaosAcceptsAllValidTypes) {
  for (const std::string& t : {"latency", "partition", "crash", "corruption", "throttle"}) {
    auto [status, body] = Post("/v1/chaos/" + t, {});
    EXPECT_EQ(status, 201) << "expected 201 for chaos type=" << t;
  }
}

}  // namespace agamemnon::test
