#include "projectagamemnon/github_client.hpp"

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Disabled path (no env vars set in test environment) ──────────────────────

TEST(GitHubClientTest, DisabledWhenConfigEmpty) {
  GitHubClient::Config cfg;  // all fields empty
  GitHubClient client(cfg);
  EXPECT_FALSE(client.is_enabled());
}

TEST(GitHubClientTest, DisabledWhenTokenMissing) {
  GitHubClient::Config cfg;
  cfg.owner = "myorg";
  cfg.repo = "myrepo";
  GitHubClient client(cfg);
  EXPECT_FALSE(client.is_enabled());
}

TEST(GitHubClientTest, DisabledWhenOwnerMissing) {
  GitHubClient::Config cfg;
  cfg.token = "ghp_test";
  cfg.repo = "myrepo";
  GitHubClient client(cfg);
  EXPECT_FALSE(client.is_enabled());
}

TEST(GitHubClientTest, DisabledWhenRepoMissing) {
  GitHubClient::Config cfg;
  cfg.token = "ghp_test";
  cfg.owner = "myorg";
  GitHubClient client(cfg);
  EXPECT_FALSE(client.is_enabled());
}

TEST(GitHubClientTest, EnabledWhenAllFieldsPresent) {
  GitHubClient::Config cfg;
  cfg.token = "ghp_test";
  cfg.owner = "myorg";
  cfg.repo = "myrepo";
  GitHubClient client(cfg);
  EXPECT_TRUE(client.is_enabled());
}

TEST(GitHubClientTest, CreateIssueReturnsMinusOneWhenDisabled) {
  GitHubClient client(GitHubClient::Config{});
  EXPECT_EQ(client.create_issue("title", "body", {}), -1);
}

TEST(GitHubClientTest, UpdateIssueReturnsFalseWhenDisabled) {
  GitHubClient client(GitHubClient::Config{});
  EXPECT_FALSE(client.update_issue(1, "title", "body", {}, "open"));
}

TEST(GitHubClientTest, ListIssuesReturnsEmptyArrayWhenDisabled) {
  GitHubClient client(GitHubClient::Config{});
  auto result = client.list_issues("type:task");
  EXPECT_TRUE(result.is_array());
  EXPECT_TRUE(result.empty());
}

TEST(GitHubClientTest, ConfigFromEnvReadsEnvironment) {
  // Verify config_from_env doesn't crash when env vars are absent.
  auto cfg = GitHubClient::config_from_env();
  // In test environment GITHUB_TOKEN etc. are not set, so all fields should be
  // empty (or whatever the env actually has — we just ensure no crash).
  (void)cfg;
  SUCCEED();
}

}  // namespace projectagamemnon::test
