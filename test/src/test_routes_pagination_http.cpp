#define CPPHTTPLIB_NO_EXCEPTIONS
#include "route_test_fixture.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace agamemnon::test {

using json = nlohmann::json;

// HTTP-layer pagination coverage for the list endpoints (#175). The store-level
// pagination math is covered by test_pagination.cpp; these tests exercise the
// route handlers end-to-end (query-string parsing → 400 on garbage, slicing).
class RoutesPaginationHttpTest : public RouteTestFixture {
 protected:
  // Seeds `n` agents via POST /v1/agents and returns their ids.
  std::vector<std::string> seed_agents(int n) {
    std::vector<std::string> ids;
    for (int i = 0; i < n; ++i) {
      auto res = Post("/v1/agents", {{"name", "pag-agent-" + std::to_string(i)}});
      if (!res || res->status != 201) return ids;
      ids.push_back(json::parse(res->body)["id"].get<std::string>());
    }
    return ids;
  }

  // Submits one brief and returns all HMAS task ids it produced.
  std::vector<std::string> seed_brief_tasks() {
    // Per src/planning_breakdown.cpp this brief produces exactly 9 HMAS tasks:
    //   L0 root                                          (1)
    //   L1 per repo: r1, r2                              (+2 = 3)
    //   L2 per (repo, module): (r1,m1) (r1,m2) (r2,core) (+3 = 6)
    //   L3 per L2 default "Implement <mod>"              (+3 = 9)
    json brief = {{"title", "pagination brief"},
                  {"repos", json::array({"r1", "r2"})},
                  {"modules", json{{"r1", json::array({"m1", "m2"})}}}};
    auto res = Post("/v1/briefs", brief);
    if (!res || res->status != 201) return {};
    std::vector<std::string> ids;
    for (const auto& t : json::parse(res->body)["tasks"])
      ids.push_back(t["id"].get<std::string>());
    return ids;
  }
};

// ── GET /v1/agents pagination ────────────────────────────────────────────────

TEST_F(RoutesPaginationHttpTest, AgentsListRespectsLimit) {
  auto seeded = seed_agents(5);
  ASSERT_EQ(seeded.size(), 5u);

  auto res = Get("/v1/agents?limit=2");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["agents"].size(), 2u);
  // Membership, not ordering: Store::list_agents sorts by id before slicing,
  // but coupling to lexicographic UUID order is brittle to future reordering.
  for (const auto& a : body["agents"]) {
    EXPECT_NE(std::find(seeded.begin(), seeded.end(), a["id"].get<std::string>()),
              seeded.end());
  }
}

TEST_F(RoutesPaginationHttpTest, AgentsListRespectsOffset) {
  ASSERT_EQ(seed_agents(5).size(), 5u);

  auto res = Get("/v1/agents?offset=3");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agents"].size(), 2u);
}

TEST_F(RoutesPaginationHttpTest, AgentsListLimitAndOffset) {
  ASSERT_EQ(seed_agents(5).size(), 5u);

  auto res = Get("/v1/agents?limit=1&offset=2");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agents"].size(), 1u);
}

TEST_F(RoutesPaginationHttpTest, AgentsListLimitZeroReturnsEmpty) {
  ASSERT_EQ(seed_agents(3).size(), 3u);

  auto res = Get("/v1/agents?limit=0");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agents"].size(), 0u);
}

TEST_F(RoutesPaginationHttpTest, AgentsListOffsetBeyondEndReturnsEmpty) {
  ASSERT_EQ(seed_agents(3).size(), 3u);

  auto res = Get("/v1/agents?offset=10");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agents"].size(), 0u);
}

TEST_F(RoutesPaginationHttpTest, AgentsListNonNumericLimitReturns400) {
  auto res = Get("/v1/agents?limit=abc");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 400);
}

// ── GET /v1/tasks pagination ─────────────────────────────────────────────────

TEST_F(RoutesPaginationHttpTest, TasksListRespectsLimit) {
  auto seeded = seed_brief_tasks();
  ASSERT_EQ(seeded.size(), 9u);

  auto res = Get("/v1/tasks?limit=2");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["tasks"].size(), 2u);
  EXPECT_EQ(body["total"], 9);
  for (const auto& t : body["tasks"]) {
    EXPECT_NE(std::find(seeded.begin(), seeded.end(), t["id"].get<std::string>()),
              seeded.end());
  }
}

TEST_F(RoutesPaginationHttpTest, TasksListRespectsOffset) {
  ASSERT_EQ(seed_brief_tasks().size(), 9u);

  auto res = Get("/v1/tasks?offset=3");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["tasks"].size(), 6u);
}

// ── GET /v1/chaos pagination ─────────────────────────────────────────────────

TEST_F(RoutesPaginationHttpTest, ChaosListRespectsLimit) {
  for (int i = 0; i < 5; ++i) {
    auto create = Post("/v1/chaos/latency", json::object());
    ASSERT_TRUE(create);
    ASSERT_EQ(create->status, 201);
  }

  auto res = Get("/v1/chaos?limit=2");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["faults"].size(), 2u);
}

// ── GET /v1/teams pagination ─────────────────────────────────────────────────

TEST_F(RoutesPaginationHttpTest, TeamsListRespectsLimit) {
  for (int i = 0; i < 5; ++i) {
    auto create = Post("/v1/teams", {{"name", "pag-team-" + std::to_string(i)}});
    ASSERT_TRUE(create);
    ASSERT_EQ(create->status, 201);
  }

  auto res = Get("/v1/teams?limit=2");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["teams"].size(), 2u);
}

}  // namespace agamemnon::test
