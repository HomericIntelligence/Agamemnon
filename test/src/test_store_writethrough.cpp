// Tests for Store write-through to GitHub (MockGitHubClient), null-client
// in-memory mode, and per-entity-type hydration isolation.
//
// These are the non-hydration tests extracted from test_store_persistence.cpp,
// which cannot be included directly because it duplicates the HydrationTest_*
// symbols already defined in test_store_hydration.cpp.

#include "projectagamemnon/github_client.hpp"
#include "projectagamemnon/store.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Write-through Tests ────────────────────────────────────────────────────────

TEST(WriteThroughTest, CreateAgentPublishesToGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  store.create_agent({{"name", "smoketest"}, {"role", "worker"}});

  long creates = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "create_issue" && c.arg3 == "agamemnon-agent";
  });
  EXPECT_EQ(creates, 1);
}

TEST(WriteThroughTest, CreateAgentBodyContainsJson) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  store.create_agent({{"name", "body-check"}, {"role", "worker"}});

  ASSERT_FALSE(mock->calls.empty());
  auto it = std::find_if(mock->calls.begin(), mock->calls.end(),
                         [](const auto& c) { return c.method == "create_issue"; });
  ASSERT_NE(it, mock->calls.end());
  EXPECT_NE(it->arg2.find("body-check"), std::string::npos);
}

TEST(WriteThroughTest, CreateAgentStoresIssueNumber) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_agent({{"name", "num-check"}, {"role", "worker"}});
  std::string id = result["id"];

  auto agent = store.get_agent(id);
  ASSERT_FALSE(agent.is_null());
  EXPECT_TRUE(agent.contains("_github_issue"));
}

TEST(WriteThroughTest, DeleteAgentClosesGitHubIssue) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_agent({{"name", "to-delete"}});
  std::string id = result["id"];
  auto agent = store.get_agent(id);
  std::string issue_num = agent["_github_issue"];

  store.delete_agent(id);

  EXPECT_FALSE(mock->closed_issues.empty());
  EXPECT_EQ(mock->closed_issues.back(), issue_num);
}

TEST(WriteThroughTest, UpdateAgentEditsGitHubIssue) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_agent({{"name", "to-update"}});
  std::string id = result["id"];

  store.update_agent(id, {{"role", "architect"}});

  EXPECT_FALSE(mock->updated_bodies.empty());
}

TEST(WriteThroughTest, StartAgentUpdatesGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_agent({{"name", "start-me"}});
  std::string id = result["id"];
  mock->calls.clear();

  store.start_agent(id);

  long updates = std::count_if(mock->calls.begin(), mock->calls.end(),
                               [](const auto& c) { return c.method == "update_issue_body"; });
  EXPECT_EQ(updates, 1);
}

TEST(WriteThroughTest, StopAgentUpdatesGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_agent({{"name", "stop-me"}});
  std::string id = result["id"];
  mock->calls.clear();

  store.stop_agent(id);

  long updates = std::count_if(mock->calls.begin(), mock->calls.end(),
                               [](const auto& c) { return c.method == "update_issue_body"; });
  EXPECT_EQ(updates, 1);
}

TEST(WriteThroughTest, CreateTeamPublishesToGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  store.create_team({{"name", "team-alpha"}});

  long creates = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "create_issue" && c.arg3 == "agamemnon-team";
  });
  EXPECT_EQ(creates, 1);
}

TEST(WriteThroughTest, DeleteTeamClosesGitHubIssue) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_team({{"name", "bye-team"}});
  std::string id = result["team"]["id"];

  store.delete_team(id);

  EXPECT_FALSE(mock->closed_issues.empty());
}

TEST(WriteThroughTest, CreateTaskPublishesToGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  store.create_task("team-1", {{"subject", "implement X"}});

  long creates = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "create_issue" && c.arg3 == "agamemnon-task";
  });
  EXPECT_EQ(creates, 1);
}

TEST(WriteThroughTest, MarkTaskCompletedUpdatesGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_task("team-1", {{"subject", "my task"}});
  std::string task_id = result["task"]["id"];
  mock->calls.clear();

  store.mark_task_completed(task_id);

  long updates = std::count_if(mock->calls.begin(), mock->calls.end(),
                               [](const auto& c) { return c.method == "update_issue_body"; });
  EXPECT_EQ(updates, 1);
}

TEST(WriteThroughTest, CreateFaultPublishesToGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  store.create_fault("latency");

  long creates = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "create_issue" && c.arg3 == "agamemnon-fault";
  });
  EXPECT_EQ(creates, 1);
}

TEST(WriteThroughTest, RemoveFaultClosesGitHubIssue) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  auto result = store.create_fault("crash");
  std::string id = result["fault"]["id"];

  store.remove_fault(id);

  EXPECT_FALSE(mock->closed_issues.empty());
}

// ── Null Client (in-memory mode) Tests ────────────────────────────────────────

TEST(NullClientTest, StoreWorksWithNullClient) {
  Store store(nullptr);

  auto r = store.create_agent({{"name", "no-github"}, {"role", "worker"}});
  EXPECT_FALSE(r.is_null());

  std::string id = r["id"];
  auto agent = store.get_agent(id);
  EXPECT_EQ(agent["name"], "no-github");

  EXPECT_TRUE(store.delete_agent(id));
  EXPECT_TRUE(store.get_agent(id).is_null());
}

TEST(NullClientTest, DefaultConstructorIsNullClient) {
  Store store;

  auto r = store.create_team({{"name", "default-team"}});
  EXPECT_FALSE(r.is_null());

  std::string id = r["team"]["id"];
  EXPECT_TRUE(store.delete_team(id));
}

TEST(NullClientTest, NullClientDoesNotHydrate) {
  Store store(nullptr);
  auto result = store.list_agents();
  EXPECT_EQ(result["agents"].size(), 0u);
}

// ── Isolation Tests (hydration per entity type) ───────────────────────────────

TEST(IsolationTest, AgentHydrationDoesNotTriggerTeamHydration) {
  auto mock = std::make_shared<MockGitHubClient>();
  json seed_agent = {{"id", "id-iso"},
                     {"name", "agent-alpha"},
                     {"label", ""},
                     {"program", ""},
                     {"workingDirectory", ""},
                     {"programArgs", json::array()},
                     {"taskDescription", ""},
                     {"tags", json::array()},
                     {"owner", ""},
                     {"role", "worker"},
                     {"host", "local"},
                     {"status", "offline"},
                     {"createdAt", "2026-01-01T00:00:00Z"}};
  std::string body = "## AgamemnonEntity: agents/id-iso\n\n```json\n" + seed_agent.dump(2) + "\n```\n";
  mock->seed_issues["agamemnon-agent"] = {{{"number", 1}, {"body", body}}};

  Store store(mock);
  store.list_agents();

  long team_calls = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "list_issues" && c.arg1 == "agamemnon-team";
  });
  EXPECT_EQ(team_calls, 0);
}

}  // namespace projectagamemnon::test
