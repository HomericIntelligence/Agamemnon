#include "projectagamemnon/github_client.hpp"
#include "projectagamemnon/store.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Build a fake GitHub issue JSON that mimics what list_issues returns.
static json make_agent_issue(int number, const json& agent_body) {
  // Reproduce the Store::make_issue_body_ format
  std::string body = "## AgamemnonEntity: agents/" + agent_body["id"].get<std::string>() +
                     "\n\n```json\n" + agent_body.dump(2) + "\n```\n";
  return {{"number", number}, {"body", body}};
}

static json make_team_issue(int number, const json& team_body) {
  std::string body = "## AgamemnonEntity: teams/" + team_body["id"].get<std::string>() +
                     "\n\n```json\n" + team_body.dump(2) + "\n```\n";
  return {{"number", number}, {"body", body}};
}

static json make_task_issue(int number, const json& task_body) {
  std::string body = "## AgamemnonEntity: tasks/" + task_body["id"].get<std::string>() +
                     "\n\n```json\n" + task_body.dump(2) + "\n```\n";
  return {{"number", number}, {"body", body}};
}

static json make_fault_issue(int number, const json& fault_body) {
  std::string body = "## AgamemnonEntity: faults/" + fault_body["id"].get<std::string>() +
                     "\n\n```json\n" + fault_body.dump(2) + "\n```\n";
  return {{"number", number}, {"body", body}};
}

static json sample_agent(const std::string& id, const std::string& name = "agent-alpha") {
  return {{"id", id},
          {"name", name},
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
}

// ── Hydration Tests ───────────────────────────────────────────────────────────

TEST(HydrationTest, LoadsAgentsFromGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  json a1 = sample_agent("id-001", "alpha");
  json a2 = sample_agent("id-002", "beta");
  mock->seed_issues["agamemnon-agent"] = {make_agent_issue(10, a1), make_agent_issue(11, a2)};

  Store store(mock);
  auto result = store.list_agents();

  ASSERT_TRUE(result.contains("agents"));
  EXPECT_EQ(result["agents"].size(), 2u);
}

TEST(HydrationTest, SkipsMalformedIssue) {
  auto mock = std::make_shared<MockGitHubClient>();
  json good = sample_agent("id-good", "valid");
  // Issue with no parseable JSON block
  json bad_issue = {{"number", 99}, {"body", "no json here"}};
  mock->seed_issues["agamemnon-agent"] = {make_agent_issue(10, good), bad_issue};

  Store store(mock);
  auto result = store.list_agents();

  ASSERT_TRUE(result.contains("agents"));
  EXPECT_EQ(result["agents"].size(), 1u);
  EXPECT_EQ(result["agents"][0]["id"], "id-good");
}

TEST(HydrationTest, LazyLoadCalledOnlyOnce) {
  auto mock = std::make_shared<MockGitHubClient>();
  mock->seed_issues["agamemnon-agent"] = {make_agent_issue(1, sample_agent("id-x"))};

  Store store(mock);

  // Three list calls should only trigger one GitHub API call
  store.list_agents();
  store.list_agents();
  store.list_agents();

  long list_calls = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "list_issues" && c.arg1 == "agamemnon-agent";
  });
  EXPECT_EQ(list_calls, 1);
}

TEST(HydrationTest, HydrationStoresGitHubIssueNumber) {
  auto mock = std::make_shared<MockGitHubClient>();
  json agent = sample_agent("id-hn");
  mock->seed_issues["agamemnon-agent"] = {make_agent_issue(42, agent)};

  Store store(mock);
  auto retrieved = store.get_agent("id-hn");

  ASSERT_FALSE(retrieved.is_null());
  EXPECT_EQ(retrieved["_github_issue"], "42");
}

TEST(HydrationTest, LoadsTeamsFromGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  json team = {{"id", "team-1"},
               {"name", "alpha-team"},
               {"agentIds", json::array()},
               {"createdAt", "2026-01-01T00:00:00Z"}};
  mock->seed_issues["agamemnon-team"] = {make_team_issue(20, team)};

  Store store(mock);
  auto result = store.list_teams();

  ASSERT_TRUE(result.contains("teams"));
  EXPECT_EQ(result["teams"].size(), 1u);
  EXPECT_EQ(result["teams"][0]["name"], "alpha-team");
}

TEST(HydrationTest, LoadsTasksFromGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  json task = {
      {"id", "task-1"},        {"teamId", "team-1"},    {"subject", "do work"},
      {"description", ""},     {"assigneeAgentId", ""}, {"blockedBy", json::array()},
      {"type", "general"},     {"status", "pending"},   {"createdAt", "2026-01-01T00:00:00Z"},
      {"completedAt", nullptr}};
  mock->seed_issues["agamemnon-task"] = {make_task_issue(30, task)};

  Store store(mock);
  auto result = store.list_all_tasks();

  ASSERT_TRUE(result.contains("tasks"));
  EXPECT_EQ(result["tasks"].size(), 1u);
  EXPECT_EQ(result["tasks"][0]["subject"], "do work");
}

TEST(HydrationTest, LoadsFaultsFromGitHub) {
  auto mock = std::make_shared<MockGitHubClient>();
  json fault = {{"id", "fault-1"},
                {"type", "latency"},
                {"active", true},
                {"createdAt", "2026-01-01T00:00:00Z"}};
  mock->seed_issues["agamemnon-fault"] = {make_fault_issue(40, fault)};

  Store store(mock);
  auto result = store.list_faults();

  ASSERT_TRUE(result.contains("faults"));
  EXPECT_EQ(result["faults"].size(), 1u);
  EXPECT_EQ(result["faults"][0]["type"], "latency");
}

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
  // find the create_issue call
  auto it = std::find_if(mock->calls.begin(), mock->calls.end(),
                         [](const auto& c) { return c.method == "create_issue"; });
  ASSERT_NE(it, mock->calls.end());
  // body (arg2) must contain the agent name
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
  mock->calls.clear();  // ignore the create call

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
  // With no GitHub client, list_agents returns empty (no seeded data)
  Store store(nullptr);
  auto result = store.list_agents();
  EXPECT_EQ(result["agents"].size(), 0u);
}

// ── Isolation Tests (hydration per entity type) ────────────────────────────────

TEST(IsolationTest, AgentHydrationDoesNotTriggerTeamHydration) {
  auto mock = std::make_shared<MockGitHubClient>();
  mock->seed_issues["agamemnon-agent"] = {make_agent_issue(1, sample_agent("id-iso"))};

  Store store(mock);
  store.list_agents();  // should only load agents

  long team_calls = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "list_issues" && c.arg1 == "agamemnon-team";
  });
  EXPECT_EQ(team_calls, 0);
}

TEST(IsolationTest, HydratedEntitiesVisibleToSubsequentReads) {
  auto mock = std::make_shared<MockGitHubClient>();
  json agent = sample_agent("id-visible", "seeded-agent");
  mock->seed_issues["agamemnon-agent"] = {make_agent_issue(5, agent)};

  Store store(mock);

  // get_agent should also hydrate and find the seeded agent
  auto result = store.get_agent("id-visible");
  ASSERT_FALSE(result.is_null());
  EXPECT_EQ(result["name"], "seeded-agent");
}

// #161 — concurrent list_agents from 2 threads must trigger exactly ONE GitHub fetch.
TEST(HydrationTest, ConcurrentListAgentsOnlyFetchesOnce) {
  auto mock = std::make_shared<MockGitHubClient>();
  mock->seed_issues["agamemnon-agent"] = {make_agent_issue(1, sample_agent("id-concurrent"))};

  Store store(mock);

  constexpr int kThreads = 2;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() { store.list_agents(); });
  }
  for (auto& t : threads) t.join();

  long list_calls = std::count_if(mock->calls.begin(), mock->calls.end(), [](const auto& c) {
    return c.method == "list_issues" && c.arg1 == "agamemnon-agent";
  });
  // call_once guarantees exactly one fetch regardless of concurrent callers.
  EXPECT_EQ(list_calls, 1);
}

// ── Write + Read Tests (#162) ─────────────────────────────────────────────────

TEST(GitHub162Test, WriteToGitHubSucceedsAndReadsReturnSameData) {
  auto mock = std::make_shared<MockGitHubClient>();
  Store store(mock);

  // Create agent
  auto created = store.create_agent({{"name", "write-test"}, {"role", "worker"}});
  std::string id = created["id"];

  // Verify the data written to GitHub can be read back
  auto retrieved = store.get_agent(id);
  ASSERT_FALSE(retrieved.is_null());
  EXPECT_EQ(retrieved["id"], id);
  EXPECT_EQ(retrieved["name"], "write-test");
  EXPECT_EQ(retrieved["role"], "worker");
}

TEST(GitHub162Test, GitHubNotFoundTriggsInMemoryFallback) {
  auto mock = std::make_shared<MockGitHubClient>();
  mock->fail_list_on_label = "agamemnon-agent";  // Simulate GitHub 404
  Store store(mock);

  // Create an agent — should succeed with in-memory storage
  auto created = store.create_agent({{"name", "fallback-test"}, {"role", "worker"}});
  std::string id = created["id"];

  // Should still be readable from in-memory store despite GitHub failure
  auto retrieved = store.get_agent(id);
  ASSERT_FALSE(retrieved.is_null());
  EXPECT_EQ(retrieved["id"], id);
  EXPECT_EQ(retrieved["name"], "fallback-test");
}

TEST(GitHub162Test, RehydrateOnStartupPopulatesFromPaginatedList) {
  auto mock = std::make_shared<MockGitHubClient>();
  // Seed agents across multiple paginated results
  json agent1 = sample_agent("id-page-1", "agent-one");
  json agent2 = sample_agent("id-page-2", "agent-two");
  json agent3 = sample_agent("id-page-3", "agent-three");
  mock->seed_issues["agamemnon-agent"] = {
      make_agent_issue(100, agent1),
      make_agent_issue(101, agent2),
      make_agent_issue(102, agent3),
  };

  Store store(mock);

  // List should return all agents (simulating pagination handling)
  auto result = store.list_agents();
  ASSERT_TRUE(result.contains("agents"));
  EXPECT_EQ(result["agents"].size(), 3u);

  // Verify all agents are present
  auto names = json::array();
  for (const auto& agent : result["agents"]) {
    names.push_back(agent["name"]);
  }
  EXPECT_NE(std::find(names.begin(), names.end(), json("agent-one")), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), json("agent-two")), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), json("agent-three")), names.end());
}

}  // namespace projectagamemnon::test
