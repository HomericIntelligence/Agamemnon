#include "projectagamemnon/store.hpp"

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// ── Helper ────────────────────────────────────────────────────────────────────

static json make_agent(const std::string& name) {
  return {{"name", name}};
}

static json make_team(const std::string& name) {
  return {{"name", name}};
}

static json make_task(const std::string& subject) {
  return {{"subject", subject}, {"type", "general"}};
}

// ── list_agents ───────────────────────────────────────────────────────────────

TEST(PaginationAgents, EmptyStore) {
  Store s;
  auto r = s.list_agents(100, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 0);
  EXPECT_EQ(r["agents"].size(), 0);
  EXPECT_EQ(r["limit"].get<std::size_t>(), 100);
  EXPECT_EQ(r["offset"].get<std::size_t>(), 0);
}

TEST(PaginationAgents, LimitSlicesResults) {
  Store s;
  for (int i = 0; i < 5; ++i) s.create_agent(make_agent("a" + std::to_string(i)));

  auto r = s.list_agents(2, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 5);
  EXPECT_EQ(r["agents"].size(), 2);
  EXPECT_EQ(r["limit"].get<std::size_t>(), 2);
  EXPECT_EQ(r["offset"].get<std::size_t>(), 0);
}

TEST(PaginationAgents, OffsetBeyondEnd) {
  Store s;
  for (int i = 0; i < 5; ++i) s.create_agent(make_agent("a" + std::to_string(i)));

  auto r = s.list_agents(10, 4);
  EXPECT_EQ(r["total"].get<std::size_t>(), 5);
  EXPECT_EQ(r["agents"].size(), 1);
}

TEST(PaginationAgents, OffsetPastEnd) {
  Store s;
  for (int i = 0; i < 5; ++i) s.create_agent(make_agent("a" + std::to_string(i)));

  auto r = s.list_agents(10, 10);
  EXPECT_EQ(r["total"].get<std::size_t>(), 5);
  EXPECT_EQ(r["agents"].size(), 0);
}

TEST(PaginationAgents, LimitZeroReturnsEmpty) {
  Store s;
  s.create_agent(make_agent("x"));
  auto r = s.list_agents(0, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 1);
  EXPECT_EQ(r["agents"].size(), 0);
}

// ── list_faults ───────────────────────────────────────────────────────────────

TEST(PaginationFaults, EmptyStore) {
  Store s;
  auto r = s.list_faults(100, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 0);
  EXPECT_EQ(r["faults"].size(), 0);
}

TEST(PaginationFaults, LimitSlicesResults) {
  Store s;
  for (int i = 0; i < 4; ++i) s.create_fault("latency");

  auto r = s.list_faults(2, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 4);
  EXPECT_EQ(r["faults"].size(), 2);
  EXPECT_EQ(r["limit"].get<std::size_t>(), 2);
}

TEST(PaginationFaults, OffsetBeyondEnd) {
  Store s;
  for (int i = 0; i < 3; ++i) s.create_fault("drop");

  auto r = s.list_faults(10, 5);
  EXPECT_EQ(r["total"].get<std::size_t>(), 3);
  EXPECT_EQ(r["faults"].size(), 0);
}

// ── list_all_tasks ────────────────────────────────────────────────────────────

TEST(PaginationAllTasks, LimitAndOffset) {
  Store s;
  auto team = s.create_team({{"name", "t"}});
  std::string tid = team["team"]["id"].get<std::string>();
  for (int i = 0; i < 6; ++i) s.create_task(tid, make_task("s" + std::to_string(i)));

  auto r = s.list_all_tasks(3, 2);
  EXPECT_EQ(r["total"].get<std::size_t>(), 6);
  EXPECT_EQ(r["tasks"].size(), 3);
  EXPECT_EQ(r["offset"].get<std::size_t>(), 2);
}

// ── list_tasks_for_team ───────────────────────────────────────────────────────

TEST(PaginationTeamTasks, OnlyCountsTeamTasks) {
  Store s;
  auto t1 = s.create_team({{"name", "team1"}});
  auto t2 = s.create_team({{"name", "team2"}});
  std::string id1 = t1["team"]["id"].get<std::string>();
  std::string id2 = t2["team"]["id"].get<std::string>();

  for (int i = 0; i < 4; ++i) s.create_task(id1, make_task("s" + std::to_string(i)));
  for (int i = 0; i < 3; ++i) s.create_task(id2, make_task("x" + std::to_string(i)));

  auto r = s.list_tasks_for_team(id1, 100, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 4);
  EXPECT_EQ(r["tasks"].size(), 4);
}

TEST(PaginationTeamTasks, LimitWithinTeam) {
  Store s;
  auto t1 = s.create_team({{"name", "team1"}});
  std::string id1 = t1["team"]["id"].get<std::string>();
  for (int i = 0; i < 5; ++i) s.create_task(id1, make_task("s" + std::to_string(i)));

  auto r = s.list_tasks_for_team(id1, 2, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 5);
  EXPECT_EQ(r["tasks"].size(), 2);
}

// ── list_teams ────────────────────────────────────────────────────────────────

TEST(PaginationTeams, DefaultEnvelope) {
  Store s;
  for (int i = 0; i < 3; ++i) s.create_team(make_team("t" + std::to_string(i)));

  auto r = s.list_teams(100, 0);
  EXPECT_EQ(r["total"].get<std::size_t>(), 3);
  EXPECT_EQ(r["teams"].size(), 3);
  EXPECT_TRUE(r.contains("limit"));
  EXPECT_TRUE(r.contains("offset"));
}

}  // namespace projectagamemnon::test
