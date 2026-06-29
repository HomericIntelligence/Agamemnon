#include "projectagamemnon/fake_nats_publisher.hpp"
#include "projectagamemnon/github_client.hpp"
#include "projectagamemnon/orchestrator.hpp"
#include "projectagamemnon/store.hpp"

#include <memory>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// Build an issue list from a mock's created_issues, then re-seed seed_issues
// so a fresh Store(mock) hydrates those issues. This mirrors a server restart:
// new process, same GitHub repo.
static void simulate_restart(MockGitHubClient& mock) {
  for (const auto& [num, issue] : mock.created_issues) {
    const std::string label = issue.value("label", "");
    json full = {{"number", std::stoi(num)}, {"body", issue.value("body", "")}};
    mock.seed_issues[label].push_back(full);
  }
}

TEST(OrchestratorPersistence, GetPlanSurvivesRestart) {
  auto mock = std::make_shared<MockGitHubClient>();
  FakeNatsPublisher nats;

  // Phase 1: submit a brief.
  std::string brief_id;
  {
    Store store(mock);
    Orchestrator orch(store, nats);
    TaskBrief b;
    b.title = "restart-test";
    b.repos = {"repo-a"};
    b.modules["repo-a"] = {"m1"};
    brief_id = orch.submit(std::move(b));
  }

  // Phase 2: simulate restart by giving the mock back its writes as seeds.
  simulate_restart(*mock);

  // Phase 3: fresh Store + Orchestrator on the same mock.
  Store store2(mock);
  Orchestrator orch2(store2, nats);
  json plan = orch2.get_plan(brief_id);

  EXPECT_EQ(plan["brief_id"], brief_id);
  EXPECT_FALSE(plan["root"].is_null());
  EXPECT_GE(plan["tasks"].size(), 4u);  // L0 + L1 + L2 + L3
}

TEST(OrchestratorPersistence, SubmitPublishesDelegatedOnHiTasks) {
  auto mock = std::make_shared<MockGitHubClient>();
  FakeNatsPublisher nats;
  Store store(mock);
  Orchestrator orch(store, nats);

  TaskBrief b;
  b.title = "pub";
  b.repos = {"repo-a"};
  b.modules["repo-a"] = {"m1"};
  orch.submit(std::move(b));

  EXPECT_TRUE(nats.has_subject("hi.tasks.delegated"));
}

TEST(OrchestratorPersistence, CompletionPublishesHiTasksCompleted) {
  auto mock = std::make_shared<MockGitHubClient>();
  FakeNatsPublisher nats;
  Store store(mock);
  Orchestrator orch(store, nats);

  TaskBrief b;
  b.title = "done";
  b.repos = {"repo-a"};
  b.modules["repo-a"] = {"m1"};
  std::string brief_id = orch.submit(std::move(b));
  auto tasks = store.list_hmas_tasks_by_brief(brief_id);
  std::string l3_id;
  for (const auto& t : tasks) {
    if (t.layer == HmasLayer::L3_TaskAgent) {
      l3_id = t.id;
      break;
    }
  }
  ASSERT_FALSE(l3_id.empty());

  nats.clear();
  json payload = {{"task_id", l3_id}};
  orch.on_myrmidon_completion("hi.myrmidon.task_agent." + l3_id, payload.dump());

  EXPECT_TRUE(nats.has_subject("hi.tasks.completed"));
}

}  // namespace projectagamemnon::test
