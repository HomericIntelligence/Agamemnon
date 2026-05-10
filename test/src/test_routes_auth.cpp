#include "projectagamemnon/auth.hpp"
#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/rate_limiter.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <atomic>
#include <thread>

#include "httplib.h"
#include <gtest/gtest.h>

namespace projectagamemnon::test {

// Starts a real httplib::Server in a background thread for integration tests.
// The server is stopped and the thread joined in TearDown.
class RoutesAuthTest : public ::testing::Test {
 protected:
  static constexpr int kPort = 18065;
  static constexpr const char* kKey = "test-api-key";

  void SetUp() override {
    auth_ = std::make_unique<AuthMiddleware>(kKey);
    store_ = std::make_unique<Store>();
    nats_ = std::make_unique<NatsClient>("nats://localhost:4222");

    register_routes(server_, *store_, *nats_, rate_limiter_, *auth_);

    server_thread_ = std::thread([this] { server_.listen("127.0.0.1", kPort); });

    // Wait until the server is actually listening before tests run.
    for (int i = 0; i < 50; ++i) {
      if (server_.is_running()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  void TearDown() override {
    server_.stop();
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
  }

  // Helpers for making authenticated / unauthenticated requests.
  httplib::Result get_no_auth(const std::string& path) {
    httplib::Client cli("127.0.0.1", kPort);
    return cli.Get(path);
  }

  httplib::Result get_with_key(const std::string& path) {
    httplib::Client cli("127.0.0.1", kPort);
    return cli.Get(path, {{"X-API-Key", kKey}});
  }

  httplib::Result get_with_bearer(const std::string& path) {
    httplib::Client cli("127.0.0.1", kPort);
    return cli.Get(path, {{"Authorization", std::string("Bearer ") + kKey}});
  }

  httplib::Result get_with_wrong_key(const std::string& path) {
    httplib::Client cli("127.0.0.1", kPort);
    return cli.Get(path, {{"X-API-Key", "wrong-key"}});
  }

  httplib::Server server_;
  std::thread server_thread_;
  RateLimiter rate_limiter_{1e9, 1e9};  // effectively unlimited for auth tests
  std::unique_ptr<AuthMiddleware> auth_;
  std::unique_ptr<Store> store_;
  std::unique_ptr<NatsClient> nats_;
};

// ── Health endpoints: exempt from auth ───────────────────────────────────────

TEST_F(RoutesAuthTest, HealthNoAuth) {
  auto res = get_no_auth("/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(RoutesAuthTest, V1HealthNoAuth) {
  auto res = get_no_auth("/v1/health");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

// ── Protected endpoints: 401 without credentials ─────────────────────────────

TEST_F(RoutesAuthTest, AgentsListUnauthorized) {
  auto res = get_no_auth("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(RoutesAuthTest, TeamsListUnauthorized) {
  auto res = get_no_auth("/v1/teams");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(RoutesAuthTest, TasksListUnauthorized) {
  auto res = get_no_auth("/v1/tasks");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(RoutesAuthTest, ChaosListUnauthorized) {
  auto res = get_no_auth("/v1/chaos");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(RoutesAuthTest, WorkflowsListUnauthorized) {
  auto res = get_no_auth("/v1/workflows");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(RoutesAuthTest, VersionUnauthorized) {
  auto res = get_no_auth("/v1/version");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

// ── Protected endpoints: 401 with wrong key ───────────────────────────────────

TEST_F(RoutesAuthTest, AgentsListWrongKey) {
  auto res = get_with_wrong_key("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

TEST_F(RoutesAuthTest, ChaosListWrongKey) {
  auto res = get_with_wrong_key("/v1/chaos");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
}

// ── Protected endpoints: 200 with valid X-API-Key ─────────────────────────────

TEST_F(RoutesAuthTest, AgentsListWithApiKey) {
  auto res = get_with_key("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(RoutesAuthTest, TeamsListWithApiKey) {
  auto res = get_with_key("/v1/teams");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(RoutesAuthTest, TasksListWithApiKey) {
  auto res = get_with_key("/v1/tasks");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(RoutesAuthTest, ChaosListWithApiKey) {
  auto res = get_with_key("/v1/chaos");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(RoutesAuthTest, WorkflowsListWithApiKey) {
  auto res = get_with_key("/v1/workflows");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

// ── Protected endpoints: 200 with valid Bearer token ─────────────────────────

TEST_F(RoutesAuthTest, AgentsListWithBearer) {
  auto res = get_with_bearer("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

TEST_F(RoutesAuthTest, TeamsListWithBearer) {
  auto res = get_with_bearer("/v1/teams");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
}

// ── 401 response body is valid JSON ──────────────────────────────────────────

TEST_F(RoutesAuthTest, UnauthorizedResponseIsJson) {
  auto res = get_no_auth("/v1/agents");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 401);
  EXPECT_EQ(res->get_header_value("Content-Type"), "application/json");
  EXPECT_FALSE(res->body.empty());
}

}  // namespace projectagamemnon::test
