#include "agamemnon/auth.hpp"
#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/fleet.hpp"
#include "agamemnon/github_client.hpp"
#include "agamemnon/metrics.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/rate_limiter.hpp"
#include "agamemnon/routes.hpp"
#include "agamemnon/store.hpp"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "httplib.h"
#include <gtest/gtest.h>

namespace agamemnon::test {

class FailingGitHub : public MockGitHubClient {
 public:
  bool fail_create = false;
  bool fail_update = false;
  bool empty_create = false;
  bool fail_after_commit = false;
  bool fail_fleet_update = false;
  bool fail_create_after_commit = false;

  std::vector<json> list_issues(std::string_view label) override {
    auto seeds = MockGitHubClient::list_issues(label);
    std::map<int, json> issues;
    for (const auto& issue : seeds) issues[issue["number"].get<int>()] = issue;
    for (const auto& [number, issue] : created_issues)
      if (issue["label"] == label)
        issues[std::stoi(number)] = {{"number", std::stoi(number)}, {"body", issue["body"]}};
    std::vector<json> result;
    for (const auto& [number, issue] : issues) result.push_back(issue);
    return result;
  }

  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    if (fail_create) throw std::runtime_error("GitHub unavailable");
    if (empty_create) return "";
    auto number = MockGitHubClient::create_issue(title, body, label);
    if (fail_create_after_commit) throw std::runtime_error("create response lost after commit");
    return number;
  }
  void update_issue_body(std::string_view number, std::string_view body) override {
    if (fail_update) throw std::runtime_error("GitHub unavailable");
    if (fail_fleet_update && body.starts_with("## AgamemnonEntity: fleet"))
      throw std::runtime_error("Fleet issue write unavailable");
    MockGitHubClient::update_issue_body(number, body);
    if (fail_after_commit) throw std::runtime_error("response lost after GitHub commit");
  }
};

class FleetRoutes : public ::testing::Test {
 protected:
  std::shared_ptr<FailingGitHub> github = std::make_shared<FailingGitHub>();
  FakeNatsPublisher publisher;
  Store store{github};
  AuthMiddleware auth{"fleet-test-key"};
  RateLimiter limiter{10000, 10000};
  MetricsRegistry metrics;
  Orchestrator orchestrator{store, publisher};
  httplib::Server server;
  std::unique_ptr<httplib::Client> client;
  std::thread listener;

  void SetUp() override {
    auto fleet =
        std::make_shared<FleetService>(store, publisher, &orchestrator, "test-operator-key");
    register_routes(server, store, publisher, limiter, auth, metrics, orchestrator, fleet);
    int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    listener = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
    client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_default_headers({{"Authorization", "Bearer fleet-test-key"}});
  }
  void TearDown() override {
    server.stop();
    if (listener.joinable()) listener.join();
  }
  httplib::Result post(const std::string& path, const json& body) {
    return client->Post("/v1/fleet/" + path, body.dump(), "application/json");
  }
  void seed_session(const std::string& id = "session-1", const std::string& agent = "agent-1") {
    if (github->created_issues.empty()) {
      ASSERT_EQ(post("pools", {{"id", "laptop"}, {"capacity", 2}})->status, 201);
      ASSERT_EQ(
          post("workers",
               {{"id", "worker-1"}, {"poolId", "laptop"}, {"capacity", 1}, {"host", "laptop"}})
              ->status,
          201);
      HmasTask task{};
      task.id = "task-1";
      task.layer = HmasLayer::L3_TaskAgent;
      task.state = TaskState::Pending;
      store.create_hmas_task(task);
    }
    ASSERT_EQ(post("sessions", {{"id", id},
                                {"workerId", "worker-1"},
                                {"agentId", agent},
                                {"workspace", "/work/" + id},
                                {"taskId", "task-1"},
                                {"domain", "pipeline"},
                                {"hmasRole", "task-agent"},
                                {"issueUrl", "https://github.com/org/repo/issues/1"}})
                  ->status,
              201);
  }
  json command(const std::string& id = "command-1") {
    return {
        {"commandId", id},
        {"idempotencyKey", id},
        {"generation", 1},
        {"payload", id.starts_with("input-") ? json{{"inputRef", std::string(32, 'a') + ".json"}}
                                             : json::object()}};
  }
  json fact(const std::string& status, const std::string& event = "event-1") {
    return {{"schema", "hi/fleet/v1"},
            {"workerId", "worker-1"},
            {"commandId", "command-1"},
            {"eventId", event},
            {"generation", 1},
            {"status", status},
            {"receipt", {{"stage", "implementation"}}}};
  }
  json activity(const std::string& value, int sequence = 1, const std::string& outcome = "") {
    json event = {
        {"activity", value}, {"observedAt", "2026-09-10T12:00:00Z"}, {"stage", "implementation"}};
    if (!outcome.empty()) {
      event["outcome"] = outcome;
      event["backgroundCleanup"] = "confirmed_empty";
    }
    if (outcome == "cancelled") event["commandId"] = "cancel-1";
    if (outcome == "interrupted") event["commandId"] = "interrupt-1";
    return {{"schema", "hi/fleet/v1"},
            {"eventId", "activity-" + std::to_string(sequence)},
            {"workerId", "worker-1"},
            {"generation", 1},
            {"sourceSequence", sequence},
            {"kind", "activity"},
            {"targetKind", "sessions"},
            {"targetId", "session-1"},
            {"event", event}};
  }
  json decision() {
    return {{"decisionId", "decision-1"},
            {"generation", 1},
            {"outcome", "completed"},
            {"decision", "approve_completion"},
            {"reviewerId", "operator-reviewer"},
            {"evidenceRef", "private-review-receipt"}};
  }
  httplib::Result resolve(json body) {
    return client->Post("/v1/fleet/sessions/session-1/resolve",
                        {{"X-Fleet-Resolution-Key", "test-operator-key"}}, body.dump(),
                        "application/json");
  }
};

TEST_F(FleetRoutes, CreatesGitHubBackedPoolWithoutDispatch) {
  auto response = post("pools", {{"id", "laptop"}, {"capacity", 12}});
  ASSERT_TRUE(response);
  ASSERT_EQ(response->status, 201) << response->body;
  EXPECT_EQ(json::parse(response->body)["id"], "laptop");
  EXPECT_EQ(github->created_issues.size(), 1u);
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetRoutes, ProjectsProjectionIsDisabledWithoutExplicitConfiguration) {
  auto response = client->Get("/v1/fleet/projects");
  ASSERT_TRUE(response);
  ASSERT_EQ(response->status, 200);
  EXPECT_EQ(json::parse(response->body)["state"], "disabled");
  EXPECT_EQ(post("projects/reconcile", json::object())->status, 200);
  EXPECT_TRUE(github->created_issues.empty());
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetRoutes, FailedDurableWriteCannotCreateOrDispatch) {
  github->fail_create = true;
  auto response = post("pools", {{"id", "laptop"}, {"capacity", 12}});
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 503);
  auto listed = client->Get("/v1/fleet/pools");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->status, 200);
  EXPECT_TRUE(json::parse(listed->body)["items"].empty());
  EXPECT_TRUE(publisher.calls.empty());
}

TEST(FleetPersistence, HmasFailedWriteLeavesConfirmedStateUntouched) {
  auto github = std::make_shared<FailingGitHub>();
  Store store(github);
  HmasTask task{};
  task.layer = HmasLayer::L3_TaskAgent;
  task.state = TaskState::Pending;
  task.id = "work-1";
  task.state = TaskState::Pending;
  store.create_hmas_task(task);
  github->fail_update = true;
  EXPECT_THROW(store.update_hmas_task_state(task.id, TaskState::InProgress), std::runtime_error);
  ASSERT_TRUE(store.get_hmas_task(task.id));
  EXPECT_EQ(store.get_hmas_task(task.id)->state, TaskState::Pending);
}

TEST(FleetPersistence, EmptyGitHubAcknowledgmentCannotCreateHmasTask) {
  auto github = std::make_shared<FailingGitHub>();
  Store store(github);
  HmasTask task{};
  task.id = "work-1";
  github->empty_create = true;
  EXPECT_THROW(store.create_hmas_task(task), std::runtime_error);
  EXPECT_FALSE(store.get_hmas_task(task.id));
}

TEST(FleetPersistence, UncertainHmasCreationRehydratesBeforeRetry) {
  auto github = std::make_shared<FailingGitHub>();
  Store store(github);
  HmasTask task{};
  task.id = "uncertain-create";
  github->fail_create_after_commit = true;
  EXPECT_THROW(store.create_hmas_task(task), std::runtime_error);
  github->fail_create_after_commit = false;
  ASSERT_TRUE(store.get_hmas_task(task.id).has_value());
  EXPECT_THROW(store.create_hmas_task(task), std::runtime_error);
  EXPECT_EQ(github->created_issues.size(), 1u);
}

TEST_F(FleetRoutes, MalformedBackingJsonReportsPersistenceFailure) {
  github->seed_issues["agamemnon-fleet"] =
      json::array({{{"number", 20}, {"body", "## AgamemnonEntity: fleet\n\n```json\n{\n```\n"}}});
  EXPECT_EQ(client->Get("/v1/fleet/pools")->status, 503);
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetRoutes, DurableCommandUsesCanonicalRoleQueueAndWaitsForObservedStart) {
  seed_session();
  auto response = post("sessions/session-1/start", command());
  ASSERT_EQ(response->status, 202) << response->body;
  ASSERT_EQ(publisher.calls.size(), 1u);
  EXPECT_EQ(publisher.calls[0].subject, "hi.myrmidon.pipeline.task-agent.task.task-1");
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["status"], "admitted");
  EXPECT_TRUE(record["lastActivityAt"].is_null());
  EXPECT_EQ(record["claimStatus"], "reserved");
  EXPECT_EQ(record["poolId"], "laptop");
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/sessions/session-1")->body)["status"], "admitted");
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["status"], "running");
  EXPECT_EQ(record["claimStatus"], "claimed");
  EXPECT_FALSE(record["lastActivityAt"].is_null());
}

TEST_F(FleetRoutes, FailedIntentWritePublishesNothingAndKeepsUnclaimedState) {
  seed_session();
  github->fail_update = true;
  EXPECT_EQ(post("sessions/session-1/start", command())->status, 503);
  EXPECT_TRUE(publisher.calls.empty());
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["claimStatus"], "unclaimed");
}

TEST_F(FleetRoutes, DuplicateIntentKeepsIdentityAndRejectsChangedPayload) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(publisher.calls.size(), 2u);
  EXPECT_EQ(publisher.calls[0].payload, publisher.calls[1].payload);
  auto changed = command();
  changed["payload"]["inputRef"] = "private:other";
  EXPECT_EQ(post("sessions/session-1/start", changed)->status, 409);
  ASSERT_EQ(post("sessions/session-1/ack", fact("accepted"))->status, 200);
  EXPECT_EQ(post("sessions/session-1/start", command())->status, 202);
  EXPECT_EQ(publisher.calls.size(), 2u);
}

TEST_F(FleetRoutes, CapacityAndWriterClaimsBlockConcurrentStart) {
  seed_session();
  seed_session("session-2", "agent-2");
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  EXPECT_EQ(post("sessions/session-2/start", command("command-2"))->status, 409);
}

TEST_F(FleetRoutes, GenerationAndWorkerIdentityFenceAcknowledgments) {
  seed_session();
  auto wrong = command();
  wrong["generation"] = 2;
  EXPECT_EQ(post("sessions/session-1/start", wrong)->status, 409);
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  auto ack = fact("accepted");
  ack["workerId"] = "other-worker";
  EXPECT_EQ(post("sessions/session-1/ack", ack)->status, 409);
  ack = fact("accepted");
  ack["generation"] = 2;
  EXPECT_EQ(post("sessions/session-1/ack", ack)->status, 409);
}

TEST_F(FleetRoutes, AcknowledgmentReplayIsIdempotentAndTurnEndDoesNotReleaseTask) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  auto before = client->Get("/v1/fleet/events?after=0")->body;
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  EXPECT_EQ(client->Get("/v1/fleet/events?after=0")->body, before);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("events", activity("idle", 2, "completed"))->status, 200);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["claimStatus"], "claimed");
  EXPECT_NE(record["status"], "completed");
}

TEST_F(FleetRoutes, CancellationOnlyCompletesAfterWorkerConfirmation) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/cancel", command("cancel-1"))->status, 202);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["status"], "cancelling");
  EXPECT_EQ(record["claimStatus"], "claimed");
  auto ack = fact("completed", "event-2");
  ack["commandId"] = "cancel-1";
  ASSERT_EQ(post("sessions/session-1/ack", ack)->status, 200);
  record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["status"], "cancelling");
  ASSERT_EQ(post("events", activity("idle", 2, "cancelled"))->status, 200);
  record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["status"], "cancelled");
  EXPECT_EQ(record["claimStatus"], "released");
}

TEST_F(FleetRoutes, StopCannotReleaseCapacityWithoutConfirmedBackgroundCleanup) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/cancel", command("cancel-1"))->status, 202);
  auto stopped = activity("idle", 2, "cancelled");
  stopped["event"].erase("backgroundCleanup");
  EXPECT_EQ(post("events", stopped)->status, 409);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["claimStatus"], "claimed");
  EXPECT_EQ(record["status"], "cancelling");
  stopped["event"]["backgroundCleanup"] = "unconfirmed";
  EXPECT_EQ(post("events", stopped)->status, 409);
  stopped["event"]["backgroundCleanup"] = "confirmed_empty";
  EXPECT_EQ(post("events", stopped)->status, 200);
  record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["claimStatus"], "released");
  EXPECT_EQ(record["backgroundCleanup"], "confirmed_empty");
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::InProgress);
}

TEST_F(FleetRoutes, ResolutionRequiresCurrentBackgroundCleanupEvidence) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  auto idle = activity("idle", 2, "completed");
  idle["event"].erase("backgroundCleanup");
  ASSERT_EQ(post("events", idle)->status, 200);
  EXPECT_EQ(resolve(decision())->status, 409);
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::InProgress);
  ASSERT_EQ(post("events", activity("idle", 3, "completed"))->status, 200);
  EXPECT_EQ(resolve(decision())->status, 200);
}

TEST_F(FleetRoutes, ActivityCannotAttributeAnotherLogicalExecutionToTheClaim) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  const auto admitted_state = store.get_hmas_task("task-1")->state;
  for (const auto* field :
       {"taskId", "agentId", "sessionId", "executionId", "workerId", "generation"}) {
    auto mismatched = activity("model_working");
    mismatched["event"][field] = "different-owner";
    EXPECT_EQ(post("events", mismatched)->status, 409) << field;
  }
  EXPECT_EQ(store.get_hmas_task("task-1")->state, admitted_state);
  EXPECT_EQ(post("events", activity("model_working"))->status, 200);
}

TEST_F(FleetRoutes, CleanupCannotConfirmAnOlderTurnAfterTheStopReceipt) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/input", command("input-1"))->status, 202);
  auto input_ack = fact("completed", "input-ack");
  input_ack["commandId"] = "input-1";
  input_ack["receipt"] = {{"providerTurnId", "current-turn"}};
  ASSERT_EQ(post("sessions/session-1/ack", input_ack)->status, 200);
  ASSERT_EQ(post("sessions/session-1/cancel", command("cancel-1"))->status, 202);
  auto stop_ack = fact("accepted", "stop-ack");
  stop_ack["commandId"] = "cancel-1";
  stop_ack["receipt"] = {{"waitingReason", "provider_stop_confirmation"}};
  ASSERT_EQ(post("sessions/session-1/ack", stop_ack)->status, 200);
  auto stale = activity("idle", 1, "cancelled");
  stale["event"]["providerTurnId"] = "old-turn";
  EXPECT_EQ(post("events", stale)->status, 409);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/sessions/session-1")->body)["claimStatus"],
            "reserved");
  stale["event"]["providerTurnId"] = "current-turn";
  EXPECT_EQ(post("events", stale)->status, 200);
}

TEST(FleetPersistence, MemoryOnlyModeIsRejected) {
  Store store;
  FakeNatsPublisher publisher;
  FleetService fleet(store, publisher);
  EXPECT_THROW(fleet.list("pools"), FleetError);
  EXPECT_THROW(fleet.create("pools", {{"id", "laptop"}, {"capacity", 12}}), FleetError);
}

TEST_F(FleetRoutes, RestartHydratesLinkageAndControlCursor) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  for (const auto& [number, issue] : github->created_issues) {
    github->seed_issues[issue["label"].get<std::string>()].push_back(
        {{"number", std::stoi(number)}, {"body", issue["body"]}});
  }
  FleetService restarted(store, publisher);
  auto record = restarted.get("sessions", "session-1");
  EXPECT_EQ(record["taskId"], "task-1");
  EXPECT_EQ(record["claimStatus"], "reserved");
  auto cursor = restarted.events(0)["cursor"].get<std::uint64_t>();
  EXPECT_TRUE(restarted.events(cursor)["events"].empty());
}

class ResponseGitHub : public CurlGitHubClient {
 public:
  ResponseGitHub() : CurlGitHubClient("example/repo", "test-token") {}
  Response response{403, "denied", ""};

 protected:
  Response do_get(const std::string&) const override { return response; }
  Response do_post(const std::string&, const std::string&) const override { return response; }
  Response do_patch(const std::string&, const std::string&) const override { return response; }
};

TEST(FleetGitHub, HttpFailuresArePropagatedForEveryPersistenceOperation) {
  ResponseGitHub client;
  for (long status : {401L, 403L, 404L, 429L, 500L}) {
    client.response.status = status;
    EXPECT_THROW(client.list_issues("agamemnon-fleet"), std::runtime_error);
    EXPECT_THROW(client.create_issue("title", "body", "label"), std::runtime_error);
    EXPECT_THROW(client.update_issue_body("1", "body"), std::runtime_error);
    EXPECT_THROW(client.close_issue("1"), std::runtime_error);
  }
}

TEST(FleetGitHub, MalformedAcknowledgmentsCannotSatisfyDurability) {
  ResponseGitHub client;
  client.response = {200, "not-json", ""};
  EXPECT_THROW(client.list_issues("agamemnon-fleet"), std::runtime_error);
  client.response = {201, "{}", ""};
  EXPECT_THROW(client.create_issue("title", "body", "label"), std::runtime_error);
}

TEST_F(FleetRoutes, UncertainCommitMustRehydrateBeforeAnotherAdmission) {
  seed_session();
  seed_session("session-2", "agent-2");
  github->fail_after_commit = true;
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 503);
  github->fail_after_commit = false;
  EXPECT_EQ(post("sessions/session-2/start", command("command-2"))->status, 409);
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetRoutes, InputCompletionDoesNotReleaseRunningSessionClaim) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/input", command("input-1"))->status, 202);
  auto ack = fact("completed", "event-2");
  ack["commandId"] = "input-1";
  ASSERT_EQ(post("sessions/session-1/ack", ack)->status, 200);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["status"], "running");
  EXPECT_EQ(record["claimStatus"], "claimed");
}

TEST(FleetPersistence, EscalationAndReplacementRollBackOnWriteFailure) {
  auto github = std::make_shared<FailingGitHub>();
  Store store(github);
  HmasTask task{};
  task.layer = HmasLayer::L3_TaskAgent;
  task.state = TaskState::Pending;
  task.id = "work-1";
  task.brief_id = "brief-1";
  store.create_hmas_task(task);
  github->fail_update = true;
  EscalationRecord escalation{};
  EXPECT_THROW(store.update_hmas_task_state_and_record_escalation(task.id, TaskState::InProgress,
                                                                  escalation),
               std::runtime_error);
  EXPECT_TRUE(store.get_hmas_task(task.id)->escalations.empty());
  auto changed = task;
  changed.brief_id = "brief-2";
  EXPECT_THROW(store.update_hmas_task(changed), std::runtime_error);
  EXPECT_EQ(store.get_hmas_task(task.id)->brief_id, "brief-1");
  EXPECT_EQ(store.list_hmas_tasks_by_brief("brief-1").size(), 1u);
}

TEST_F(FleetRoutes, SameIdAcrossKindsDoesNotBypassAdmission) {
  seed_session();
  ASSERT_EQ(post("executions", {{"id", "session-1"},
                                {"workerId", "worker-1"},
                                {"agentId", "agent-2"},
                                {"workspace", "/work/other"},
                                {"taskId", "task-1"},
                                {"domain", "pipeline"},
                                {"hmasRole", "task-agent"}})
                ->status,
            201);
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  EXPECT_EQ(post("executions/session-1/start", command("command-2"))->status, 409);
}

TEST_F(FleetRoutes, WorkerProtocolAndUnadmittedActivityAreRejected) {
  seed_session();
  auto invalid = activity("model_working");
  invalid["schema"] = "hi/fleet/v2";
  EXPECT_EQ(post("events", invalid)->status, 400);
  EXPECT_EQ(post("events", activity("model_working"))->status, 409);
}

TEST_F(FleetRoutes, WorkerActivityIsOrderedAndDoesNotWriteGitHubPerPacket) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  auto writes = github->calls.size();
  ASSERT_EQ(post("events", activity("tool_running", 2))->status, 200);
  ASSERT_EQ(post("events", activity("idle", 3, "completed"))->status, 200);
  EXPECT_EQ(github->calls.size(), writes);
  auto replay = post("events", activity("tool_running", 2));
  ASSERT_EQ(replay->status, 200);
  EXPECT_EQ(json::parse(replay->body)["superseded"], true);
  EXPECT_EQ(json::parse(replay->body)["record"]["activity"], "idle");
  EXPECT_EQ(github->calls.size(), writes);
}

TEST_F(FleetRoutes, CanonicalAssignmentAndDependenciesGateFleetAdmission) {
  seed_session();
  auto task = *store.get_hmas_task("task-1");
  task.assigned_lead_id = "legacy-agent@other-host";
  ASSERT_TRUE(store.update_hmas_task(task));
  EXPECT_EQ(post("sessions/session-1/start", command())->status, 409);
  task.assigned_lead_id.clear();
  task.state = TaskState::Delegated;
  ASSERT_TRUE(store.update_hmas_task(task));
  EXPECT_EQ(post("sessions/session-1/start", command())->status, 409);
  task.state = TaskState::Pending;
  task.blocked_by = {"unresolved-dependency"};
  ASSERT_TRUE(store.update_hmas_task(task));
  EXPECT_EQ(post("sessions/session-1/start", command())->status, 409);
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetRoutes, CanonicalTaskStartsOnlyAfterCurrentWorkerObservation) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::Delegated);
  auto stale = activity("model_working");
  stale["generation"] = 2;
  EXPECT_EQ(post("events", stale)->status, 409);
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::Delegated);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::InProgress);
  ASSERT_EQ(post("events", activity("idle", 2, "completed"))->status, 200);
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::InProgress);
}

TEST_F(FleetRoutes, ConcurrentLegacyAndFleetClaimHaveOnlyOneCanonicalWinner) {
  seed_session();
  auto stale = *store.get_hmas_task("task-1");
  stale.assigned_lead_id = "legacy-agent";
  stale.state = TaskState::InProgress;
  bool legacy_won = false;
  int fleet_status = 0;
  std::thread legacy([&] {
    try {
      legacy_won = store.update_hmas_task(stale);
    } catch (const std::runtime_error&) {
    }
  });
  std::thread fleet([&] {
    httplib::Client concurrent("127.0.0.1", server.is_running() ? client->port() : 0);
    concurrent.set_default_headers({{"Authorization", "Bearer fleet-test-key"}});
    fleet_status =
        concurrent.Post("/v1/fleet/sessions/session-1/start", command().dump(), "application/json")
            ->status;
  });
  legacy.join();
  fleet.join();
  EXPECT_TRUE((legacy_won && fleet_status == 409) || (!legacy_won && fleet_status == 202));
}

TEST_F(FleetRoutes, FleetClaimFencesLegacyStateAndCompletionWriters) {
  seed_session();
  auto stale = *store.get_hmas_task("task-1");
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  auto canonical = hmas_task_to_json(*store.get_hmas_task("task-1"));
  ASSERT_TRUE(canonical.contains("fleet_claim"));
  EXPECT_EQ(canonical["fleet_claim"]["workerId"], "worker-1");
  EXPECT_EQ(canonical["fleet_claim"]["generation"], 1);
  stale.state = TaskState::Completed;
  EXPECT_THROW(store.update_hmas_task(stale), std::runtime_error);
  EXPECT_THROW(store.update_hmas_task_state("task-1", TaskState::Completed), std::runtime_error);
  orchestrator.on_myrmidon_completion("hi.tasks.completed", R"({"task_id":"task-1"})");
  EXPECT_NE(store.get_hmas_task("task-1")->state, TaskState::Completed);
  EXPECT_THROW(store.create_hmas_task(stale), std::runtime_error);
}

TEST_F(FleetRoutes, FleetClaimRejectsLegacySplitBeforeCreatingChildren) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  const auto records = github->created_issues.size();
  const auto publications = publisher.calls.size();
  json result = json::object();
  EXPECT_NO_THROW(result =
                      orchestrator.split_task("task-1", json::array({{{"title", "remainder"}}})));
  EXPECT_EQ(result.value("error", ""), "Fleet-owned task requires generation-fenced planning");
  EXPECT_EQ(github->created_issues.size(), records);
  EXPECT_EQ(publisher.calls.size(), publications);
  EXPECT_TRUE(store.get_hmas_task("task-1")->child_task_ids.empty());
}

TEST_F(FleetRoutes, ConcurrentSplitAndFleetAdmissionHaveOneCanonicalWinner) {
  seed_session();
  const auto records = github->created_issues.size();
  json split;
  int admission = 0;
  std::thread planning([&] {
    try {
      split = orchestrator.split_task("task-1", json::array({{{"title", "remainder"}}}));
    } catch (const std::exception&) {
      split = {{"error", "split failed"}};
    }
  });
  std::thread claiming([&] { admission = post("sessions/session-1/start", command())->status; });
  planning.join();
  claiming.join();
  const auto task = store.get_hmas_task("task-1");
  if (split.contains("error")) {
    EXPECT_EQ(admission, 202);
    EXPECT_TRUE(task->child_task_ids.empty());
    EXPECT_EQ(github->created_issues.size(), records);
  } else {
    EXPECT_EQ(admission, 409);
    EXPECT_TRUE(task->fleet_claim.is_null());
    EXPECT_EQ(task->child_task_ids.size(), 1u);
    EXPECT_EQ(github->created_issues.size(), records + 1);
  }
}

TEST_F(FleetRoutes, FailedSplitParentWriteCannotCreateChildren) {
  seed_session();
  const auto records = github->created_issues.size();
  github->fail_update = true;
  EXPECT_THROW(orchestrator.split_task("task-1", json::array({{{"title", "remainder"}}})),
               std::runtime_error);
  EXPECT_EQ(github->created_issues.size(), records);
  EXPECT_TRUE(store.get_hmas_task("task-1")->child_task_ids.empty());
}

TEST_F(FleetRoutes, FleetClaimRejectsMisleadingLegacyCompletionSuccess) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  const auto state = store.get_hmas_task("task-1")->state;
  const auto response = client->Post("/v1/tasks/task-1/complete", "{}", "application/json");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 409);
  EXPECT_FALSE(json::parse(response->body).value("completed", false));
  EXPECT_EQ(store.get_hmas_task("task-1")->state, state);
}

TEST(FleetGitHub, AmbiguousCreationDoesNotRetryTheRequestAttempt) {
  class AmbiguousPost : public CurlGitHubClient {
   public:
    AmbiguousPost() : CurlGitHubClient("example/repo", "test-token") {}
    mutable int attempts = 0;
    bool transport_failure = true;

   protected:
    Response do_post_once(const std::string&, const std::string&) const override {
      if (++attempts == 1) {
        if (transport_failure) throw std::runtime_error("response lost after create");
        return {500, "upstream response lost after create", ""};
      }
      return {201, R"({"number":99})", ""};
    }
  } client;
  EXPECT_THROW(client.create_issue("title", "body", "label"), std::runtime_error);
  EXPECT_EQ(client.attempts, 1);
  client.attempts = 0;
  client.transport_failure = false;
  EXPECT_THROW(client.create_issue("title", "body", "label"), std::runtime_error);
  EXPECT_EQ(client.attempts, 1);
}

TEST_F(FleetRoutes, UncertainCanonicalClaimIsHydratedBeforeLegacyMutation) {
  seed_session();
  auto stale = *store.get_hmas_task("task-1");
  github->fail_after_commit = true;
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 503);
  github->fail_after_commit = false;
  auto canonical = hmas_task_to_json(*store.get_hmas_task("task-1"));
  ASSERT_TRUE(canonical.contains("fleet_claim"));
  EXPECT_FALSE(canonical["fleet_claim"].is_null());
  EXPECT_THROW(store.update_hmas_task(stale), std::runtime_error);
  EXPECT_TRUE(publisher.calls.empty());
  EXPECT_EQ(post("sessions/session-1/start", command())->status, 202);
}

TEST_F(FleetRoutes, ManualResolutionRequiresInactiveWorkAndKeepsApprovalProvenance) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  EXPECT_EQ(resolve(decision())->status, 409);
  ASSERT_EQ(post("events", activity("idle", 2, "completed"))->status, 200);
  EXPECT_EQ(post("sessions/session-1/resolve", decision())->status, 403);
  auto stale = decision();
  stale["generation"] = 2;
  EXPECT_EQ(resolve(stale)->status, 409);
  auto resolved = resolve(decision());
  ASSERT_EQ(resolved->status, 200) << resolved->body;
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::Completed);
  auto record = json::parse(resolved->body);
  EXPECT_EQ(record["claimStatus"], "released");
  EXPECT_EQ(record["resolution"]["provenance"], "manual");
  EXPECT_EQ(record["resolution"]["verifiedApproval"], false);
  auto writes = github->calls.size();
  EXPECT_EQ(resolve(decision())->status, 200);
  EXPECT_EQ(github->calls.size(), writes);
  auto changed = decision();
  changed["evidenceRef"] = "other-review";
  EXPECT_EQ(resolve(changed)->status, 409);
}

TEST_F(FleetRoutes, InputInvalidatesPreviousIdleBeforeManualResolution) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("events", activity("idle", 2, "completed"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/input", command("input-1"))->status, 202);
  auto ack = fact("completed", "input-ack");
  ack["commandId"] = "input-1";
  ack["receipt"] = {{"providerTurnId", "new-turn"}};
  ASSERT_EQ(post("sessions/session-1/ack", ack)->status, 200);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["observationState"], "awaiting_activity");
  EXPECT_EQ(resolve(decision())->status, 409);
}

TEST_F(FleetRoutes, CanonicalResolutionRetrySurvivesFleetWriteFailureAndFencesNewInput) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("events", activity("idle", 2, "completed"))->status, 200);
  github->fail_fleet_update = true;
  ASSERT_EQ(resolve(decision())->status, 503);
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::Completed);
  github->fail_fleet_update = false;
  EXPECT_EQ(post("sessions/session-1/input", command("input-after-resolution"))->status, 409);
  EXPECT_EQ(resolve(decision())->status, 200);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["claimStatus"], "released");
}

TEST_F(FleetRoutes, InterruptedWorkspaceRemainsOwnedWhileActiveCapacityIsReusable) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/interrupt", command("interrupt-1"))->status, 202);
  ASSERT_EQ(post("events", activity("idle", 2, "interrupted"))->status, 200);
  ASSERT_EQ(post("sessions", {{"id", "session-2"},
                              {"workerId", "worker-1"},
                              {"agentId", "agent-2"},
                              {"workspace", "/work/session-1"}})
                ->status,
            201);
  EXPECT_EQ(post("sessions/session-2/start", command("start-2"))->status, 409);
  ASSERT_EQ(post("sessions", {{"id", "session-3"},
                              {"workerId", "worker-1"},
                              {"agentId", "agent-3"},
                              {"workspace", "/work/other"}})
                ->status,
            201);
  EXPECT_EQ(post("sessions/session-3/start", command("start-3"))->status, 202);
}

TEST_F(FleetRoutes, ExportLifecycleEnvelopes) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/input", command("input-1"))->status, 202);
  auto ack = fact("completed", "input-ack");
  ack["commandId"] = "input-1";
  ASSERT_EQ(post("sessions/session-1/ack", ack)->status, 200);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/interrupt", command("interrupt-1"))->status, 202);
  ASSERT_EQ(post("events", activity("idle", 2, "interrupted"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/resume", command("resume-1"))->status, 202);
  ack = fact("completed", "resume-ack");
  ack["commandId"] = "resume-1";
  ASSERT_EQ(post("sessions/session-1/ack", ack)->status, 200);
  ASSERT_EQ(post("sessions/session-1/cancel", command("cancel-1"))->status, 202);
  json commands = json::array();
  for (const auto& publication : publisher.calls)
    commands.push_back(
        {{"subject", publication.subject}, {"command", json::parse(publication.payload)}});
  ASSERT_EQ(commands.size(), 5u);
  auto created = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  ASSERT_TRUE(created.contains("executionId"));
  ASSERT_TRUE(created["executionId"].is_string());
  EXPECT_FALSE(created["executionId"].get<std::string>().empty());
  for (const auto& emitted : commands)
    EXPECT_EQ(emitted["command"]["executionId"], created["executionId"]);
  if (const char* path = std::getenv("FLEET_CONTRACT_OUTPUT")) {
    std::ofstream output(path);
    ASSERT_TRUE(output.good());
    output << json{{"schema", "hi/fleet/contracts/v1"}, {"commands", commands}}.dump(2) << '\n';
    ASSERT_TRUE(output.good());
  }
}

TEST_F(FleetRoutes, AdapterCanVerifyOriginalDurableCommandAndCurrentClaim) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  auto response = client->Get("/v1/fleet/commands/command-1");
  ASSERT_EQ(response->status, 200);
  auto body = json::parse(response->body);
  EXPECT_TRUE(body["command"]["payload"].empty());
  EXPECT_EQ(body["record"]["claimStatus"], "reserved");
  EXPECT_EQ(body["record"]["generation"], 1);
  EXPECT_EQ(client->Get("/v1/fleet/commands/missing")->status, 404);
}

TEST_F(FleetRoutes, StopFencesNewInputAndRequiresCorrelatedConfirmation) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  ASSERT_EQ(post("sessions/session-1/cancel", command("cancel-1"))->status, 202);
  EXPECT_EQ(post("sessions/session-1/input", command("input-1"))->status, 409);
  auto stale = activity("idle", 2, "cancelled");
  stale["event"]["commandId"] = "old-cancel";
  EXPECT_EQ(post("events", stale)->status, 409);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["status"], "cancelling");
  EXPECT_EQ(record["claimStatus"], "claimed");
}

TEST_F(FleetRoutes, UnsolicitedIdleAndWrongAckSchemaCannotAlterState) {
  seed_session();
  EXPECT_EQ(post("events", activity("idle"))->status, 409);
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  auto wrong = fact("completed");
  wrong["schema"] = "hi/fleet/v9";
  EXPECT_EQ(post("sessions/session-1/ack", wrong)->status, 400);
}

TEST_F(FleetRoutes, ApprovalResponsePersistsReferenceWithoutPrivateContent) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  ASSERT_EQ(post("events", activity("waiting_approval"))->status, 200);
  auto response = command("respond-1");
  response["payload"] = {{"requestId", "approval-1"},
                         {"responseRef", std::string(32, 'b') + ".json"}};
  ASSERT_EQ(post("sessions/session-1/respond", response)->status, 202);
  auto persisted = json::parse(client->Get("/v1/fleet/commands/respond-1")->body);
  EXPECT_EQ(persisted["command"]["payload"]["responseRef"], std::string(32, 'b') + ".json");
}

TEST_F(FleetRoutes, NormalCommandsCannotOvertakePendingStart) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  EXPECT_EQ(post("sessions/session-1/input", command("input-1"))->status, 409);
  auto record = json::parse(client->Get("/v1/fleet/sessions/session-1")->body);
  EXPECT_EQ(record["commandId"], "command-1");
}

TEST_F(FleetRoutes, PrivateReferenceShapeIsBoundToOperation) {
  seed_session();
  auto invalid = command();
  invalid["payload"] = {{"inputRef", std::string(32, 'a') + ".json"}};
  EXPECT_EQ(post("sessions/session-1/start", invalid)->status, 400);
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/ack", fact("completed"))->status, 200);
  invalid = command("input-1");
  invalid["payload"]["promptRef"] = std::string(32, 'b') + ".json";
  EXPECT_EQ(post("sessions/session-1/input", invalid)->status, 400);
}

TEST(FleetGitHub, ClosedBackingRecordsRetainClaimsDuringHydration) {
  class ClosedListing : public ResponseGitHub {
   public:
    json issues;

   protected:
    Response do_get(const std::string& url) const override {
      return {200, url.find("state=all") == std::string::npos ? "[]" : issues.dump(), ""};
    }
  };
  auto github = std::make_shared<ClosedListing>();
  json document = {{"schema", "hi/fleet/v1"},
                   {"kind", "pools"},
                   {"record", {{"id", "closed-pool"}, {"capacity", 12}}},
                   {"commands", json::array()},
                   {"events", json::array()}};
  github->issues = json::array(
      {{{"number", 42},
        {"state", "closed"},
        {"body", "## AgamemnonEntity: fleet\n\n```json\n" + document.dump() + "\n```\n"}}});
  Store store(github);
  FakeNatsPublisher publisher;
  FleetService fleet(store, publisher);
  auto result = fleet.list("pools");
  EXPECT_EQ(result["total"], 1);
}

TEST_F(FleetRoutes, RestartRetainsClaimButDoesNotReportHistoricalWorkAsCurrent) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("events", activity("model_working"))->status, 200);
  FleetService restarted(store, publisher);
  auto record = restarted.get("sessions", "session-1");
  EXPECT_EQ(record["claimStatus"], "claimed");
  EXPECT_EQ(record["activity"], "unknown");
  EXPECT_EQ(record["observationState"], "reconciliation_required");
}

TEST_F(FleetRoutes, WorkerReceiptCannotPersistConversationContent) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  auto ack = fact("completed");
  ack["transcript"] = "private conversation content";
  EXPECT_EQ(post("sessions/session-1/ack", ack)->status, 400);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/commands/command-1")->body)["status"], "pending");
}

TEST_F(FleetRoutes, ConfirmedInterruptSupersedesPendingControlAndAllowsResume) {
  seed_session();
  ASSERT_EQ(post("sessions/session-1/start", command())->status, 202);
  ASSERT_EQ(post("sessions/session-1/interrupt", command("interrupt-1"))->status, 202);
  ASSERT_EQ(post("events", activity("idle", 1, "interrupted"))->status, 200);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/commands/command-1")->body)["status"], "superseded");
  EXPECT_EQ(post("sessions/session-1/resume", command("resume-1"))->status, 202);
}

}  // namespace agamemnon::test
