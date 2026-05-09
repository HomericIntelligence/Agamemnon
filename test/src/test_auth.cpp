#include "projectagamemnon/auth.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"
#include <gtest/gtest.h>

namespace projectagamemnon::test {

namespace {

httplib::Request make_request(const std::string& path) {
  httplib::Request req;
  req.path = path;
  return req;
}

}  // namespace

class AuthMiddlewareTest : public ::testing::Test {
 protected:
  AuthMiddleware auth_{"secret-key"};
};

// ── is_exempt ────────────────────────────────────────────────────────────────

TEST_F(AuthMiddlewareTest, ExemptHealth) { EXPECT_TRUE(auth_.is_exempt("/health")); }

TEST_F(AuthMiddlewareTest, ExemptV1Health) { EXPECT_TRUE(auth_.is_exempt("/v1/health")); }

TEST_F(AuthMiddlewareTest, NotExemptAgents) { EXPECT_FALSE(auth_.is_exempt("/v1/agents")); }

TEST_F(AuthMiddlewareTest, NotExemptChaos) { EXPECT_FALSE(auth_.is_exempt("/v1/chaos/network")); }

TEST_F(AuthMiddlewareTest, NotExemptTasks) { EXPECT_FALSE(auth_.is_exempt("/v1/tasks")); }

TEST_F(AuthMiddlewareTest, NotExemptVersion) { EXPECT_FALSE(auth_.is_exempt("/v1/version")); }

// ── validate: exempt paths ────────────────────────────────────────────────────

TEST_F(AuthMiddlewareTest, ValidateExemptHealthNoHeader) {
  auto req = make_request("/health");
  EXPECT_TRUE(auth_.validate(req));
}

TEST_F(AuthMiddlewareTest, ValidateExemptV1HealthNoHeader) {
  auto req = make_request("/v1/health");
  EXPECT_TRUE(auth_.validate(req));
}

// ── validate: Authorization: Bearer header ────────────────────────────────────

TEST_F(AuthMiddlewareTest, ValidateBearerCorrectKey) {
  auto req = make_request("/v1/agents");
  req.set_header("Authorization", "Bearer secret-key");
  EXPECT_TRUE(auth_.validate(req));
}

TEST_F(AuthMiddlewareTest, ValidateBearerWrongKey) {
  auto req = make_request("/v1/agents");
  req.set_header("Authorization", "Bearer wrong-key");
  EXPECT_FALSE(auth_.validate(req));
}

TEST_F(AuthMiddlewareTest, ValidateBearerEmptyKey) {
  auto req = make_request("/v1/agents");
  req.set_header("Authorization", "Bearer ");
  EXPECT_FALSE(auth_.validate(req));
}

TEST_F(AuthMiddlewareTest, ValidateBearerMalformedNoSpace) {
  auto req = make_request("/v1/agents");
  req.set_header("Authorization", "Bearersecret-key");
  EXPECT_FALSE(auth_.validate(req));
}

// ── validate: X-API-Key header ────────────────────────────────────────────────

TEST_F(AuthMiddlewareTest, ValidateXApiKeyCorrectKey) {
  auto req = make_request("/v1/agents");
  req.set_header("X-API-Key", "secret-key");
  EXPECT_TRUE(auth_.validate(req));
}

TEST_F(AuthMiddlewareTest, ValidateXApiKeyWrongKey) {
  auto req = make_request("/v1/agents");
  req.set_header("X-API-Key", "wrong-key");
  EXPECT_FALSE(auth_.validate(req));
}

TEST_F(AuthMiddlewareTest, ValidateXApiKeyEmpty) {
  auto req = make_request("/v1/agents");
  req.set_header("X-API-Key", "");
  EXPECT_FALSE(auth_.validate(req));
}

// ── validate: no auth header ─────────────────────────────────────────────────

TEST_F(AuthMiddlewareTest, ValidateNoHeader) {
  auto req = make_request("/v1/agents");
  EXPECT_FALSE(auth_.validate(req));
}

TEST_F(AuthMiddlewareTest, ValidateNoHeaderChaos) {
  auto req = make_request("/v1/chaos/network");
  EXPECT_FALSE(auth_.validate(req));
}

// ── validate: chaos endpoints also need auth ─────────────────────────────────

TEST_F(AuthMiddlewareTest, ValidateChaosWithCorrectKey) {
  auto req = make_request("/v1/chaos/network");
  req.set_header("X-API-Key", "secret-key");
  EXPECT_TRUE(auth_.validate(req));
}

}  // namespace projectagamemnon::test
