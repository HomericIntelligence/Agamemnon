#define CPPHTTPLIB_NO_EXCEPTIONS
#include "route_test_fixture.hpp"

namespace projectagamemnon::test {

class RoutesAuthTest : public RouteTestFixture {
 public:
  static constexpr const char* kKey = "test-api-key";

  RoutesAuthTest() { api_key_ = kKey; }

 protected:
  // Preserve fresh-client-per-call semantics from test_routes_auth.cpp:47-65.
  // Cpp-httplib Client maintains a keep-alive socket across calls; reusing
  // client_ would change semantics for auth state assertions.
  httplib::Result get_no_auth(const std::string& path) {
    httplib::Client cli("127.0.0.1", port_);
    return cli.Get(path);
  }
  httplib::Result get_with_key(const std::string& path) {
    httplib::Client cli("127.0.0.1", port_);
    return cli.Get(path, {{"X-API-Key", kKey}});
  }
  httplib::Result get_with_bearer(const std::string& path) {
    httplib::Client cli("127.0.0.1", port_);
    return cli.Get(path, {{"Authorization", std::string("Bearer ") + kKey}});
  }
  httplib::Result get_with_wrong_key(const std::string& path) {
    httplib::Client cli("127.0.0.1", port_);
    return cli.Get(path, {{"X-API-Key", "wrong-key"}});
  }
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
