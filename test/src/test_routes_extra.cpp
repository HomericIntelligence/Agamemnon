#define CPPHTTPLIB_NO_EXCEPTIONS
#include "route_test_fixture.hpp"

#include "agamemnon/state_machine.hpp"

namespace agamemnon::test {

using json = nlohmann::json;

// Route tests for endpoints not exercised elsewhere (#175): populated
// dead-letter queue, chaos creation happy path, the escalate happy path, and
// the per-team empty task list.
class RoutesExtraTest : public RouteTestFixture {};

// ── GET /v1/teams/:team_id/tasks (empty) ─────────────────────────────────────

TEST_F(RoutesExtraTest, ListTasksForTeamEmpty) {
  auto create = Post("/v1/teams", {{"name", "empty-tasks-team"}});
  ASSERT_TRUE(create);
  ASSERT_EQ(create->status, 201);
  std::string team_id = json::parse(create->body)["team"]["id"].get<std::string>();

  auto res = Get("/v1/teams/" + team_id + "/tasks");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_TRUE(body["tasks"].empty());
  EXPECT_EQ(body["total"], 0);
}

// ── POST /v1/chaos/:type ─────────────────────────────────────────────────────

TEST_F(RoutesExtraTest, CreateChaosHappyPath) {
  // The handler ignores the request body entirely: it captures the path type
  // and unconditionally calls Store::create_fault(type) → 201.
  auto res = Post("/v1/chaos/latency", json::object());
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 201);

  // Store::create_fault returns {"fault": {...}} — id/type are nested.
  auto fault = json::parse(res->body)["fault"];
  EXPECT_TRUE(fault.contains("id"));
  EXPECT_EQ(fault["type"], "latency");
  EXPECT_TRUE(fault["active"].get<bool>());

  // The new fault is visible via GET /v1/chaos.
  auto list = Get("/v1/chaos");
  ASSERT_TRUE(list);
  ASSERT_EQ(list->status, 200);
  auto faults = json::parse(list->body)["faults"];
  ASSERT_EQ(faults.size(), 1u);
  EXPECT_EQ(faults[0]["id"], fault["id"]);
}

// ── GET/DELETE /v1/dead-letter ───────────────────────────────────────────────

TEST_F(RoutesExtraTest, DeadLetterGetReturnsQueuedItems) {
  nats_->dead_letter_queue()->push("hi.test", "payload", 1);

  auto res = Get("/v1/dead-letter");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto entries = json::parse(res->body)["dead_letter_queue"];
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0]["subject"], "hi.test");
  EXPECT_EQ(entries[0]["payload"], "payload");
  EXPECT_EQ(entries[0]["attempts"], 1);

  // GET drains: a second read returns an empty array.
  auto second = Get("/v1/dead-letter");
  ASSERT_TRUE(second);
  EXPECT_TRUE(json::parse(second->body)["dead_letter_queue"].empty());
}

TEST_F(RoutesExtraTest, DeadLetterDeleteEmptiesPopulatedQueue) {
  nats_->dead_letter_queue()->push("hi.a", "pa", 1);
  nats_->dead_letter_queue()->push("hi.b", "pb", 2);
  ASSERT_EQ(nats_->dead_letter_queue()->size(), 2u);

  auto del = Delete("/v1/dead-letter");
  ASSERT_TRUE(del);
  EXPECT_EQ(del->status, 200);
  EXPECT_EQ(json::parse(del->body)["cleared"], true);

  auto res = Get("/v1/dead-letter");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(json::parse(res->body)["dead_letter_queue"].empty());
}

// ── POST /v1/tasks/:task_id/escalate ─────────────────────────────────────────

TEST_F(RoutesExtraTest, EscalateTaskHappyPath) {
  // The HTTP API has no endpoint that transitions a task to InProgress.
  // Reach the 200 branch of POST /v1/tasks/:id/escalate the same way
  // test_orchestrator.cpp does: drive the state machine directly, then
  // exercise the HTTP route.
  auto create = Post("/v1/briefs", {{"title", "escalate brief"},
                                    {"repos", json::array({"repo-e"})}});
  ASSERT_TRUE(create);
  ASSERT_EQ(create->status, 201);

  // Use "root" — deterministic; "tasks" iteration order is non-deterministic
  // because list_hmas_tasks_by_brief walks an unordered container.
  // Orchestrator::get_plan exposes the L0 root explicitly.
  std::string task_id = json::parse(create->body)["root"]["id"].get<std::string>();

  // Manually move to InProgress so escalate is legal.
  auto task_opt = store_->get_hmas_task(task_id);
  ASSERT_TRUE(task_opt.has_value());
  HmasTask task = *task_opt;
  TaskStateMachine sm;
  ASSERT_TRUE(sm.try_transition(task, TaskEvent::Start));
  store_->update_hmas_task(task);

  auto res = Post("/v1/tasks/" + task_id + "/escalate", {{"reason", "blocked-on-x"}});
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 200);
  auto body = json::parse(res->body);
  EXPECT_EQ(body["task_id"], task_id);
  EXPECT_TRUE(body["escalated"].get<bool>());

  // Regression check: state should now be Escalated.
  auto state = Get("/v1/tasks/" + task_id + "/state");
  ASSERT_TRUE(state);
  ASSERT_EQ(state->status, 200);
  EXPECT_EQ(json::parse(state->body)["state"], "Escalated");
}

}  // namespace agamemnon::test
