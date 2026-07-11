#include "agamemnon/state_machine.hpp"

#include <gtest/gtest.h>

namespace agamemnon::test {

namespace {
HmasTask make_task(HmasLayer layer = HmasLayer::L0_ChiefArchitect,
                   TaskState state = TaskState::Pending) {
  HmasTask t;
  t.id = "test-task";
  t.layer = layer;
  t.state = state;
  return t;
}
}  // namespace

TEST(StateMachineTest, PendingToDecomposingOnSubmit) {
  TaskStateMachine sm;
  HmasTask t = make_task();
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Submit));
  EXPECT_EQ(t.state, TaskState::Decomposing);
}

TEST(StateMachineTest, DecomposingToDelegatedOnDelegate) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L0_ChiefArchitect, TaskState::Decomposing);
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Delegate));
  EXPECT_EQ(t.state, TaskState::Delegated);
}

TEST(StateMachineTest, DelegatedToInProgressOnStart) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L1_ComponentLead, TaskState::Delegated);
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Start));
  EXPECT_EQ(t.state, TaskState::InProgress);
}

TEST(StateMachineTest, InProgressToEscalatedOnEscalate) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L2_ModuleLead, TaskState::InProgress);
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Escalate));
  EXPECT_EQ(t.state, TaskState::Escalated);
}

TEST(StateMachineTest, EscalatedToDelegatedOnRetry) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L2_ModuleLead, TaskState::Escalated);
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Retry));
  EXPECT_EQ(t.state, TaskState::Delegated);
}

TEST(StateMachineTest, InProgressToCompletedSetsTimestamp) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L3_TaskAgent, TaskState::InProgress);
  EXPECT_TRUE(t.completed_at.empty());
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Complete));
  EXPECT_EQ(t.state, TaskState::Completed);
  EXPECT_FALSE(t.completed_at.empty());
}

TEST(StateMachineTest, CompletedToInProgressIsInvalid) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L0_ChiefArchitect, TaskState::Completed);
  EXPECT_FALSE(sm.try_transition(t, TaskEvent::Start));
  EXPECT_EQ(t.state, TaskState::Completed);
}

TEST(StateMachineTest, L3LeafSkipsDecomposing) {
  // L3 tasks should go Pending → Delegated directly.
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L3_TaskAgent, TaskState::Pending);
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Delegate));
  EXPECT_EQ(t.state, TaskState::Delegated);
}

TEST(StateMachineTest, NonL3TaskCannotSkipDecomposing) {
  // L0 tasks must go through Decomposing; Pending → Delegated should be rejected for L0.
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L0_ChiefArchitect, TaskState::Pending);
  EXPECT_FALSE(sm.try_transition(t, TaskEvent::Delegate));
  EXPECT_EQ(t.state, TaskState::Pending);
}

TEST(StateMachineTest, InProgressToFailedSetsTimestamp) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L3_TaskAgent, TaskState::InProgress);
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Fail));
  EXPECT_EQ(t.state, TaskState::Failed);
  EXPECT_FALSE(t.completed_at.empty());
}

TEST(StateMachineTest, EscalatedToFailed) {
  TaskStateMachine sm;
  HmasTask t = make_task(HmasLayer::L2_ModuleLead, TaskState::Escalated);
  EXPECT_TRUE(sm.try_transition(t, TaskEvent::Fail));
  EXPECT_EQ(t.state, TaskState::Failed);
}

TEST(StateMachineTest, ValidTargetsReflectsTransitionTable) {
  TaskStateMachine sm;
  auto targets = sm.valid_targets(TaskState::InProgress, TaskEvent::Escalate);
  ASSERT_EQ(targets.size(), 1u);
  EXPECT_EQ(targets[0], TaskState::Escalated);
}

TEST(StateMachineTest, ValidTargetsEmptyForUnknownPair) {
  TaskStateMachine sm;
  auto targets = sm.valid_targets(TaskState::Completed, TaskEvent::Escalate);
  EXPECT_TRUE(targets.empty());
}

}  // namespace agamemnon::test
