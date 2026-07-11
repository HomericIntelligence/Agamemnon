#include "agamemnon/hmas_types.hpp"

#include <gtest/gtest.h>

namespace agamemnon::test {

TEST(HmasTypesTest, LayerRoundTrip) {
  for (auto layer : {HmasLayer::L0_ChiefArchitect, HmasLayer::L1_ComponentLead,
                     HmasLayer::L2_ModuleLead, HmasLayer::L3_TaskAgent}) {
    EXPECT_EQ(hmas_layer_from_string(hmas_layer_to_string(layer)), layer);
  }
}

TEST(HmasTypesTest, StateRoundTrip) {
  for (auto state :
       {TaskState::Pending, TaskState::Decomposing, TaskState::Delegated, TaskState::InProgress,
        TaskState::Escalated, TaskState::Completed, TaskState::Failed}) {
    EXPECT_EQ(task_state_from_string(task_state_to_string(state)), state);
  }
}

TEST(HmasTypesTest, InvalidLayerThrows) {
  EXPECT_THROW(hmas_layer_from_string("bogus"), std::invalid_argument);
}

TEST(HmasTypesTest, InvalidStateThrows) {
  EXPECT_THROW(task_state_from_string("bogus"), std::invalid_argument);
}

TEST(HmasTypesTest, HmasTaskToJsonContainsAllFields) {
  HmasTask t;
  t.id = "abc";
  t.brief_id = "brief-1";
  t.parent_task_id = "parent-1";
  t.layer = HmasLayer::L2_ModuleLead;
  t.state = TaskState::InProgress;
  t.subject = "subject";
  t.description = "desc";
  t.repo = "repo-a";
  t.module = "mod-1";
  t.created_at = "2026-01-01T00:00:00Z";

  json j = hmas_task_to_json(t);
  EXPECT_EQ(j["id"], "abc");
  EXPECT_EQ(j["layer"], "L2_ModuleLead");
  EXPECT_EQ(j["state"], "InProgress");
  EXPECT_EQ(j["repo"], "repo-a");
  EXPECT_EQ(j["module"], "mod-1");
  EXPECT_TRUE(j["child_task_ids"].is_array());
  EXPECT_TRUE(j["escalations"].is_array());
}

TEST(HmasTypesTest, TaskBriefRoundTrip) {
  json j = {{"title", "test brief"},
            {"description", "desc"},
            {"repos", {"repo-a", "repo-b"}},
            {"modules", {{"repo-a", {"mod-1", "mod-2"}}, {"repo-b", {"mod-3"}}}},
            {"impls", {{"repo-a", {{"mod-1", {"impl task A", "impl task B"}}}}}}};

  TaskBrief brief = task_brief_from_json(j);
  EXPECT_EQ(brief.title, "test brief");
  ASSERT_EQ(brief.repos.size(), 2u);
  EXPECT_EQ(brief.repos[0], "repo-a");
  ASSERT_EQ(brief.modules["repo-a"].size(), 2u);
  EXPECT_EQ(brief.impls["repo-a"]["mod-1"][0], "impl task A");

  json j2 = task_brief_to_json(brief);
  EXPECT_EQ(j2["title"], "test brief");
  EXPECT_EQ(j2["repos"].size(), 2u);
}

}  // namespace agamemnon::test
