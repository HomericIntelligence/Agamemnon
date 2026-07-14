#include "agamemnon/store.hpp"

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace agamemnon::test {

// ── Helper ────────────────────────────────────────────────────────────────────

static json make_agent(const std::string& name) { return {{"name", name}}; }

static json make_team(const std::string& name) { return {{"name", name}}; }

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

// ── #340 Deterministic pagination (no dups / full coverage) ──────────────────

TEST(PaginationDeterminism, AgentPagesNoDuplicatesFullCoverage) {
  Store s;
  constexpr int N = 10;
  for (int i = 0; i < N; ++i) s.create_agent(make_agent("agent" + std::to_string(i)));

  constexpr std::size_t kLimit = 3;
  std::set<std::string> seen_ids;
  std::size_t total_seen = 0;

  for (std::size_t offset = 0; offset < N; offset += kLimit) {
    auto r = s.list_agents(kLimit, offset);
    const auto& agents = r["agents"];
    for (const auto& a : agents) {
      std::string id = a["id"].get<std::string>();
      EXPECT_EQ(seen_ids.count(id), 0u) << "Duplicate agent id: " << id;
      seen_ids.insert(id);
      ++total_seen;
    }
  }
  EXPECT_EQ(total_seen, static_cast<std::size_t>(N));
}

TEST(PaginationDeterminism, AgentPageOrderIsStable) {
  Store s;
  constexpr int N = 6;
  for (int i = 0; i < N; ++i) s.create_agent(make_agent("a" + std::to_string(i)));

  // Two calls with the same parameters must return the same order.
  auto r1 = s.list_agents(N, 0);
  auto r2 = s.list_agents(N, 0);
  ASSERT_EQ(r1["agents"].size(), r2["agents"].size());
  for (std::size_t i = 0; i < r1["agents"].size(); ++i) {
    EXPECT_EQ(r1["agents"][i]["id"], r2["agents"][i]["id"]);
  }
}

TEST(PaginationDeterminism, TaskPagesNoDuplicatesFullCoverage) {
  Store s;
  auto t = s.create_team({{"name", "det-team"}});
  std::string tid = t["team"]["id"].get<std::string>();
  constexpr int N = 10;
  for (int i = 0; i < N; ++i) s.create_task(tid, make_task("t" + std::to_string(i)));

  constexpr std::size_t kLimit = 3;
  std::set<std::string> seen_ids;
  std::size_t total_seen = 0;

  for (std::size_t offset = 0; offset < N; offset += kLimit) {
    auto r = s.list_tasks_for_team(tid, kLimit, offset);
    for (const auto& task : r["tasks"]) {
      std::string id = task["id"].get<std::string>();
      EXPECT_EQ(seen_ids.count(id), 0u) << "Duplicate task id: " << id;
      seen_ids.insert(id);
      ++total_seen;
    }
  }
  EXPECT_EQ(total_seen, static_cast<std::size_t>(N));
}

}  // namespace agamemnon::test
