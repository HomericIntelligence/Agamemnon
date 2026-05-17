#include "projectagamemnon/fake_nats_publisher.hpp"
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

// OrchestratorTest uses FakeNatsPublisher — no real NATS connection is required.
// This removes the ~690ms per-test overhead and makes tests order-independent.

class OrchestratorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    orch_ = std::make_unique<Orchestrator>(store_, fake_nats_);
  }

  Store store_;
  FakeNatsPublisher fake_nats_;
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

// ── FakeNatsPublisher injection tests (#159) ──────────────────────────────────

TEST_F(OrchestratorTest, SubmitPublishesToMyrmidonSubject) {
  // Verifies that submit() triggers at least one publish() via FakeNatsPublisher.
  // This would time out for ~690ms previously (real NatsClient connect attempt).
  fake_nats_.clear();
  orch_->submit(make_brief("pub-test", {"repo-a"}, {{"repo-a", {"m1"}}}));
  EXPECT_TRUE(fake_nats_.has_subject_prefix("hi.myrmidon."));
}

TEST_F(OrchestratorTest, FakeNatsPublisherReturnsTrueOnPublish) {
  // Verify the fake publish always succeeds (happy path for caller logic).
  EXPECT_TRUE(fake_nats_.publish("hi.test.subj", R"({"k":"v"})"));
  ASSERT_EQ(fake_nats_.calls.size(), 1u);
  EXPECT_EQ(fake_nats_.calls[0].subject, "hi.test.subj");
}

TEST_F(OrchestratorTest, FakeNatsPublisherRecordsPublishLog) {
  // Verify publish_log() is recorded in log_calls.
  fake_nats_.publish_log("hi.logs.agamemnon.info", "info", "msg", {});
  ASSERT_EQ(fake_nats_.log_calls.size(), 1u);
  EXPECT_EQ(fake_nats_.log_calls[0], "hi.logs.agamemnon.info");
}

}  // namespace projectagamemnon::test
