// #164: Test that internal fields like _github_issue do not leak into REST API responses
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "agamemnon/auth.hpp"
#include "agamemnon/github_client.hpp"  // MockGitHubClient
#include "agamemnon/metrics.hpp"
#include "agamemnon/nats_client.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/rate_limiter.hpp"
#include "agamemnon/routes.hpp"
#include "agamemnon/store.hpp"

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace agamemnon::test {
using json = nlohmann::json;

// Recursive walk: returns true if `node` (or anything reachable from it)
// is an object containing the key "_github_issue". Used to assert that
// response payloads are clean of the internal field anywhere in the tree.
static bool contains_github_issue(const json& node) {
  if (node.is_object()) {
    if (node.contains("_github_issue")) return true;
    for (auto it = node.begin(); it != node.end(); ++it) {
      if (contains_github_issue(it.value())) return true;
    }
  } else if (node.is_array()) {
    for (const auto& v : node)
      if (contains_github_issue(v)) return true;
  }
  return false;
}

class RoutesNoInternalLeakTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_ = std::make_shared<MockGitHubClient>();
    store_ = std::make_unique<Store>(mock_);  // <-- key injection (Decision 4)
    orchestrator_ = std::make_unique<Orchestrator>(*store_, nats_);
    register_routes(server_, *store_, nats_, rate_limiter_, auth_, metrics_, *orchestrator_);
    int port = server_.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
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

  std::shared_ptr<MockGitHubClient> mock_;
  std::unique_ptr<Store> store_;
  NatsClient nats_{"nats://127.0.0.1:14222"};
  RateLimiter rate_limiter_{1e9, 1e9};
  AuthMiddleware auth_{""};
  MetricsRegistry metrics_;
  std::unique_ptr<Orchestrator> orchestrator_;
  httplib::Server server_;
  std::thread server_thread_;
  std::unique_ptr<httplib::Client> client_;
};

// Anti-vacuity guard: prove the mock is actually wired so Store writes
// _github_issue. If this fails, every later assertion is meaningless and
// the test will be obviously broken instead of silently green.
TEST_F(RoutesNoInternalLeakTest, FixtureActuallySeedsGithubIssueIntoStore) {
  auto create =
      client_->Post("/v1/agents", json{{"name", "seed-agent"}}.dump(), "application/json");
  ASSERT_TRUE(create);
  ASSERT_EQ(create->status, 201);
  auto id = json::parse(create->body)["id"].get<std::string>();
  json stored = store_->get_agent(id);  // bypass HTTP — read raw store
  ASSERT_TRUE(stored.contains("_github_issue"))
      << "MockGitHubClient not wired — test would pass vacuously";
}

TEST_F(RoutesNoInternalLeakTest, AgentResponsesNeverLeakGithubIssue) {
  auto post = client_->Post("/v1/agents", json{{"name", "a1"}}.dump(), "application/json");
  ASSERT_EQ(post->status, 201);
  auto body = json::parse(post->body);
  EXPECT_FALSE(contains_github_issue(body)) << "POST /v1/agents leaked";
  auto id = body["id"].get<std::string>();

  auto get_one = client_->Get("/v1/agents/" + id);
  EXPECT_FALSE(contains_github_issue(json::parse(get_one->body))) << "GET /v1/agents/:id leaked";

  auto get_by_name = client_->Get("/v1/agents/by-name/a1");
  EXPECT_FALSE(contains_github_issue(json::parse(get_by_name->body)))
      << "GET /v1/agents/by-name/:name leaked";

  auto list = client_->Get("/v1/agents");
  EXPECT_FALSE(contains_github_issue(json::parse(list->body))) << "GET /v1/agents leaked";

  auto patch =
      client_->Patch("/v1/agents/" + id, json{{"label", "updated"}}.dump(), "application/json");
  EXPECT_FALSE(contains_github_issue(json::parse(patch->body))) << "PATCH /v1/agents/:id leaked";
}

TEST_F(RoutesNoInternalLeakTest, TeamResponsesNeverLeakGithubIssue) {
  auto post = client_->Post("/v1/teams", json{{"name", "t1"}}.dump(), "application/json");
  ASSERT_EQ(post->status, 201);
  auto body = json::parse(post->body);
  EXPECT_FALSE(contains_github_issue(body));
  auto id = body["team"]["id"].get<std::string>();
  EXPECT_FALSE(contains_github_issue(json::parse(client_->Get("/v1/teams")->body)));
  EXPECT_FALSE(contains_github_issue(json::parse(client_->Get("/v1/teams/" + id)->body)));
  // Teams expose PUT /v1/teams/:id for updates (no PATCH route); use PUT here.
  auto put = client_->Put("/v1/teams/" + id, json{{"name", "renamed"}}.dump(), "application/json");
  EXPECT_FALSE(contains_github_issue(json::parse(put->body)));
}

TEST_F(RoutesNoInternalLeakTest, TaskResponsesNeverLeakGithubIssue) {
  auto team =
      json::parse(client_->Post("/v1/teams", json{{"name", "t"}}.dump(), "application/json")->body);
  auto team_id = team["team"]["id"].get<std::string>();
  auto post = client_->Post("/v1/teams/" + team_id + "/tasks",
                            json{{"subject", "s"}, {"type", "general"}}.dump(), "application/json");
  ASSERT_EQ(post->status, 201);
  auto task = json::parse(post->body);
  EXPECT_FALSE(contains_github_issue(task));
  auto task_id = task["task"]["id"].get<std::string>();
  EXPECT_FALSE(contains_github_issue(json::parse(client_->Get("/v1/tasks")->body)));
  EXPECT_FALSE(
      contains_github_issue(json::parse(client_->Get("/v1/teams/" + team_id + "/tasks")->body)));
  EXPECT_FALSE(contains_github_issue(
      json::parse(client_->Get("/v1/teams/" + team_id + "/tasks/" + task_id)->body)));
  auto patch = client_->Patch("/v1/teams/" + team_id + "/tasks/" + task_id,
                              json{{"status", "in_progress"}}.dump(), "application/json");
  EXPECT_FALSE(contains_github_issue(json::parse(patch->body)));
}

TEST_F(RoutesNoInternalLeakTest, ChaosResponsesNeverLeakGithubIssue) {
  auto post = client_->Post("/v1/chaos/latency", std::string{}, "application/json");
  ASSERT_EQ(post->status, 201);
  EXPECT_FALSE(contains_github_issue(json::parse(post->body)));
  EXPECT_FALSE(contains_github_issue(json::parse(client_->Get("/v1/chaos")->body)));
}

}  // namespace agamemnon::test
