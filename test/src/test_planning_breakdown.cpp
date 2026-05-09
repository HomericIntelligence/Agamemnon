#include "projectagamemnon/planning_breakdown.hpp"

#include <algorithm>
#include <unordered_set>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

namespace {
TaskBrief make_brief(
    std::vector<std::string> repos,
    std::unordered_map<std::string, std::vector<std::string>> modules = {},
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>
        impls = {}) {
  TaskBrief b;
  b.id = "brief-test";
  b.title = "Test Brief";
  b.description = "desc";
  b.repos = std::move(repos);
  b.modules = std::move(modules);
  b.impls = std::move(impls);
  return b;
}
}  // namespace

TEST(PlanningBreakdownTest, SingleRepoSingleModule) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({"repo-a"}, {{"repo-a", {"mod-1"}}}));

  // Expect: 1 L0 + 1 L1 + 1 L2 + 1 L3 = 4
  ASSERT_EQ(tasks.size(), 4u);
  EXPECT_EQ(tasks[0].layer, HmasLayer::L0_ChiefArchitect);
}

TEST(PlanningBreakdownTest, L0RootHasNullParent) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({"repo-a"}));
  EXPECT_TRUE(tasks[0].parent_task_id.empty());
}

TEST(PlanningBreakdownTest, AllTasksHaveBriefId) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({"repo-a"}, {{"repo-a", {"m1", "m2"}}}));
  for (const auto& t : tasks) EXPECT_EQ(t.brief_id, "brief-test");
}

TEST(PlanningBreakdownTest, TwoReposThreeModulesEach) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief(
      {"repo-a", "repo-b"}, {{"repo-a", {"m1", "m2", "m3"}}, {"repo-b", {"m4", "m5", "m6"}}}));

  int l0 = 0, l1 = 0, l2 = 0, l3 = 0;
  for (const auto& t : tasks) {
    switch (t.layer) {
      case HmasLayer::L0_ChiefArchitect:
        ++l0;
        break;
      case HmasLayer::L1_ComponentLead:
        ++l1;
        break;
      case HmasLayer::L2_ModuleLead:
        ++l2;
        break;
      case HmasLayer::L3_TaskAgent:
        ++l3;
        break;
    }
  }
  EXPECT_EQ(l0, 1);
  EXPECT_EQ(l1, 2);
  EXPECT_EQ(l2, 6);
  EXPECT_EQ(l3, 6);  // one default impl per module
}

TEST(PlanningBreakdownTest, ExplicitImplsExpandL3) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({"repo-a"}, {{"repo-a", {"mod-1"}}},
                                       {{"repo-a", {{"mod-1", {"impl-A", "impl-B", "impl-C"}}}}}));

  int l3 = 0;
  for (const auto& t : tasks) {
    if (t.layer == HmasLayer::L3_TaskAgent) ++l3;
  }
  EXPECT_EQ(l3, 3);
}

TEST(PlanningBreakdownTest, ChildTaskIdsLinkage) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({"repo-a"}, {{"repo-a", {"m1"}}}));

  // Build index by ID.
  std::unordered_map<std::string, const HmasTask*> idx;
  for (const auto& t : tasks) idx[t.id] = &t;

  for (const auto& t : tasks) {
    for (const auto& child_id : t.child_task_ids) {
      ASSERT_TRUE(idx.count(child_id) > 0) << "child_id not in task list: " << child_id;
      EXPECT_EQ(idx.at(child_id)->parent_task_id, t.id);
    }
  }
}

TEST(PlanningBreakdownTest, L1BlockedByL0Root) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({"repo-a"}));
  std::string root_id = tasks[0].id;

  for (const auto& t : tasks) {
    if (t.layer == HmasLayer::L1_ComponentLead) {
      auto& bb = t.blocked_by;
      EXPECT_TRUE(std::find(bb.begin(), bb.end(), root_id) != bb.end());
    }
  }
}

TEST(PlanningBreakdownTest, MaxDepthIsL3) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({"repo-a"}, {{"repo-a", {"m1", "m2"}}}));
  for (const auto& t : tasks) {
    EXPECT_LE(static_cast<int>(t.layer), static_cast<int>(HmasLayer::L3_TaskAgent));
  }
}

TEST(PlanningBreakdownTest, AllIdsUnique) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(
      make_brief({"repo-a", "repo-b"}, {{"repo-a", {"m1", "m2"}}, {"repo-b", {"m3"}}}));
  std::unordered_set<std::string> ids;
  for (const auto& t : tasks) {
    EXPECT_TRUE(ids.insert(t.id).second) << "duplicate ID: " << t.id;
  }
}

TEST(PlanningBreakdownTest, EmptyReposYieldsOnlyL0) {
  PlanningBreakdown pd;
  auto tasks = pd.decompose(make_brief({}));
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0].layer, HmasLayer::L0_ChiefArchitect);
}

}  // namespace projectagamemnon::test
