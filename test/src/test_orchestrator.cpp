#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/store.hpp"

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

namespace agamemnon::test {

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
  void SetUp() override { orch_ = std::make_unique<Orchestrator>(store_, fake_nats_); }

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

TEST_F(OrchestratorTest, SubmitPublishesTaskStateOnHiTasksSubject) {
  fake_nats_.clear();
  orch_->submit(make_brief("hi-tasks", {"repo-a"}, {{"repo-a", {"m1"}}}));
  // L0 root transitions Pending -> Decomposing -> Delegated; publish_task_state
  // fires once after update_hmas_task on the Delegated state.
  EXPECT_TRUE(fake_nats_.has_subject("hi.tasks.delegated"));
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

TEST_F(OrchestratorTest, DelegateUnblockedChildrenUsesChildTaskIds) {
  HmasTask parent;
  parent.id = "parent-1";
  parent.brief_id = "brief-foo";
  parent.layer = HmasLayer::L2_ModuleLead;
  parent.state = TaskState::Completed;
  parent.child_task_ids = {"child-1"};
  store_.create_hmas_task(parent);

  HmasTask child;
  child.id = "child-1";
  child.brief_id = "brief-foo";
  child.parent_task_id = "parent-1";
  child.layer = HmasLayer::L3_TaskAgent;
  child.state = TaskState::Pending;
  child.blocked_by = {"parent-1"};
  store_.create_hmas_task(child);

  // Unrelated task in a DIFFERENT brief — must not be touched even though
  // pre-fix code would have scanned it.
  HmasTask noise;
  noise.id = "noise";
  noise.brief_id = "brief-other";
  noise.layer = HmasLayer::L3_TaskAgent;
  noise.state = TaskState::Pending;
  store_.create_hmas_task(noise);

  json payload = {{"task_id", "parent-1"}};
  orch_->on_myrmidon_completion("hi.tasks.t.t.completed", payload.dump());

  auto child_after = store_.get_hmas_task("child-1");
  ASSERT_TRUE(child_after.has_value());
  EXPECT_NE(child_after->state, TaskState::Pending);  // delegated
  auto noise_after = store_.get_hmas_task("noise");
  ASSERT_TRUE(noise_after.has_value());
  EXPECT_EQ(noise_after->state, TaskState::Pending);  // untouched
}

TEST_F(OrchestratorTest, DelegateUnblockedChildrenDelegatesNonLeafChild) {
  // Pending → Delegated is guarded to L3 leaves; a Pending L1/L2 child must be
  // walked through Submit → Decomposing → Delegate when its parent completes.
  HmasTask root;
  root.id = "root-1";
  root.brief_id = "brief-foo";
  root.layer = HmasLayer::L0_ChiefArchitect;
  root.state = TaskState::Completed;
  root.child_task_ids = {"lead-1"};
  store_.create_hmas_task(root);

  HmasTask lead;
  lead.id = "lead-1";
  lead.brief_id = "brief-foo";
  lead.parent_task_id = "root-1";
  lead.layer = HmasLayer::L1_ComponentLead;
  lead.state = TaskState::Pending;
  lead.blocked_by = {"root-1"};
  store_.create_hmas_task(lead);

  json payload = {{"task_id", "root-1"}};
  orch_->on_myrmidon_completion("hi.tasks.t.t.completed", payload.dump());

  auto lead_after = store_.get_hmas_task("lead-1");
  ASSERT_TRUE(lead_after.has_value());
  EXPECT_EQ(lead_after->state, TaskState::Delegated);
  EXPECT_TRUE(fake_nats_.has_subject_prefix("hi.myrmidon.pipeline.component-lead.task."));
}

// ── HMAS mesh wire tests (Odysseus ADR-013) ──────────────────────────────────

TEST_F(OrchestratorTest, SubmitDualPublishesRoleAddressedDispatch) {
  orch_->submit(make_brief());
  EXPECT_TRUE(fake_nats_.has_subject_prefix("hi.myrmidon.chief_architect."));
  EXPECT_TRUE(fake_nats_.has_subject_prefix("hi.myrmidon.pipeline.chief-architect.task."));
}

TEST_F(OrchestratorTest, OnEpicRegisteredCreatesDecomposingRootAndDispatchesPlanner) {
  const json payload = {
      {"epic", {{"repo", "Homeric/Repo"}, {"issue", 42}, {"key", "homeric-repo-42"}}},
      {"children", {101, 102}}};
  const std::string brief_id =
      orch_->on_epic_registered("hi.pipeline.epic.homeric-repo-42.registered", payload.dump());
  ASSERT_FALSE(brief_id.empty());

  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  ASSERT_EQ(tasks.size(), 1u);  // placeholder root only — planner builds the tree
  EXPECT_EQ(tasks[0].layer, HmasLayer::L0_ChiefArchitect);
  EXPECT_EQ(tasks[0].state, TaskState::Decomposing);
  EXPECT_EQ(tasks[0].repo, "Homeric/Repo");
  EXPECT_EQ(tasks[0].issue, 42);

  EXPECT_TRUE(fake_nats_.has_subject("hi.myrmidon.pipeline.chief-architect.task." + tasks[0].id));
  // Dispatch payload carries the epic pointers for the planner.
  bool found_epic = false;
  for (const auto& c : fake_nats_.calls) {
    if (c.subject.rfind("hi.myrmidon.pipeline.chief-architect.task.", 0) == 0) {
      const json body = json::parse(c.payload);
      EXPECT_EQ(body["epic"]["issue"], 42);
      EXPECT_EQ(body["role"], "chief-architect");
      found_epic = true;
    }
  }
  EXPECT_TRUE(found_epic);
}

TEST_F(OrchestratorTest, OnEpicRegisteredRejectsMissingEpicFields) {
  EXPECT_EQ(orch_->on_epic_registered("s", json{{"epic", json::object()}}.dump()), "");
  EXPECT_EQ(orch_->on_epic_registered("s", "not json"), "");
}

TEST_F(OrchestratorTest, OnMyrmidonStartedRecordsAssignmentAtClaim) {
  const std::string brief_id = orch_->submit(make_brief());
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  const std::string root_id = tasks[0].id;

  const json started = {
      {"task_id", root_id}, {"agent_id", "mesh-worker-1"}, {"exec_host", "hermes"}};
  orch_->on_myrmidon_started("hi.tasks.mesh." + root_id + ".started", started.dump());

  auto updated = store_.get_hmas_task(root_id);
  ASSERT_TRUE(updated.has_value());
  EXPECT_EQ(updated->state, TaskState::InProgress);
  EXPECT_EQ(updated->assigned_lead_id, "mesh-worker-1@hermes");
}

TEST_F(OrchestratorTest, OnMyrmidonFailedDrivesTaskToFailed) {
  const std::string brief_id = orch_->submit(make_brief());
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  const std::string root_id = tasks[0].id;

  const json failed = {{"task_id", root_id},
                       {"error", {{"kind", "Boom"}, {"message", "x"}, {"retryable", true}}}};
  orch_->on_myrmidon_failed("hi.tasks.mesh." + root_id + ".failed", failed.dump());

  auto updated = store_.get_hmas_task(root_id);
  ASSERT_TRUE(updated.has_value());
  EXPECT_EQ(updated->state, TaskState::Failed);
}

TEST_F(OrchestratorTest, SplitTaskCreatesRemainderBlockedByOriginal) {
  const std::string brief_id = orch_->submit(make_brief("t", {"repo-a"}, {{"repo-a", {"m1"}}}));
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  auto it = std::find_if(tasks.begin(), tasks.end(),
                         [](const HmasTask& t) { return t.layer == HmasLayer::L3_TaskAgent; });
  ASSERT_NE(it, tasks.end());
  const std::string original_id = it->id;

  const json subtasks =
      json::array({{{"title", "remainder A"}, {"description", "d"}, {"base_branch", "feat/x"}},
                   {{"title", "remainder B"}}});
  const json result = orch_->split_task(original_id, subtasks);
  ASSERT_FALSE(result.contains("error")) << result.dump();
  ASSERT_EQ(result["created"].size(), 2u);

  const std::string first_id = result["created"][0].get<std::string>();
  const std::string second_id = result["created"][1].get<std::string>();

  auto first = store_.get_hmas_task(first_id);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->state, TaskState::Pending);
  EXPECT_EQ(first->parent_task_id, it->parent_task_id);
  EXPECT_NE(std::find(first->blocked_by.begin(), first->blocked_by.end(), original_id),
            first->blocked_by.end());
  EXPECT_NE(first->description.find("Base branch: feat/x"), std::string::npos);

  auto second = store_.get_hmas_task(second_id);
  ASSERT_TRUE(second.has_value());
  // Sequential chaining: remainder B waits on remainder A too.
  EXPECT_NE(std::find(second->blocked_by.begin(), second->blocked_by.end(), first_id),
            second->blocked_by.end());

  // Original tracks the remainder for delegate_unblocked_children.
  auto original = store_.get_hmas_task(original_id);
  ASSERT_TRUE(original.has_value());
  EXPECT_NE(std::find(original->child_task_ids.begin(), original->child_task_ids.end(), first_id),
            original->child_task_ids.end());
}

TEST_F(OrchestratorTest, SplitThenCompleteDispatchesRemainder) {
  const std::string brief_id = orch_->submit(make_brief("t", {"repo-a"}, {{"repo-a", {"m1"}}}));
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  auto it = std::find_if(tasks.begin(), tasks.end(),
                         [](const HmasTask& t) { return t.layer == HmasLayer::L3_TaskAgent; });
  ASSERT_NE(it, tasks.end());
  const std::string original_id = it->id;

  const json result = orch_->split_task(original_id, json::array({{{"title", "remainder"}}}));
  const std::string remainder_id = result["created"][0].get<std::string>();
  fake_nats_.clear();

  // Worker completes the first slice → remainder becomes dispatchable.
  orch_->on_myrmidon_completion("hi.tasks.mesh." + original_id + ".completed",
                                json{{"task_id", original_id}}.dump());

  auto remainder = store_.get_hmas_task(remainder_id);
  ASSERT_TRUE(remainder.has_value());
  EXPECT_EQ(remainder->state, TaskState::Delegated);
  EXPECT_TRUE(fake_nats_.has_subject("hi.myrmidon.pipeline.task-agent.task." + remainder_id));
}

TEST_F(OrchestratorTest, SplitTaskValidatesInput) {
  EXPECT_TRUE(orch_->split_task("nope", json::array({{{"title", "x"}}})).contains("error"));

  const std::string brief_id = orch_->submit(make_brief());
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);
  EXPECT_TRUE(orch_->split_task(tasks[0].id, json::array()).contains("error"));
  EXPECT_TRUE(orch_->split_task(tasks[0].id, json::array({{{"description", "no title"}}}))
                  .contains("error"));
}

}  // namespace agamemnon::test
