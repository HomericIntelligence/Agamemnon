#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <iomanip>
#include <sstream>
#include "projectagamemnon/github_webhook.hpp"

namespace projectagamemnon {

namespace {

// Helper to compute SHA-256 HMAC and return hex string.
std::string compute_hmac_sha256(std::string_view secret, std::string_view body) {
  unsigned int sig_len = 0;
  unsigned char sig[EVP_MAX_MD_SIZE];

  const EVP_MD* md = EVP_sha256();
  HMAC(md, secret.data(), static_cast<int>(secret.size()),
       reinterpret_cast<const unsigned char*>(body.data()), body.size(), sig, &sig_len);

  std::ostringstream oss;
  for (unsigned int i = 0; i < sig_len; ++i) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(sig[i]);
  }
  return oss.str();
}

}  // namespace

class GitHubWebhookTest : public ::testing::Test {};

TEST_F(GitHubWebhookTest, VerifyAcceptsCorrectSignature) {
  std::string secret = "dev";
  std::string body = R"({"action":"edited","issue":{"number":1}})";
  std::string hex_sig = compute_hmac_sha256(secret, body);
  std::string header = "sha256=" + hex_sig;

  EXPECT_TRUE(verify_github_signature(secret, header, body));
}

TEST_F(GitHubWebhookTest, VerifyRejectsTamperedBody) {
  std::string secret = "dev";
  std::string body = R"({"action":"edited","issue":{"number":1}})";
  std::string hex_sig = compute_hmac_sha256(secret, body);
  std::string header = "sha256=" + hex_sig;

  // Tamper with body.
  std::string tampered = body + " ";
  EXPECT_FALSE(verify_github_signature(secret, header, tampered));
}

TEST_F(GitHubWebhookTest, VerifyRejectsEmptySecret) {
  std::string body = R"({"action":"edited"})";
  std::string header = "sha256=0000000000000000000000000000000000000000000000000000000000000000";

  EXPECT_FALSE(verify_github_signature("", header, body));
}

TEST_F(GitHubWebhookTest, VerifyRejectsMissingPrefix) {
  std::string secret = "dev";
  std::string body = R"({"action":"edited"})";
  std::string hex_sig = compute_hmac_sha256(secret, body);

  // Missing "sha256=" prefix.
  EXPECT_FALSE(verify_github_signature(secret, hex_sig, body));
}

TEST_F(GitHubWebhookTest, VerifyRejectsWrongLengthHeader) {
  std::string secret = "dev";
  std::string body = R"({"action":"edited"})";

  // Header too short.
  EXPECT_FALSE(verify_github_signature(secret, "sha256=abc", body));

  // Header too long.
  EXPECT_FALSE(verify_github_signature(secret, "sha256=" + std::string(100, 'a'), body));
}

TEST_F(GitHubWebhookTest, NormalizeFiltersActionAssigned) {
  nlohmann::json payload = nlohmann::json::parse(R"({
    "action": "assigned",
    "issue": {"number": 1, "body": "test"}
  })");

  auto result = normalize_issues_event(payload);
  EXPECT_FALSE(result.has_value());
}

TEST_F(GitHubWebhookTest, NormalizeRoutesByFirstAgamemnonLabel) {
  nlohmann::json payload = nlohmann::json::parse(R"({
    "action": "opened",
    "issue": {
      "number": 42,
      "body": "test issue",
      "labels": [
        {"name": "other"},
        {"name": "agamemnon-agent"},
        {"name": "agamemnon-task"}
      ]
    }
  })");

  auto result = normalize_issues_event(payload);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->action, "opened");
  EXPECT_EQ(result->entity_label, "agamemnon-agent");
  EXPECT_EQ(result->issue_shape["number"], 42);
  EXPECT_EQ(result->issue_shape["body"], "test issue");
}

TEST_F(GitHubWebhookTest, NormalizePassesThroughUpdatedAt) {
  nlohmann::json payload = nlohmann::json::parse(R"({
    "action": "edited",
    "issue": {
      "number": 1,
      "body": "edited",
      "updated_at": "2026-06-04T12:00:00Z",
      "labels": [{"name": "agamemnon-task"}]
    }
  })");

  auto result = normalize_issues_event(payload);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->updated_at, "2026-06-04T12:00:00Z");
}

TEST_F(GitHubWebhookTest, NormalizeNulloptOnMissingIssueBody) {
  nlohmann::json payload = nlohmann::json::parse(R"({
    "action": "opened",
    "issue": {
      "number": 1,
      "labels": [{"name": "agamemnon-agent"}]
    }
  })");

  auto result = normalize_issues_event(payload);
  EXPECT_FALSE(result.has_value());
}

TEST_F(GitHubWebhookTest, NormalizeNulloptOnMissingNumber) {
  nlohmann::json payload = nlohmann::json::parse(R"({
    "action": "opened",
    "issue": {
      "body": "test",
      "labels": [{"name": "agamemnon-agent"}]
    }
  })");

  auto result = normalize_issues_event(payload);
  EXPECT_FALSE(result.has_value());
}

TEST_F(GitHubWebhookTest, NormalizeNulloptOnMissingAgamemnonLabel) {
  nlohmann::json payload = nlohmann::json::parse(R"({
    "action": "opened",
    "issue": {
      "number": 1,
      "body": "test",
      "labels": [{"name": "other-label"}]
    }
  })");

  auto result = normalize_issues_event(payload);
  EXPECT_FALSE(result.has_value());
}

TEST_F(GitHubWebhookTest, NormalizeAllowedActions) {
  const std::array<std::string_view, 6> actions = {"opened", "edited", "closed", "reopened",
                                                    "labeled", "unlabeled"};

  for (auto action : actions) {
    nlohmann::json payload = nlohmann::json::parse(
        R"({"issue": {"number": 1, "body": "test", "labels": [{"name": "agamemnon-team"}]}})",
        nullptr, false);
    payload["action"] = action;

    auto result = normalize_issues_event(payload);
    EXPECT_TRUE(result.has_value()) << "Action '" << action << "' should be allowed";
    EXPECT_EQ(result->action, action);
  }
}

}  // namespace projectagamemnon
