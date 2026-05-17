#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/orchestrator.hpp"
#include "projectagamemnon/store.hpp"

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

namespace {

// Build a minimal TaskBrief for testing.
TaskBrief make_brief(std::string title = "test", std::vector<std::string> repos = {"repo-a"},
                     std::unordered_map<std::string, std::vector<std::string>> modules = {}) {
  TaskBrief b;
  b.title = std::move(title);
  b.repos = std::move(repos);
  b.modules = std::move(modules);
  return b;
}

}  // namespace

// NatsClient is a heavy dependency; these tests use a real NatsClient that
// simply fails to connect (connected_ = false) so all publish/subscribe calls
// are no-ops.  This validates Orchestrator logic without requiring a live NATS server.

class OrchestratorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    nats_ = std::make_unique<NatsClient>("nats://localhost:14222");  // port unlikely to be open
    // connect() gracefully degrades — no exception on failure
    nats_->connect();
    orch_ = std::make_unique<Orchestrator>(store_, *nats_);
  }

  Store store_;
  std::unique_ptr<NatsClient> nats_;
  std::unique_ptr<Orchestrator> orch_;
};

TEST_F(OrchestratorTest, SubmitReturnsBriefId) {
  std::string id = orch_->submit(make_brief());
  EXPECT_FALSE(id.empty());
}

TEST_F(OrchestratorTest, SubmitPersistsTasksInStore) {
  std::string brief_id = orch_->submit(make_brief("t", {"repo-a"}, {{"repo-a", {"m1"}}}));
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  // 1 L0 + 1 L1 + 1 L2 + 1 L3
  EXPECT_EQ(tasks.size(), 4u);
}

TEST_F(OrchestratorTest, SubmitDelegatesL0Root) {
  std::string brief_id = orch_->submit(make_brief());
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  ASSERT_FALSE(tasks.empty());
  // Find L0 root.
  auto it = std::find_if(tasks.begin(), tasks.end(),
                         [](const HmasTask& t) { return t.layer == HmasLayer::L0_ChiefArchitect; });
  ASSERT_NE(it, tasks.end());
  EXPECT_EQ(it->state, TaskState::Delegated);
}

TEST_F(OrchestratorTest, GetPlanReturnsBriefIdAndRootAndTasks) {
  std::string brief_id = orch_->submit(make_brief("plan test", {"repo-a"}, {{"repo-a", {"m1"}}}));
  json plan = orch_->get_plan(brief_id);
  EXPECT_EQ(plan["brief_id"], brief_id);
  EXPECT_FALSE(plan["root"].is_null());
  EXPECT_GT(plan["tasks"].size(), 0u);
}

TEST_F(OrchestratorTest, GetPlanUnknownBriefReturnsEmptyTasks) {
  json plan = orch_->get_plan("nonexistent-brief");
  EXPECT_EQ(plan["tasks"].size(), 0u);
}

TEST_F(OrchestratorTest, EscalateTransitionsToEscalated) {
  std::string brief_id = orch_->submit(make_brief("esc", {"repo-a"}, {{"repo-a", {"m1"}}}));
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);

  // Find L0 root — it is Delegated after submit.
  std::optional<HmasTask> root_opt;
  for (auto& t : tasks) {
    if (t.layer == HmasLayer::L0_ChiefArchitect) {
      root_opt = store_.get_hmas_task(t.id);
      break;
    }
  }
  ASSERT_TRUE(root_opt.has_value());
  HmasTask root = *root_opt;

  // Manually move to InProgress so we can escalate.
  TaskStateMachine sm;
  sm.try_transition(root, TaskEvent::Start);
  store_.update_hmas_task(root);

  EXPECT_TRUE(orch_->escalate(root.id, "blocked"));
  auto updated = store_.get_hmas_task(root.id);
  ASSERT_TRUE(updated.has_value());
  EXPECT_EQ(updated->state, TaskState::Escalated);
  ASSERT_EQ(updated->escalations.size(), 1u);
  EXPECT_EQ(updated->escalations[0].reason, "blocked");
}

TEST_F(OrchestratorTest, EscalateNonExistentReturnsFalse) {
  EXPECT_FALSE(orch_->escalate("does-not-exist", "reason"));
}

TEST_F(OrchestratorTest, OnMyrmidonCompletionMarksTaskCompleted) {
  std::string brief_id = orch_->submit(make_brief("complete", {"repo-a"}, {{"repo-a", {"m1"}}}));
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);

  // Find an L3 leaf task.
  std::optional<HmasTask> leaf_opt;
  for (auto& t : tasks) {
    if (t.layer == HmasLayer::L3_TaskAgent) {
      leaf_opt = store_.get_hmas_task(t.id);
      break;
    }
  }
  ASSERT_TRUE(leaf_opt.has_value());
  HmasTask leaf = *leaf_opt;

  // Move leaf to InProgress.
  TaskStateMachine sm;
  if (leaf.state == TaskState::Pending) sm.try_transition(leaf, TaskEvent::Delegate);
  sm.try_transition(leaf, TaskEvent::Start);
  store_.update_hmas_task(leaf);

  json payload = {{"task_id", leaf.id}};
  orch_->on_myrmidon_completion("hi.tasks.t.t.completed", payload.dump());

  auto after = store_.get_hmas_task(leaf.id);
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->state, TaskState::Completed);
  EXPECT_FALSE(after->completed_at.empty());
}

}  // namespace projectagamemnon::test
