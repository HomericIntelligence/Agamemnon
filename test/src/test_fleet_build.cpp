#include "agamemnon/auth.hpp"
#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/fleet.hpp"
#include "agamemnon/github_client.hpp"
#include "agamemnon/metrics.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/rate_limiter.hpp"
#include "agamemnon/routes.hpp"
#include "agamemnon/store.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "httplib.h"
#include <gtest/gtest.h>

namespace agamemnon::test {
namespace {

class BuildBacking : public MockGitHubClient {
 public:
  bool lose_create_response = false;
  bool lose_update_response = false;
  bool reject_update = false;

  std::vector<json> list_issues(std::string_view label) override {
    auto result = MockGitHubClient::list_issues(label);
    for (const auto& [number, issue] : created_issues)
      if (issue.at("label") == label)
        result.push_back({{"number", std::stoi(number)}, {"body", issue.at("body")}});
    return result;
  }

  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    const auto result = MockGitHubClient::create_issue(title, body, label);
    if (lose_create_response)
      throw std::runtime_error("fixture: create response lost after commit");
    return result;
  }

  void update_issue_body(std::string_view number, std::string_view body) override {
    if (reject_update) throw std::runtime_error("fixture: update rejected before commit");
    MockGitHubClient::update_issue_body(number, body);
    if (lose_update_response)
      throw std::runtime_error("fixture: update response lost after commit");
  }
};

class BuildPublisher : public FakeNatsPublisher {
 public:
  bool unavailable = false;
  bool publish(const std::string& subject, const std::string& payload) override {
    FakeNatsPublisher::publish(subject, payload);
    return !unavailable;
  }
};

class FleetBuildRoutes : public ::testing::Test {
 protected:
  std::shared_ptr<BuildBacking> github = std::make_shared<BuildBacking>();
  BuildPublisher publisher;
  Store store{github};
  AuthMiddleware auth{"build-requester-fixture"};
  RateLimiter limiter{10000, 10000};
  MetricsRegistry metrics;
  Orchestrator orchestrator{store, publisher};
  std::unique_ptr<httplib::Server> server;
  std::thread listener;
  std::unique_ptr<httplib::Client> client;

  virtual json build_catalog() { return json::object(); }
  virtual json build_authorities() { return json::object(); }
  virtual json build_artifacts() { return json::object(); }
  virtual std::string parent_workspace() { return "/work/parent-source"; }

  std::shared_ptr<FleetService> configured_service(const json& catalog, const json& authorities) {
    return std::make_shared<FleetService>(store, publisher, &orchestrator, "", nullptr, catalog,
                                          authorities, build_artifacts());
  }

  void start_service(const json& catalog, const json& authorities) {
    server = std::make_unique<httplib::Server>();
    auto fleet = configured_service(catalog, authorities);
    register_routes(*server, store, publisher, limiter, auth, metrics, orchestrator, fleet);
    const auto port = server->bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    listener = std::thread([this] { server->listen_after_bind(); });
    server->wait_until_ready();
    client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2);
    client->set_read_timeout(2);
    client->set_write_timeout(2);
    client->set_default_headers({{"Authorization", "Bearer build-requester-fixture"}});
  }

  void SetUp() override { start_service(build_catalog(), build_authorities()); }

  void TearDown() override {
    if (server) server->stop();
    if (listener.joinable()) listener.join();
  }

  void restart_service(bool admission = true) {
    TearDown();
    start_service(admission ? build_catalog() : json::object(), build_authorities());
  }

  httplib::Result post(const std::string& path, const json& body) {
    return client->Post("/v1/fleet/" + path, body.dump(), "application/json");
  }

  httplib::Result supervisor_post(const std::string& path, const json& body,
                                  const std::string& key = "supervisor-only-fixture") {
    return client->Post("/v1/fleet/" + path, {{"X-Fleet-Build-Key", key}}, body.dump(),
                        "application/json");
  }

  void active_parent(const std::string& start_id = "parent-start") {
    ASSERT_EQ(post("pools", {{"id", "laptop"}, {"capacity", 1}})->status, 201);
    ASSERT_EQ(
        post("workers",
             {{"id", "laptop-worker"}, {"poolId", "laptop"}, {"capacity", 1}, {"host", "laptop"}})
            ->status,
        201);
    HmasTask task{};
    task.id = "real-parent-task";
    task.layer = HmasLayer::L3_TaskAgent;
    task.state = TaskState::Pending;
    task.repo = "HomericIntelligence/Hephaestus";
    store.create_hmas_task(task);
    ASSERT_EQ(post("sessions", {{"id", "parent-session"},
                                {"workerId", "laptop-worker"},
                                {"agentId", "parent-agent"},
                                {"executionId", "parent-execution"},
                                {"workspace", parent_workspace()},
                                {"taskId", task.id},
                                {"domain", "pipeline"},
                                {"hmasRole", "task-agent"}})
                  ->status,
              201);
    ASSERT_EQ(post("sessions/parent-session/start", {{"commandId", start_id},
                                                     {"idempotencyKey", start_id},
                                                     {"generation", 1},
                                                     {"payload", json::object()}})
                  ->status,
              202);
    ASSERT_EQ(
        post("events",
             {{"schema", "hi/fleet/v1"},
              {"eventId", "parent-active"},
              {"workerId", "laptop-worker"},
              {"targetKind", "sessions"},
              {"targetId", "parent-session"},
              {"generation", 1},
              {"sourceSequence", 1},
              {"kind", "activity"},
              {"event", {{"activity", "model_working"}, {"observedAt", "2026-09-13T00:00:00Z"}}}})
            ->status,
        200);
  }

  json submission() {
    return {{"schema", "hi/fleet/build-submit/v1"},
            {"workspaceId", "hephaestus-source"},
            {"recipeId", "hephaestus-test-unit-v1"},
            {"parameters", json::object()},
            {"idempotencyKey", "build-request-1"},
            {"parent",
             {{"targetKind", "sessions"},
              {"targetId", "parent-session"},
              {"sessionId", "parent-session"},
              {"executionId", "parent-execution"},
              {"generation", 1}}},
            {"snapshot",
             {{"reference", "snapshot-1"},
              {"manifestDigest", std::string(64, 'a')},
              {"baseCommit", std::string(40, 'b')},
              {"members", 2},
              {"bytes", 100},
              {"policyDigest", std::string(64, 'c')}}}};
  }
};

TEST_F(FleetBuildRoutes, MissingBuildPolicyPreservesTheRunningParentAndPublishesNothing) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto parent_before = client->Get("/v1/fleet/sessions/parent-session")->body;
  const auto backing_count = github->created_issues.size();
  const auto publication_count = publisher.calls.size();

  auto response = post("build-jobs/submit", submission());
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 503) << response->body;
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
  EXPECT_EQ(client->Get("/v1/fleet/sessions/parent-session")->body, parent_before);
  EXPECT_EQ(github->created_issues.size(), backing_count);
  EXPECT_EQ(publisher.calls.size(), publication_count);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/build-jobs")->body)["items"].size(), 0u);
}

TEST_F(FleetBuildRoutes, SubmissionUsesExistingAuthenticationAndRequestBounds) {
  client->set_default_headers({});
  EXPECT_EQ(post("build-jobs/submit", submission())->status, 401);
  client->set_default_headers({{"Authorization", "Bearer build-requester-fixture"}});
  auto malformed = client->Post("/v1/fleet/build-jobs/submit", "{", "application/json");
  ASSERT_TRUE(malformed);
  EXPECT_EQ(malformed->status, 400);
  auto oversized = submission();
  oversized["idempotencyKey"] = std::string(17000, 'x');
  EXPECT_EQ(post("build-jobs/submit", oversized)->status, 413);
  EXPECT_TRUE(github->created_issues.empty());
  EXPECT_TRUE(publisher.calls.empty());
}

// These are explicitly synthetic operator inputs. They grant no host, cluster or
// runtime authority and are used only with the controlled publisher/GitHub fixture.
class FleetConfiguredBuildRoutes : public FleetBuildRoutes {
 protected:
  static constexpr std::int64_t gib = 1024LL * 1024 * 1024;

  json build_catalog() override {
    const json workspace = {{"id", "hephaestus-source"},
                            {"repository", "HomericIntelligence/Hephaestus"},
                            {"parentWorkspace", "/work/parent-source"},
                            {"snapshotPolicyDigest", std::string(64, 'c')}};
    const json recipe = {{"id", "hephaestus-test-unit-v1"},
                         {"repository", "HomericIntelligence/Hephaestus"},
                         {"argv", {"just", "test-unit"}},
                         {"parameters", json::object()},
                         {"recipeDigest", std::string(64, 'd')},
                         {"lockDigest", std::string(64, 'e')},
                         {"platform", "linux/aarch64"},
                         {"imageDigest", "sha256:" + std::string(64, 'f')},
                         {"toolchainDigest", std::string(64, '1')},
                         {"resources",
                          {{"cpus", 2},
                           {"gpus", 0},
                           {"memoryBytes", 2 * gib},
                           {"diskBytes", 4 * gib},
                           {"wallSeconds", 120},
                           {"outputBytes", 65536},
                           {"artifactBytes", 1048576},
                           {"snapshotBytes", 16777216},
                           {"snapshotMembers", 1000}}}};
    const json allocation = {
        {"id", "tool-allocation-1"},
        {"workerId", "tool-worker-1"},
        {"generation", 1},
        {"authorityId", "supervisor-1"},
        {"platform", "linux/aarch64"},
        {"imageDigest", "sha256:" + std::string(64, 'f')},
        {"toolchainDigest", std::string(64, '1')},
        {"qualificationReceiptDigest", std::string(64, '2')},
        {"resources",
         {{"cpus", 18}, {"gpus", 0}, {"memoryBytes", 72 * gib}, {"diskBytes", 64 * gib}}},
        {"supervision", {{"cpus", 2}, {"memoryBytes", 8 * gib}}}};
    return {{"schema", "hi/fleet/build-catalog/v1"},
            {"workspaces", json::array({workspace})},
            {"recipes", json::array({recipe})},
            {"allocations", json::array({allocation})}};
  }

  json build_authorities() override {
    return {{"schema", "hi/fleet/build-authorities/v1"},
            {"authorities",
             {{{"id", "supervisor-1"},
               {"workerId", "tool-worker-1"},
               {"allocationId", "tool-allocation-1"},
               {"generation", 1},
               {"key", "supervisor-only-fixture"}}}}};
  }
};

TEST_F(FleetConfiguredBuildRoutes, AdmissionPreservesTheParentAndUsesSeparateToolCapacity) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto parent_before = client->Get("/v1/fleet/sessions/parent-session")->body;
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto calls_before = publisher.calls.size();
  const auto response = post("build-jobs/submit", submission());
  ASSERT_TRUE(response);
  ASSERT_EQ(response->status, 202) << response->body;
  const auto result = json::parse(response->body);
  const auto& record = result.at("record");
  EXPECT_EQ(record.at("kind"), "build-jobs");
  EXPECT_FALSE(record.contains("taskId"));
  EXPECT_FALSE(record.contains("agentId"));
  EXPECT_EQ(record.at("parent").at("taskId"), "real-parent-task");
  EXPECT_EQ(record.at("parent").at("claim"), task_before.at("fleet_claim"));
  EXPECT_EQ(record.at("build").at("reservation"), "reserved");
  EXPECT_EQ(record.at("build").at("allocation").at("id"), "tool-allocation-1");
  EXPECT_NE(record.at("build").at("snapshotWorkspace"), "/work/parent-source");
  EXPECT_FALSE(record.at("collectionVerified").get<bool>());
  ASSERT_EQ(publisher.calls.size(), calls_before + 1);
  EXPECT_EQ(publisher.calls.back().subject, "hi.fleet.control.tool-worker-1");
  EXPECT_EQ(json::parse(publisher.calls.back().payload), result.at("command"));
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
  EXPECT_EQ(client->Get("/v1/fleet/sessions/parent-session")->body, parent_before);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/workers")->body).at("total"), 1);
  const auto calls_after = publisher.calls.size();
  auto second = submission();
  second["idempotencyKey"] = "second-build";
  EXPECT_EQ(post("build-jobs/submit", second)->status, 409);
  EXPECT_EQ(publisher.calls.size(), calls_after);
}

TEST_F(FleetConfiguredBuildRoutes, ClosedInputsAndStaleParentFailBeforeAnyWriteOrPublication) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto backing_before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  auto request = submission();
  request["parameters"] = {{"command", "arbitrary"}};
  EXPECT_EQ(post("build-jobs/submit", request)->status, 400);
  request = submission();
  request["recipeId"] = "unregistered";
  EXPECT_EQ(post("build-jobs/submit", request)->status, 400);
  request = submission();
  request["workspaceId"] = "unregistered";
  EXPECT_EQ(post("build-jobs/submit", request)->status, 400);
  request = submission();
  request["snapshot"]["members"] = 1.5;
  EXPECT_EQ(post("build-jobs/submit", request)->status, 400);
  request = submission();
  request["snapshot"]["manifestDigest"] = "not-a-digest";
  EXPECT_EQ(post("build-jobs/submit", request)->status, 400);
  request = submission();
  request["parent"]["generation"] = 2;
  EXPECT_EQ(post("build-jobs/submit", request)->status, 409);
  request = submission();
  request["parent"]["executionId"] = "another-execution";
  EXPECT_EQ(post("build-jobs/submit", request)->status, 409);
  EXPECT_EQ(github->created_issues, backing_before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
}

TEST_F(FleetConfiguredBuildRoutes, SameRequestReplaysOneIdentityAndChangedIntentConflicts) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto request = submission();
  const auto first = post("build-jobs/submit", request);
  ASSERT_TRUE(first);
  ASSERT_EQ(first->status, 202) << first->body;
  const auto first_result = json::parse(first->body);
  const auto backing_before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  const auto repeated = post("build-jobs/submit", request);
  ASSERT_TRUE(repeated);
  ASSERT_EQ(repeated->status, 202) << repeated->body;
  EXPECT_EQ(json::parse(repeated->body).at("record"), first_result.at("record"));
  EXPECT_EQ(github->created_issues, backing_before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
  auto changed = request;
  changed["snapshot"]["manifestDigest"] = std::string(64, '3');
  EXPECT_EQ(post("build-jobs/submit", changed)->status, 409);
  EXPECT_EQ(github->created_issues, backing_before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/build-jobs")->body).at("total"), 1);
}

json run_claim(const json& admitted) {
  return {{"schema", "hi/fleet/build-claim/v1"},
          {"workerId", "tool-worker-1"},
          {"allocationId", "tool-allocation-1"},
          {"generation", 1},
          {"attempt", 1},
          {"commandId", admitted.at("command").at("commandId")},
          {"claimId", "supervisor-claim-1"}};
}

TEST_F(FleetConfiguredBuildRoutes, LostCreateAcknowledgementRehydratesOneChildWithoutRepublishing) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto writes_before = github->created_issues.size();
  const auto publications_before = publisher.calls.size();
  github->lose_create_response = true;
  EXPECT_EQ(post("build-jobs/submit", submission())->status, 503);
  github->lose_create_response = false;
  ASSERT_EQ(github->created_issues.size(), writes_before + 1);
  EXPECT_EQ(publisher.calls.size(), publications_before);
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  const auto replay = post("build-jobs/submit", submission());
  ASSERT_EQ(replay->status, 202) << replay->body;
  const auto record = json::parse(replay->body).at("record");
  EXPECT_EQ(record.at("build").at("reservation"), "reserved");
  EXPECT_EQ(github->created_issues.size(), writes_before + 1);
  EXPECT_EQ(publisher.calls.size(), publications_before);
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/build-jobs")->body).at("total"), 1);
}

TEST_F(FleetConfiguredBuildRoutes, GrantRequiresDedicatedAuthorityAndAnExactImmutableClaim) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id + "/claim-run";
  auto claim = run_claim(admitted);
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  EXPECT_EQ(post(path, claim)->status, 403);
  EXPECT_EQ(supervisor_post(path, claim, "build-requester-fixture")->status, 403);
  auto wrong = claim;
  wrong["allocationId"] = "other-allocation";
  EXPECT_EQ(supervisor_post(path, wrong)->status, 409);
  EXPECT_EQ(github->created_issues, before);
  const auto granted = supervisor_post(path, claim);
  ASSERT_EQ(granted->status, 200) << granted->body;
  const auto result = json::parse(granted->body);
  EXPECT_EQ(result.at("grant").at("claim"), claim);
  EXPECT_EQ(result.at("command"), admitted.at("command"));
  const auto after = github->created_issues;
  EXPECT_EQ(json::parse(supervisor_post(path, claim)->body), result);
  EXPECT_EQ(github->created_issues, after);
  claim["claimId"] = "another-claim";
  EXPECT_EQ(supervisor_post(path, claim)->status, 409);
  EXPECT_EQ(publisher.calls.size(), calls_before);
}

TEST_F(FleetConfiguredBuildRoutes, StopBeforeGrantDeniesButSubmissionReplayRemainsReadOnly) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  ASSERT_EQ(post("sessions/parent-session/cancel", {{"commandId", "parent-stop"},
                                                    {"idempotencyKey", "parent-stop"},
                                                    {"generation", 1},
                                                    {"payload", json::object()}})
                ->status,
            202);
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  EXPECT_EQ(supervisor_post("build-jobs/" + id + "/claim-run", run_claim(admitted))->status, 409);
  EXPECT_EQ(post("build-jobs/submit", submission())->status, 202);
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
}

TEST_F(FleetConfiguredBuildRoutes, LostGrantResponseThenParentStopReplaysOnlyTheDurableGrant) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id + "/claim-run";
  const auto claim = run_claim(admitted);
  github->lose_update_response = true;
  EXPECT_EQ(supervisor_post(path, claim)->status, 503);
  github->lose_update_response = false;
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_EQ(post("sessions/parent-session/cancel", {{"commandId", "parent-stop"},
                                                    {"idempotencyKey", "parent-stop"},
                                                    {"generation", 1},
                                                    {"payload", json::object()}})
                ->status,
            202);
  const auto before = github->created_issues;
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto replay = supervisor_post(path, claim);
  ASSERT_EQ(replay->status, 200) << replay->body;
  EXPECT_EQ(json::parse(replay->body).at("grant").at("claim"), claim);
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
  auto changed = claim;
  changed["claimId"] = "replacement-claim";
  EXPECT_EQ(supervisor_post(path, changed)->status, 409);
}

TEST_F(FleetConfiguredBuildRoutes, FailedGrantWriteThenStopCannotBecomeHistoricalPermission) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id + "/claim-run";
  const auto claim = run_claim(admitted);
  github->reject_update = true;
  EXPECT_EQ(supervisor_post(path, claim)->status, 503);
  github->reject_update = false;
  restart_service();
  ASSERT_FALSE(HasFatalFailure());
  const auto before = github->created_issues;
  EXPECT_EQ(supervisor_post(path, claim)->status,
            409);  // Parent observation is unknown after hydration.
  EXPECT_EQ(github->created_issues, before);
  ASSERT_EQ(post("sessions/parent-session/cancel", {{"commandId", "parent-stop"},
                                                    {"idempotencyKey", "parent-stop"},
                                                    {"generation", 1},
                                                    {"payload", json::object()}})
                ->status,
            202);
  EXPECT_EQ(supervisor_post(path, claim)->status, 409);
}

json build_cancel() {
  return {{"schema", "hi/fleet/build-cancel/v1"},
          {"commandId", "build-stop-1"},
          {"idempotencyKey", "build-stop-key-1"},
          {"generation", 1},
          {"attempt", 1}};
}

json terminal_fact(const json& admitted, bool cancelled = false) {
  const auto& build = admitted.at("record").at("build");
  const auto& recipe = build.at("policy").at("recipe");
  return {
      {"schema", "hi/fleet/build-fact/v1"},
      {"eventId", "terminal-1"},
      {"workerId", "tool-worker-1"},
      {"allocationId", "tool-allocation-1"},
      {"generation", 1},
      {"attempt", 1},
      {"commandId", cancelled ? json("build-stop-1") : admitted.at("command").at("commandId")},
      {"policyDigest", build.at("policyDigest")},
      {"parametersDigest", build.at("parametersDigest")},
      {"snapshotDigest", build.at("request").at("snapshot").at("manifestDigest")},
      {"platform", recipe.at("platform")},
      {"imageDigest", recipe.at("imageDigest")},
      {"toolchainDigest", recipe.at("toolchainDigest")},
      {"outcome", cancelled ? "cancelled" : "completed"},
      {"exitCode", cancelled ? json(nullptr) : json(0)},
      {"cleanup", "confirmed_empty"},
      {"startFenced", cancelled},
      {"receipt", cancelled ? json(nullptr)
                            : json{{"reference", "receipt-1"}, {"digest", std::string(64, '4')}}},
      {"logs", nullptr},
      {"artifacts", nullptr}};
}

TEST_F(FleetConfiguredBuildRoutes, CleanupCancellationUsesStoredIdentityWithAdmissionDisabled) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id;
  ASSERT_EQ(post("sessions/parent-session/cancel", {{"commandId", "parent-stop"},
                                                    {"idempotencyKey", "parent-stop"},
                                                    {"generation", 1},
                                                    {"payload", json::object()}})
                ->status,
            202);
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto parent_before = client->Get("/v1/fleet/sessions/parent-session")->body;
  const auto stopped = post(path + "/cancel", build_cancel());
  ASSERT_EQ(stopped->status, 202) << stopped->body;
  EXPECT_EQ(json::parse(client->Get("/v1/fleet/" + path)->body).at("build").at("reservation"),
            "reserved");
  auto fact = terminal_fact(admitted, true);
  const auto before = github->created_issues;
  EXPECT_EQ(post(path + "/facts", fact)->status, 403);
  fact["startFenced"] = false;
  EXPECT_EQ(supervisor_post(path + "/facts", fact)->status, 409);
  fact["startFenced"] = true;
  fact["allocationId"] = "wrong-allocation";
  EXPECT_EQ(supervisor_post(path + "/facts", fact)->status, 409);
  EXPECT_EQ(github->created_issues, before);
  fact["allocationId"] = "tool-allocation-1";
  const auto cleanup = supervisor_post(path + "/facts", fact);
  ASSERT_EQ(cleanup->status, 200) << cleanup->body;
  const auto record = json::parse(cleanup->body).at("record");
  EXPECT_EQ(record.at("status"), "cancelled");
  EXPECT_EQ(record.at("build").at("reservation"), "released");
  EXPECT_EQ(record.at("build").at("evidenceState"), "incomplete");
  EXPECT_FALSE(record.at("collectionVerified").get<bool>());
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
  EXPECT_EQ(client->Get("/v1/fleet/sessions/parent-session")->body, parent_before);
}

TEST_F(FleetConfiguredBuildRoutes, CleanupTerminalRequiresGrantAndBindingsWithoutSelfVerification) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id;
  const auto fact = terminal_fact(admitted);
  EXPECT_EQ(supervisor_post(path + "/facts", fact)->status, 409);
  ASSERT_EQ(supervisor_post(path + "/claim-run", run_claim(admitted))->status, 200);
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto before = github->created_issues;
  auto wrong = fact;
  wrong["snapshotDigest"] = std::string(64, '5');
  EXPECT_EQ(supervisor_post(path + "/facts", wrong)->status, 409);
  wrong = fact;
  wrong["attempt"] = 2;
  EXPECT_EQ(supervisor_post(path + "/facts", wrong)->status, 409);
  wrong = fact;
  wrong["collectionVerified"] = true;
  EXPECT_EQ(supervisor_post(path + "/facts", wrong)->status, 400);
  wrong = fact;
  wrong["cleanup"] = "unknown";
  EXPECT_EQ(supervisor_post(path + "/facts", wrong)->status, 409);
  const json generic = {{"schema", "hi/fleet/v1"},
                        {"workerId", "tool-worker-1"},
                        {"targetKind", "build-jobs"},
                        {"targetId", id},
                        {"generation", 1},
                        {"commandId", admitted.at("command").at("commandId")},
                        {"eventId", "forged-ack"},
                        {"status", "completed"}};
  EXPECT_GE(post(path + "/ack", generic)->status, 400);
  EXPECT_GE(post("events", generic)->status, 400);
  EXPECT_EQ(github->created_issues, before);
  const auto result = supervisor_post(path + "/facts", fact);
  ASSERT_EQ(result->status, 200) << result->body;
  EXPECT_EQ(json::parse(result->body).at("record").at("build").at("reservation"), "released");
  EXPECT_FALSE(json::parse(result->body).at("record").at("collectionVerified").get<bool>());
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
}

TEST_F(FleetConfiguredBuildRoutes, CleanupLostStopAndTerminalResponsesKeepTheirExactIdentities) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto path = "build-jobs/" + admitted.at("record").at("id").get<std::string>();
  const auto publications_before = publisher.calls.size();
  github->lose_update_response = true;
  EXPECT_EQ(post(path + "/cancel", build_cancel())->status, 503);
  github->lose_update_response = false;
  EXPECT_EQ(publisher.calls.size(), publications_before);
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  const auto stop = post(path + "/cancel", build_cancel());
  ASSERT_EQ(stop->status, 202) << stop->body;
  ASSERT_EQ(publisher.calls.size(), publications_before + 1);
  EXPECT_EQ(json::parse(publisher.calls.back().payload), json::parse(stop->body).at("command"));
  const auto before = github->created_issues;
  auto changed = build_cancel();
  changed["commandId"] = "different-stop";
  EXPECT_EQ(post(path + "/cancel", changed)->status, 409);
  EXPECT_EQ(github->created_issues, before);
  const auto fact = terminal_fact(admitted, true);
  github->lose_update_response = true;
  EXPECT_EQ(supervisor_post(path + "/facts", fact)->status, 503);
  github->lose_update_response = false;
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  const auto terminal_before = github->created_issues;
  const auto replay = supervisor_post(path + "/facts", fact);
  ASSERT_EQ(replay->status, 200) << replay->body;
  EXPECT_EQ(json::parse(replay->body).at("record").at("build").at("reservation"), "released");
  EXPECT_EQ(github->created_issues, terminal_before);
}

// Synthetic historical records below model allocations accepted by an older
// controller. No real provider, broker, allocation, or GitHub service is used.
class FleetBuildAllocationRoutes : public FleetConfiguredBuildRoutes {
 protected:
  json admitted;
  std::string build_path;
  struct Snapshot {
    decltype(BuildBacking{}.created_issues) backing;
    std::size_t publications;
    std::size_t durable_writes;
    json parent_task;
    json child;
  };

  void SetUp() override {
    FleetConfiguredBuildRoutes::SetUp();
    ASSERT_FALSE(HasFatalFailure());
    active_parent();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(post("pools", {{"id", "provider-pool"}, {"capacity", 8}})->status, 201);
  }
  json provider(const std::string& id = "provider-worker",
                const json& allocation = "provider-allocation") {
    return {{"id", id},
            {"poolId", "provider-pool"},
            {"capacity", 1},
            {"host", "separate-provider-host"},
            {"allocationId", allocation}};
  }
  void add_provider(const json& allocation = "provider-allocation") {
    const auto response = post("workers", provider("provider-worker", allocation));
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 201) << response->body;
  }
  json record(const std::string& path) {
    const auto response = client->Get("/v1/fleet/" + path);
    EXPECT_TRUE(response);
    if (!response) return json::object();
    EXPECT_EQ(response->status, 200) << response->body;
    return json::parse(response->body);
  }
  void admit() {
    const auto response = post("build-jobs/submit", submission());
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 202) << response->body;
    admitted = json::parse(response->body);
    build_path = "build-jobs/" + admitted.at("record").at("id").get<std::string>();
  }
  std::size_t writes() const {
    std::size_t count = 0;
    for (const auto& call : github->calls)
      if (call.method == "create_issue" || call.method == "update_issue_body") ++count;
    return count;
  }
  Snapshot checkpoint() {
    return {github->created_issues, publisher.calls.size(), writes(),
            hmas_task_to_json(*store.get_hmas_task("real-parent-task")),
            admitted.is_null() ? json() : record(build_path)};
  }
  void unchanged(const Snapshot& before) {
    EXPECT_EQ(github->created_issues, before.backing);
    EXPECT_EQ(publisher.calls.size(), before.publications);
    EXPECT_EQ(writes(), before.durable_writes);
    EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), before.parent_task);
    if (!before.child.is_null()) EXPECT_EQ(record(build_path), before.child);
  }
  void refresh_parent() {
    const auto response =
        post("events",
             {{"schema", "hi/fleet/v1"},
              {"eventId", "parent-refreshed"},
              {"workerId", "laptop-worker"},
              {"targetKind", "sessions"},
              {"targetId", "parent-session"},
              {"generation", 1},
              {"sourceSequence", 2},
              {"kind", "activity"},
              {"event", {{"activity", "model_working"}, {"observedAt", "2026-09-16T00:00:00Z"}}}});
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200) << response->body;
    const auto parent = record("sessions/parent-session");
    ASSERT_EQ(parent.at("observationState"), "observed");
    ASSERT_EQ(parent.at("status"), "running");
    ASSERT_EQ(parent.at("claimStatus"), "claimed");
  }
  void restart_with_historical_overlap(bool admission = true) {
    TearDown();
    bool found_worker = false;
    for (auto& [number, issue] : github->created_issues) {
      if (issue.at("label") != "agamemnon-fleet") continue;
      const auto body = issue.at("body").get<std::string>();
      const auto begin = body.find("```json\n");
      const auto end = body.find("\n```", begin + 8);
      auto document = json::parse(body.substr(begin + 8, end - begin - 8));
      auto& retained = document.at("record");
      const bool worker =
          document.at("kind") == "workers" && retained.at("id") == "provider-worker";
      if (!worker && retained.value("workerId", "") != "provider-worker") continue;
      found_worker = found_worker || worker;
      retained["allocationId"] = "tool-allocation-1";
      for (auto& event : document.at("events"))
        if (event.contains("record")) event["record"]["allocationId"] = "tool-allocation-1";
      issue["body"] = "## AgamemnonEntity: fleet\n\n```json\n" + document.dump() + "\n```\n";
    }
    ASSERT_TRUE(found_worker);
    start_service(admission ? build_catalog() : json::object(), build_authorities());
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(record("workers/provider-worker").at("allocationId"), "tool-allocation-1");
  }
  json control(const std::string& operation) {
    const auto id = "provider-" + operation;
    return {
        {"commandId", id}, {"idempotencyKey", id}, {"generation", 1}, {"payload", json::object()}};
  }
};

TEST_F(FleetBuildAllocationRoutes, CatalogRejectsSharedAllocationBeforeAnyBuildExists) {
  const auto before = checkpoint();
  const auto denied = post("workers", provider("conflicting-provider", "tool-allocation-1"));
  ASSERT_TRUE(denied);
  EXPECT_EQ(denied->status, 409) << denied->body;
  unchanged(before);
  EXPECT_EQ(record("build-jobs").at("total"), 0);
  // Explicit separate capacity and optional legacy allocation metadata remain usable.
  for (const auto& [id, allocation] : std::array<std::pair<const char*, json>, 2>{
           {{"separate", "other-allocation"}, {"legacy", nullptr}}})
    EXPECT_EQ(post("workers", provider(id, allocation))->status, 201);
  auto legacy = provider("unspecified");
  legacy.erase("allocationId");
  EXPECT_EQ(post("workers", legacy)->status, 201);
}

TEST_F(FleetBuildAllocationRoutes, ProviderFirstRejectsBuildWithoutChangingParentOrPublishing) {
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  add_provider("tool-allocation-1");
  ASSERT_FALSE(HasFatalFailure());
  restart_service();
  ASSERT_FALSE(HasFatalFailure());
  refresh_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto before = checkpoint();
  const auto response = post("build-jobs/submit", submission());
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 409) << response->body;
  unchanged(before);
  EXPECT_EQ(record("build-jobs").at("total"), 0);
}

TEST_F(FleetBuildAllocationRoutes, RetainedReservationBlocksRegistrationUntilConfirmedRelease) {
  admit();
  ASSERT_FALSE(HasFatalFailure());
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  refresh_parent();
  ASSERT_FALSE(HasFatalFailure());
  for (const auto& phase : {"admitted", "authorized", "cancelling"}) {
    SCOPED_TRACE(phase);
    if (std::string(phase) == "authorized")
      ASSERT_EQ(supervisor_post(build_path + "/claim-run", run_claim(admitted))->status, 200);
    if (std::string(phase) == "cancelling")
      ASSERT_EQ(post(build_path + "/cancel", build_cancel())->status, 202);
    ASSERT_EQ(record(build_path).at("build").at("reservation"), "reserved");
    const auto before = checkpoint();
    const auto denied =
        post("workers", provider(std::string("provider-") + phase, "tool-allocation-1"));
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 409) << denied->body;
    unchanged(before);
  }
  const auto parent = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  ASSERT_EQ(supervisor_post(build_path + "/facts", terminal_fact(admitted, true))->status, 200);
  EXPECT_EQ(record(build_path).at("build").at("reservation"), "released");
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), parent);
  EXPECT_EQ(post("workers", provider("after-release", "tool-allocation-1"))->status, 201);
  restart_service();
  ASSERT_FALSE(HasFatalFailure());
  const auto before = checkpoint();
  EXPECT_EQ(post("workers", provider("still-configured", "tool-allocation-1"))->status, 409);
  unchanged(before);
}

TEST_F(FleetBuildAllocationRoutes, RestartConflictFencesFreshDeliveryAndGrantWithAnActiveParent) {
  add_provider();
  admit();
  ASSERT_FALSE(HasFatalFailure());
  restart_with_historical_overlap(false);
  ASSERT_FALSE(HasFatalFailure());
  refresh_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto before = checkpoint();
  const auto replay = post("build-jobs/submit", submission());
  ASSERT_TRUE(replay);
  ASSERT_EQ(replay->status, 202) << replay->body;
  EXPECT_EQ(json::parse(replay->body).at("record"), before.child);
  EXPECT_EQ(json::parse(replay->body).at("command"), admitted.at("command"));
  unchanged(before);
  const json delivery = {{"schema", "hi/fleet/build-delivery/v1"},
                         {"commandId", admitted.at("command").at("commandId")},
                         {"generation", 1},
                         {"attempt", 1}};
  const auto delivered = post(build_path + "/deliver", delivery);
  ASSERT_TRUE(delivered);
  EXPECT_EQ(delivered->status, 409) << delivered->body;
  unchanged(before);
  const auto granted = supervisor_post(build_path + "/claim-run", run_claim(admitted));
  ASSERT_TRUE(granted);
  EXPECT_EQ(granted->status, 409) << granted->body;
  unchanged(before);
}

TEST_F(FleetBuildAllocationRoutes, RestartConflictPreservesHistoricalReplaysAndTerminalCleanup) {
  add_provider();
  admit();
  ASSERT_FALSE(HasFatalFailure());
  const auto granted = supervisor_post(build_path + "/claim-run", run_claim(admitted));
  ASSERT_TRUE(granted);
  ASSERT_EQ(granted->status, 200) << granted->body;
  const auto original_grant = json::parse(granted->body);
  restart_with_historical_overlap(false);
  ASSERT_FALSE(HasFatalFailure());
  // Parent observation intentionally stays unknown: historical replay needs no new admission.
  const auto before = checkpoint();
  const auto replay = post("build-jobs/submit", submission());
  ASSERT_TRUE(replay);
  ASSERT_EQ(replay->status, 202) << replay->body;
  EXPECT_EQ(json::parse(replay->body).at("record"), before.child);
  EXPECT_EQ(json::parse(replay->body).at("command"), admitted.at("command"));
  const auto grant_replay = supervisor_post(build_path + "/claim-run", run_claim(admitted));
  ASSERT_TRUE(grant_replay);
  ASSERT_EQ(grant_replay->status, 200) << grant_replay->body;
  EXPECT_EQ(json::parse(grant_replay->body), original_grant);
  auto changed = run_claim(admitted);
  changed["claimId"] = "different-historical-claim";
  EXPECT_EQ(supervisor_post(build_path + "/claim-run", changed)->status, 409);
  unchanged(before);
  ASSERT_EQ(supervisor_post(build_path + "/facts", terminal_fact(admitted))->status, 200);
  EXPECT_EQ(record(build_path).at("build").at("reservation"), "released");
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), before.parent_task);
}

TEST_F(FleetBuildAllocationRoutes, RestartConflictKeepsInspectionCancellationAndExactCleanup) {
  add_provider();
  admit();
  ASSERT_FALSE(HasFatalFailure());
  restart_with_historical_overlap(false);
  ASSERT_FALSE(HasFatalFailure());
  const auto before = checkpoint();
  EXPECT_EQ(record("build-jobs").at("total"), 1);
  EXPECT_EQ(
      record("commands/" + admitted.at("command").at("commandId").get<std::string>()).at("record"),
      before.child);
  EXPECT_TRUE(record("events?after=0").contains("events"));
  unchanged(before);
  const auto stop = post(build_path + "/cancel", build_cancel());
  ASSERT_TRUE(stop);
  ASSERT_EQ(stop->status, 202) << stop->body;
  ASSERT_EQ(publisher.calls.size(), before.publications + 1);
  const auto command = json::parse(stop->body).at("command");
  EXPECT_EQ(json::parse(publisher.calls.back().payload), command);
  const auto stopping = checkpoint();
  ASSERT_EQ(post(build_path + "/cancel", build_cancel())->status, 202);
  EXPECT_EQ(github->created_issues, stopping.backing);
  EXPECT_EQ(writes(), stopping.durable_writes);
  ASSERT_EQ(publisher.calls.size(), stopping.publications + 1);
  EXPECT_EQ(json::parse(publisher.calls.back().payload), command);
  const auto retry = checkpoint();
  auto wrong = terminal_fact(admitted, true);
  wrong["allocationId"] = "wrong-allocation";
  EXPECT_EQ(supervisor_post(build_path + "/facts", wrong)->status, 409);
  wrong = terminal_fact(admitted, true);
  wrong["cleanup"] = "unknown";
  EXPECT_EQ(supervisor_post(build_path + "/facts", wrong)->status, 409);
  unchanged(retry);
  ASSERT_EQ(supervisor_post(build_path + "/facts", terminal_fact(admitted, true))->status, 200);
  EXPECT_EQ(record(build_path).at("build").at("reservation"), "released");
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), before.parent_task);
  EXPECT_EQ(publisher.calls.size(), retry.publications);
}

using ProviderActivationCase = std::pair<const char*, const char*>;
class FleetBuildProviderActivation : public FleetBuildAllocationRoutes,
                                     public ::testing::WithParamInterface<ProviderActivationCase> {
};

TEST_P(FleetBuildProviderActivation, RestartConflictFencesNewAndPendingButAllowsReadOnlyReplay) {
  const std::string operation = GetParam().first;
  const std::string phase = GetParam().second;
  add_provider();
  admit();
  ASSERT_FALSE(HasFatalFailure());
  HmasTask task{};
  task.id = "provider-task";
  task.layer = HmasLayer::L3_TaskAgent;
  task.state = TaskState::Pending;
  store.create_hmas_task(task);
  ASSERT_EQ(post("sessions", {{"id", "provider-session"},
                              {"workerId", "provider-worker"},
                              {"agentId", "provider-agent"},
                              {"workspace", "/work/provider"},
                              {"taskId", task.id},
                              {"domain", "pipeline"},
                              {"hmasRole", "task-agent"}})
                ->status,
            201);
  const std::string path = "sessions/provider-session/";
  if (operation == "resume") {
    ASSERT_EQ(post(path + "start", control("start"))->status, 202);
    ASSERT_EQ(post(path + "interrupt", control("interrupt"))->status, 202);
    const auto stopped = post("events", {{"schema", "hi/fleet/v1"},
                                         {"eventId", "provider-stopped"},
                                         {"workerId", "provider-worker"},
                                         {"targetKind", "sessions"},
                                         {"targetId", "provider-session"},
                                         {"generation", 1},
                                         {"sourceSequence", 1},
                                         {"kind", "activity"},
                                         {"event",
                                          {{"activity", "idle"},
                                           {"outcome", "interrupted"},
                                           {"backgroundCleanup", "confirmed_empty"},
                                           {"commandId", "provider-interrupt"},
                                           {"observedAt", "2026-09-16T00:00:00Z"}}}});
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200) << stopped->body;
    ASSERT_EQ(record("sessions/provider-session").at("status"), "interrupted");
    ASSERT_EQ(record("sessions/provider-session").at("claimStatus"), "released");
  }
  if (phase != "new") {
    ASSERT_EQ(post(path + operation, control(operation))->status, 202);
    if (phase != "pending") {
      ASSERT_EQ(post(path + "ack", {{"schema", "hi/fleet/v1"},
                                    {"workerId", "provider-worker"},
                                    {"generation", 1},
                                    {"commandId", "provider-" + operation},
                                    {"eventId", "provider-received"},
                                    {"status", phase}})
                    ->status,
                200);
    }
    ASSERT_EQ(record("commands/provider-" + operation).at("status"), phase);
  }
  restart_with_historical_overlap();
  ASSERT_FALSE(HasFatalFailure());
  const auto before = checkpoint();
  const auto target = record("sessions/provider-session");
  const auto provider_task = hmas_task_to_json(*store.get_hmas_task(task.id));
  const auto response = post(path + operation, control(operation));
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, phase == "new" || phase == "pending" ? 409 : 202) << response->body;
  unchanged(before);
  EXPECT_EQ(record("sessions/provider-session"), target);
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task(task.id)), provider_task);
}

INSTANTIATE_TEST_SUITE_P(AllocationOverlap, FleetBuildProviderActivation,
                         ::testing::Values(ProviderActivationCase{"start", "new"},
                                           ProviderActivationCase{"start", "pending"},
                                           ProviderActivationCase{"start", "accepted"},
                                           ProviderActivationCase{"start", "completed"},
                                           ProviderActivationCase{"resume", "new"},
                                           ProviderActivationCase{"resume", "pending"},
                                           ProviderActivationCase{"resume", "accepted"},
                                           ProviderActivationCase{"resume", "completed"}));

TEST_F(FleetConfiguredBuildRoutes, FencingDeliveryRetryRequiresTheCurrentParentAndSameCommand) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  publisher.unavailable = true;
  ASSERT_EQ(post("build-jobs/submit", submission())->status, 503);
  publisher.unavailable = false;
  const auto original = publisher.calls.back().payload;
  const auto command = json::parse(original);
  const auto path = "build-jobs/" + command.at("targetId").get<std::string>();
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  ASSERT_EQ(post("build-jobs/submit", submission())->status, 202);
  EXPECT_EQ(publisher.calls.size(), calls_before);
  const json delivery = {{"schema", "hi/fleet/build-delivery/v1"},
                         {"commandId", command.at("commandId")},
                         {"generation", 1},
                         {"attempt", 1}};
  const auto retried = post(path + "/deliver", delivery);
  ASSERT_EQ(retried->status, 202) << retried->body;
  ASSERT_EQ(publisher.calls.size(), calls_before + 1);
  EXPECT_EQ(publisher.calls.back().payload, original);
  EXPECT_EQ(github->created_issues, before);
  auto wrong = delivery;
  wrong["commandId"] = "replacement-command";
  EXPECT_EQ(post(path + "/deliver", wrong)->status, 409);
  ASSERT_EQ(post("sessions/parent-session/cancel", {{"commandId", "parent-stop"},
                                                    {"idempotencyKey", "parent-stop"},
                                                    {"generation", 1},
                                                    {"payload", json::object()}})
                ->status,
            202);
  const auto stopped_calls = publisher.calls.size();
  EXPECT_EQ(post(path + "/deliver", delivery)->status, 409);
  EXPECT_EQ(publisher.calls.size(), stopped_calls);
}

TEST_F(FleetConfiguredBuildRoutes, FencingToolWorkerCannotBeRegisteredAsAnIssueAgent) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_EQ(post("build-jobs/submit", submission())->status, 202);
  ASSERT_EQ(post("pools", {{"id", "another-pool"}, {"capacity", 1}})->status, 201);
  const auto before = github->created_issues;
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  EXPECT_EQ(post("workers", {{"id", "tool-worker-1"},
                             {"poolId", "another-pool"},
                             {"capacity", 1},
                             {"host", "other-host"}})
                ->status,
            409);
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
}

TEST_F(FleetConfiguredBuildRoutes, FencingChangedActiveRecipeCannotBorrowANewAllocation) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_EQ(post("build-jobs/submit", submission())->status, 202);
  auto catalog = build_catalog();
  catalog["recipes"][0]["recipeDigest"] = std::string(64, '6');
  catalog["allocations"][0]["id"] = "another-allocation";
  catalog["allocations"][0]["workerId"] = "another-tool-worker";
  auto authorities = build_authorities();
  authorities["authorities"][0]["allocationId"] = "another-allocation";
  authorities["authorities"][0]["workerId"] = "another-tool-worker";
  TearDown();
  start_service(catalog, authorities);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_EQ(
      post("events",
           {{"schema", "hi/fleet/v1"},
            {"eventId", "parent-fresh"},
            {"workerId", "laptop-worker"},
            {"targetKind", "sessions"},
            {"targetId", "parent-session"},
            {"generation", 1},
            {"sourceSequence", 2},
            {"kind", "activity"},
            {"event", {{"activity", "model_working"}, {"observedAt", "2026-09-13T01:00:00Z"}}}})
          ->status,
      200);
  auto request = submission();
  request["idempotencyKey"] = "second-identity";
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  EXPECT_EQ(post("build-jobs/submit", request)->status, 409);
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
}

TEST_F(FleetConfiguredBuildRoutes, ReviewExistingGenericCommandIdentityPreventsBuildAdmission) {
  // Actual producer identity for this fixture's registered workspace/key.
  const std::string command =
      "build-311e9723b2a693dbc7a1875a3514896242c6a7bc34716c8d9cccf420772c4189-start";
  active_parent(command);
  ASSERT_FALSE(HasFatalFailure());
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  const auto command_before = client->Get("/v1/fleet/commands/" + command)->body;
  ASSERT_EQ(json::parse(command_before).at("record").at("id"), "parent-session");
  EXPECT_EQ(post("build-jobs/submit", submission())->status, 409);
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
  EXPECT_EQ(client->Get("/v1/fleet/commands/" + command)->body, command_before);
}

TEST_F(FleetConfiguredBuildRoutes, ReviewRecoveryAuthorityRejectsFloatingGeneration) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto path =
      "build-jobs/" + admitted.at("record").at("id").get<std::string>() + "/claim-run";
  const auto claim = run_claim(admitted);
  const auto original = supervisor_post(path, claim);
  ASSERT_EQ(original->status, 200) << original->body;
  auto malformed = build_authorities();
  malformed["authorities"][0]["generation"] = 1.0;
  TearDown();
  start_service(json::object(), malformed);
  ASSERT_FALSE(HasFatalFailure());
  const auto before = github->created_issues;
  EXPECT_EQ(supervisor_post(path, claim)->status, 403);
  EXPECT_EQ(github->created_issues, before);
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  const auto recovered = supervisor_post(path, claim);
  ASSERT_EQ(recovered->status, 200) << recovered->body;
  EXPECT_EQ(json::parse(recovered->body), json::parse(original->body));
  EXPECT_EQ(github->created_issues, before);
}

TEST_F(FleetConfiguredBuildRoutes, HydrationRejectsCorruptedTypedAuthorityBeforeReplaying) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id;
  const auto claim = run_claim(admitted);
  const auto granted = supervisor_post(path + "/claim-run", claim);
  ASSERT_EQ(granted->status, 200) << granted->body;
  const auto original_backing = github->created_issues;
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  std::string issue_number;
  json original;
  for (const auto& [number, issue] : original_backing) {
    if (issue.at("label") != "agamemnon-fleet") continue;
    const auto body = issue.at("body").get<std::string>();
    const auto begin = body.find("```json\n");
    const auto end = body.find("\n```", begin + 8);
    const auto document = json::parse(body.substr(begin + 8, end - begin - 8));
    if (document.at("kind") == "build-jobs" && document.at("record").at("id") == id) {
      issue_number = number;
      original = document;
    }
  }
  ASSERT_FALSE(issue_number.empty());
  const std::vector<std::pair<std::string, json>> corruptions = {
      {"/kind", "executions"},
      {"/kind", "unknown-resource-kind"},
      {"/record/schema", "unknown"},
      {"/record/kind", "executions"},
      {"/record/build/schema", "unknown"},
      {"/record/build/request/snapshot/members", 2.0},
      {"/record/generation", 1.0},
      {"/record/build/allocation/generation", 1.0},
      {"/record/build/allocation/workerId", "other-worker"},
      {"/record/build/policyDigest", std::string(64, '9')},
      {"/record/build/parametersDigest", std::string(64, '9')},
      {"/record/build/policy/recipe/recipeDigest", std::string(64, '9')},
      {"/record/parent/claim/agentId", "other-agent"},
      {"/record/parent/executionId", "other-execution"},
      {"/record/build/reservation", "released"},
      {"/record/claimStatus", "claimed"},
      {"/record/collectionVerified", true},
      {"/record/build/grant/claim/generation", 1.0},
      {"/record/build/grant/grantId", std::string(64, '9')},
      {"/record/build/grant/schema", "unknown"},
      {"/commands/0/command/payload/snapshot/manifestDigest", std::string(64, '9')},
      {"/commands/0/command/generation", 2},
      {"/events/0/seq", 1.5},
      {"/events/0/targetId", "other-build"},
      {"/record/workerId", "tool-worker-1"},
      {"/record/build", nullptr}};
  for (const auto& [pointer, value] : corruptions) {
    SCOPED_TRACE(pointer + " = " + value.dump());
    auto document = original;
    if (value.is_null())
      document["record"].erase("build");
    else
      document[json::json_pointer(pointer)] = value;
    // A retained typed command cannot become a legacy resource by dropping its
    // build metadata and adding a generic worker field.
    if (pointer == "/record/workerId") document["record"].erase("build");
    TearDown();
    github->created_issues = original_backing;
    github->created_issues.at(issue_number)["body"] =
        "## AgamemnonEntity: fleet\n\n```json\n" + document.dump() + "\n```\n";
    const auto corrupted_backing = github->created_issues;
    const auto calls_before = publisher.calls.size();
    start_service(json::object(), build_authorities());
    ASSERT_FALSE(HasFatalFailure());
    const auto listed = client->Get("/v1/fleet/build-jobs");
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->status, 503) << listed->body;
    const auto replay = supervisor_post(path + "/claim-run", claim);
    ASSERT_TRUE(replay);
    EXPECT_EQ(replay->status, 503) << replay->body;
    EXPECT_EQ(github->created_issues, corrupted_backing);
    EXPECT_EQ(publisher.calls.size(), calls_before);
    EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
  }
  TearDown();
  github->created_issues = original_backing;
  start_service(json::object(), build_authorities());
  ASSERT_FALSE(HasFatalFailure());
  const auto valid = supervisor_post(path + "/claim-run", claim);
  ASSERT_EQ(valid->status, 200) << valid->body;
  EXPECT_EQ(json::parse(valid->body), json::parse(granted->body));
  EXPECT_EQ(github->created_issues, original_backing);
}

TEST_F(FleetConfiguredBuildRoutes, ConcurrentParentStopAndGrantHaveOneDurableOrder) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto submitted = post("build-jobs/submit", submission());
  ASSERT_EQ(submitted->status, 202) << submitted->body;
  const auto admission = json::parse(submitted->body);
  const auto id = admission.at("record").at("id").get<std::string>();
  const auto claim = run_claim(admission);
  const auto parent_claim = store.get_hmas_task("real-parent-task")->fleet_claim;
  const auto calls_before = publisher.calls.size();
  const json stop = {{"commandId", "concurrent-parent-stop"},
                     {"idempotencyKey", "concurrent-parent-stop"},
                     {"generation", 1},
                     {"payload", json::object()}};
  const std::array<std::string, 2> paths = {"/v1/fleet/build-jobs/" + id + "/claim-run",
                                            "/v1/fleet/sessions/parent-session/cancel"};
  const std::array<json, 2> requests = {claim, stop};
  std::array<int, 2> statuses{};
  std::array<std::string, 2> bodies;
  std::array<std::promise<void>, 2> arrivals;
  std::array<std::future<void>, 2> ready = {arrivals[0].get_future(), arrivals[1].get_future()};
  std::promise<void> release;
  const auto start = release.get_future().share();
  const auto call = [&](std::size_t index) {
    httplib::Client peer("127.0.0.1", client->port());
    peer.set_connection_timeout(2);
    peer.set_read_timeout(2);
    peer.set_write_timeout(2);
    peer.set_default_headers({{"Authorization", "Bearer build-requester-fixture"},
                              {"X-Fleet-Build-Key", "supervisor-only-fixture"}});
    arrivals[index].set_value();
    if (start.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return;
    const auto response = peer.Post(paths[index], requests[index].dump(), "application/json");
    if (response) {
      statuses[index] = response->status;
      bodies[index] = response->body;
    }
  };
  std::jthread granting(call, 0);
  std::jthread stopping(call, 1);
  const bool grant_ready = ready[0].wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  const bool stop_ready = ready[1].wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release.set_value();
  granting.join();
  stopping.join();
  ASSERT_TRUE(grant_ready && stop_ready);
  ASSERT_EQ(statuses[1], 202) << bodies[1];
  ASSERT_TRUE(statuses[0] == 200 || statuses[0] == 409) << bodies[0];
  std::cout << "Concurrent grant HTTP status: " << statuses[0] << '\n';
  const auto record = json::parse(client->Get("/v1/fleet/build-jobs/" + id)->body);
  const auto before_replay = github->created_issues;
  const auto replay = supervisor_post("build-jobs/" + id + "/claim-run", claim);
  ASSERT_TRUE(replay);
  EXPECT_EQ(replay->status, statuses[0]);
  if (statuses[0] == 200) {
    EXPECT_EQ(record.at("build").at("grant").at("claim"), claim);
    EXPECT_EQ(json::parse(replay->body), json::parse(bodies[0]));
  } else {
    EXPECT_FALSE(record.at("build").contains("grant"));
  }
  EXPECT_EQ(github->created_issues, before_replay);
  EXPECT_EQ(publisher.calls.size(), calls_before + 1);
  EXPECT_EQ(record.at("build").at("reservation"), "reserved");
  EXPECT_EQ(store.get_hmas_task("real-parent-task")->fleet_claim, parent_claim);
}

TEST_F(FleetConfiguredBuildRoutes, LogPollingRequiresExplicitBackendWithoutDurableWrites) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto response = post("build-jobs/submit", submission());
  ASSERT_EQ(response->status, 202) << response->body;
  const auto id = json::parse(response->body).at("record").at("id").get<std::string>();
  const auto path = "/v1/fleet/build-jobs/" + id + "/logs";
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  EXPECT_EQ(client->Get(path)->status, 503);
  EXPECT_EQ(client->Get(path + "?stream=stderr&after=0&limit=1024")->status, 503);
  for (const auto& query : {"stream=other", "after=-1", "after=1.0", "after=9223372036854775808",
                            "limit=0", "limit=65537", "after=0&after=1", "url=http://elsewhere"}) {
    SCOPED_TRACE(query);
    EXPECT_EQ(client->Get(path + "?" + query)->status, 400);
  }
  EXPECT_EQ(client->Get("/v1/fleet/build-jobs/missing/logs")->status, 404);
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
}

class FleetLargeBuildRoutes : public FleetConfiguredBuildRoutes {
 protected:
  // These are opaque synthetic identity bytes, never opened as a filesystem
  // path. JSON escaping stresses the actual serialized persistence budget.
  std::string parent_workspace() override { return "/" + std::string(900, '\x01'); }

  json build_catalog() override {
    auto catalog = FleetConfiguredBuildRoutes::build_catalog();
    catalog["workspaces"][0]["parentWorkspace"] = parent_workspace();
    return catalog;
  }

  std::size_t build_body_size() {
    for (const auto& [number, issue] : github->created_issues)
      if (issue.at("title").get<std::string>().starts_with("fleet: build-jobs/"))
        return issue.at("body").get<std::string>().size();
    return 0;
  }
};

class FleetAdmissionLimitBuildRoutes : public FleetLargeBuildRoutes {
 protected:
  std::string parent_workspace() override { return "/" + std::string(1000, '\x01'); }
};

TEST_F(FleetAdmissionLimitBuildRoutes, ExpandedIntentRefusesAdmissionBeforeWriteOrDispatch) {
  // The raw identity stays within the supported 1024-byte limit. Its repeated
  // JSON-escaped copies exceed the 30,000-byte admission budget; the 900-byte
  // fixture below exercises successful admission and later cleanup.
  ASSERT_LE(parent_workspace().size(), 1024);
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  auto request = submission();
  request["idempotencyKey"] = std::string(128, 'i');
  request["snapshot"]["reference"] = std::string(128, 's');
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto parent_before = client->Get("/v1/fleet/sessions/parent-session")->body;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto response = post("build-jobs/submit", request);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 507) << response->body;
    EXPECT_EQ(github->created_issues, before);
    EXPECT_EQ(publisher.calls.size(), calls_before);
    EXPECT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
    EXPECT_EQ(client->Get("/v1/fleet/sessions/parent-session")->body, parent_before);
    const auto listed = client->Get("/v1/fleet/build-jobs");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->status, 200) << listed->body;
    EXPECT_TRUE(json::parse(listed->body).at("items").empty());
  }
}

TEST_F(FleetLargeBuildRoutes, SerializedAdmissionRetainsCancelAndCleanupAfterEvidenceRefusal) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  auto request = submission();
  request["idempotencyKey"] = std::string(128, 'i');
  request["snapshot"]["reference"] = std::string(128, 's');
  const auto response = post("build-jobs/submit", request);
  ASSERT_EQ(response->status, 202) << response->body;
  const auto admitted = json::parse(response->body);
  const auto id = admitted.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id;
  const auto initial_bytes = build_body_size();
  std::cout << "Large admitted body bytes: " << initial_bytes << '\n';
  EXPECT_GT(initial_bytes, 25000);
  EXPECT_LT(initial_bytes, 30500);
  const auto claim = run_claim(admitted);
  ASSERT_EQ(supervisor_post(path + "/claim-run", claim)->status, 200);
  const auto before = github->created_issues;
  auto oversized = terminal_fact(admitted);
  oversized["receipt"]["reference"] = std::string(20000, 'r');
  EXPECT_EQ(supervisor_post(path + "/facts", oversized)->status, 413);
  oversized["receipt"]["reference"] = std::string(129, 'r');
  EXPECT_EQ(supervisor_post(path + "/facts", oversized)->status, 400);
  EXPECT_EQ(github->created_issues, before);
  const auto claim_before = store.get_hmas_task("real-parent-task")->fleet_claim;
  ASSERT_EQ(post(path + "/cancel", build_cancel())->status, 202);
  const auto stopped = supervisor_post(path + "/facts", terminal_fact(admitted, true));
  ASSERT_EQ(stopped->status, 200) << stopped->body;
  const auto record = json::parse(stopped->body).at("record");
  EXPECT_EQ(record.at("status"), "cancelled");
  EXPECT_EQ(record.at("build").at("reservation"), "released");
  EXPECT_EQ(record.at("build").at("evidenceState"), "incomplete");
  EXPECT_FALSE(record.at("collectionVerified").get<bool>());
  EXPECT_EQ(store.get_hmas_task("real-parent-task")->fleet_claim, claim_before);
  const auto terminal_bytes = build_body_size();
  std::cout << "Large terminal body bytes: " << terminal_bytes << '\n';
  EXPECT_LT(terminal_bytes, 60000);
  const auto terminal_backing = github->created_issues;
  restart_service(false);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(supervisor_post(path + "/facts", terminal_fact(admitted, true))->status, 200);
  EXPECT_EQ(github->created_issues, terminal_backing);
}

class FleetLoggedBuildRoutes : public FleetConfiguredBuildRoutes {
 protected:
  httplib::Server backend;
  std::thread backend_thread;
  int backend_port = 0;
  std::mutex backend_mutex;
  json page;
  std::string raw_body;
  int backend_status = 200;
  int delay_ms = 0;
  std::vector<httplib::Request> requests;
  json config_override;

  json build_artifacts() override {
    if (!config_override.is_null()) return config_override;
    return {{"schema", "hi/fleet/build-artifacts/v1"},
            {"origin", "http://127.0.0.1:" + std::to_string(backend_port)},
            {"key", "private-artifact-fixture"}};
  }

  void SetUp() override {
    backend.Get(R"(/v1/fleet/build-jobs/([A-Za-z0-9_-]+)/logs)",
                [this](const httplib::Request& request, httplib::Response& response) {
                  std::string body;
                  int delay = 0;
                  {
                    std::lock_guard lock(backend_mutex);
                    requests.push_back(request);
                    response.status = backend_status;
                    body = raw_body.empty() ? page.dump() : raw_body;
                    delay = delay_ms;
                  }
                  if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                  response.set_header("Location", "/must-not-follow");
                  response.set_content(body, "application/json");
                });
    backend_port = backend.bind_to_any_port("127.0.0.1");
    ASSERT_GT(backend_port, 0);
    backend_thread = std::thread([this] { backend.listen_after_bind(); });
    backend.wait_until_ready();
    FleetConfiguredBuildRoutes::SetUp();
    client->set_read_timeout(5);
  }

  void TearDown() override {
    FleetConfiguredBuildRoutes::TearDown();
    backend.stop();
    if (backend_thread.joinable()) backend_thread.join();
  }

  json admit() {
    active_parent();
    if (HasFatalFailure()) return nullptr;
    const auto response = post("build-jobs/submit", submission());
    EXPECT_EQ(response->status, 202) << response->body;
    return json::parse(response->body);
  }

  json log_page(const json& admitted) {
    // Empty-byte SHA256 is fixed independently of the implementation.
    return {{"schema", "hi/fleet/build-logs/v1"},
            {"buildId", admitted.at("record").at("id")},
            {"attempt", 1},
            {"snapshotDigest",
             admitted.at("record").at("build").at("request").at("snapshot").at("manifestDigest")},
            {"stream", "stdout"},
            {"after", 0},
            {"next", 0},
            {"data", ""},
            {"chunkDigest", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {"complete", false},
            {"truncated", false},
            {"manifest", nullptr}};
  }

  void respond_with(const json& value, int status = 200, std::string raw = "", int delay = 0) {
    std::lock_guard lock(backend_mutex);
    page = value;
    backend_status = status;
    raw_body = std::move(raw);
    delay_ms = delay;
  }

  std::size_t request_count() {
    std::lock_guard lock(backend_mutex);
    return requests.size();
  }
};

TEST_F(FleetLoggedBuildRoutes, OnlyExplicitCanonicalLoopbackOriginsCanBeConfigured) {
  const auto valid = build_artifacts();
  for (const auto& origin :
       {"https://127.0.0.1:443", "https://artifacts.example:443", "http://localhost:1234",
        "http://192.0.2.1:1234", "http://127.0.0.2:1234", "http://127.0.0.1", "http://127.0.0.1:0",
        "http://127.0.0.1:0123", "http://127.0.0.1:65536", "http://127.0.0.1:1234/",
        "http://127.0.0.1:1234/path", "http://127.0.0.1:1234?token=x",
        "http://127.0.0.1:1234#fragment", "http://user@127.0.0.1:1234", "http://2130706433:1234",
        "http://[::2]:1234", "http://[::1]", "file:///tmp/logs"}) {
    SCOPED_TRACE(origin);
    config_override = valid;
    config_override["origin"] = origin;
    EXPECT_THROW((void)configured_service(build_catalog(), build_authorities()), FleetError);
  }
  for (const auto& key : {std::string(""), std::string("has space"),
                          std::string("header\r\ninjection"), std::string(4097, 'k')}) {
    config_override = valid;
    config_override["key"] = key;
    EXPECT_THROW((void)configured_service(build_catalog(), build_authorities()), FleetError);
  }
  config_override = valid;
  config_override["origin"] = "http://[::1]:1234";
  EXPECT_NO_THROW((void)configured_service(build_catalog(), build_authorities()));
  EXPECT_EQ(request_count(), 0);  // Configuration never contacts even the local backend.
}

TEST_F(FleetLoggedBuildRoutes, PrivatePagesRetainByteIdentityAndNeverWriteControlHistory) {
  const auto admitted = admit();
  ASSERT_FALSE(HasFatalFailure());
  auto expected = log_page(admitted);
  expected["data"] = "compiled \u03bb\n";
  expected["next"] = 12;
  expected["chunkDigest"] = "230b790e2bdfd65752368313bd7506888227bfb76068f8cc30d0db1f0b9b4093";
  respond_with(expected);
  const auto path = "/v1/fleet/build-jobs/" + expected.at("buildId").get<std::string>() + "/logs";
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  const auto response = client->Get(path);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200) << response->body;
  if (response->status == 200) EXPECT_EQ(json::parse(response->body), expected);
  ASSERT_EQ(request_count(), 1);
  {
    std::lock_guard lock(backend_mutex);
    const auto& request = requests.front();
    EXPECT_EQ(request.get_header_value("Authorization"), "Bearer private-artifact-fixture");
    EXPECT_FALSE(request.has_header("X-Fleet-Build-Key"));
    EXPECT_EQ(request.get_param_value("attempt"), "1");
    EXPECT_EQ(request.get_param_value("snapshotDigest"),
              expected.at("snapshotDigest").get<std::string>());
    EXPECT_EQ(request.get_param_value("stream"), "stdout");
    EXPECT_EQ(request.get_param_value("after"), "0");
    EXPECT_EQ(request.get_param_value("limit"), "65536");
  }
  const auto id = expected.at("buildId").get<std::string>();
  ASSERT_EQ(post("build-jobs/" + id + "/cancel", build_cancel())->status, 202);
  auto fact = terminal_fact(admitted, true);
  const json manifest = {{"reference", "private-logs-1"}, {"digest", std::string(64, '8')}};
  fact["logs"] = manifest;
  ASSERT_EQ(supervisor_post("build-jobs/" + id + "/facts", fact)->status, 200);
  const auto terminal_before = github->created_issues;
  const auto terminal_calls = publisher.calls.size();
  expected = log_page(admitted);
  expected["after"] = 12;
  expected["next"] = 12;
  expected["stream"] = "stderr";
  expected["complete"] = true;
  expected["manifest"] = manifest;
  respond_with(expected);
  FleetBuildRoutes::TearDown();
  start_service(json::object(), build_authorities());
  ASSERT_FALSE(HasFatalFailure());
  const auto complete = client->Get(path + "?stream=stderr&after=12&limit=1");
  ASSERT_EQ(complete->status, 200) << complete->body;
  EXPECT_EQ(json::parse(complete->body), expected);
  EXPECT_EQ(github->created_issues, terminal_before);
  EXPECT_EQ(publisher.calls.size(), terminal_calls);
  expected["manifest"]["digest"] = std::string(64, '9');
  respond_with(expected);
  EXPECT_EQ(client->Get(path + "?stream=stderr&after=12&limit=1")->status, 503);
  EXPECT_NE(before, terminal_before);  // Only explicit cancel/fact above write control state.
  EXPECT_EQ(calls_before + 1, terminal_calls);
}

TEST_F(FleetLoggedBuildRoutes, WrongOrOversizedPrivatePagesAreRejectedAfterRealHttp) {
  const auto admitted = admit();
  ASSERT_FALSE(HasFatalFailure());
  const auto valid = log_page(admitted);
  const auto path = "/v1/fleet/build-jobs/" + valid.at("buildId").get<std::string>() + "/logs";
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  const std::vector<std::pair<std::string, json>> mutations = {
      {"/schema", "other"},
      {"/buildId", "other"},
      {"/attempt", 1.0},
      {"/snapshotDigest", std::string(64, '0')},
      {"/stream", "stderr"},
      {"/after", 0.0},
      {"/next", 1},
      {"/next", -1},
      {"/data", false},
      {"/chunkDigest", std::string(64, '0')},
      {"/complete", 1},
      {"/complete", true},
      {"/truncated", "false"},
      {"/manifest", {{"reference", "../elsewhere"}, {"digest", std::string(64, '8')}}},
      {"/extra", "unregistered"},
      {"/data", std::string(65537, 'x')}};
  for (const auto& [pointer, value] : mutations) {
    SCOPED_TRACE(pointer);
    auto bad = valid;
    bad[json::json_pointer(pointer)] = value;
    respond_with(bad);
    const auto count = request_count();
    EXPECT_EQ(client->Get(path)->status, 503);
    EXPECT_EQ(request_count(), count + 1);
  }
  for (const auto& raw :
       {std::string("{"), valid.dump().substr(0, valid.dump().size() - 1) + ",\"attempt\":1}",
        std::string(410000, 'x')}) {
    respond_with(valid, 200, raw);
    const auto count = request_count();
    EXPECT_EQ(client->Get(path)->status, 503);
    EXPECT_EQ(request_count(), count + 1);
  }
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
}

TEST_F(FleetLoggedBuildRoutes, BackendStatusRedirectAndDeadlineKeepReadOnlyFailureSemantics) {
  const auto admitted = admit();
  ASSERT_FALSE(HasFatalFailure());
  const auto valid = log_page(admitted);
  const auto path = "/v1/fleet/build-jobs/" + valid.at("buildId").get<std::string>() + "/logs";
  const auto before = github->created_issues;
  const auto calls_before = publisher.calls.size();
  for (const auto& [status, expected] :
       std::vector<std::pair<int, int>>{{409, 409}, {302, 503}, {401, 503}, {500, 503}}) {
    respond_with(valid, status);
    const auto count = request_count();
    EXPECT_EQ(client->Get(path)->status, expected);
    EXPECT_EQ(request_count(), count + 1);
  }
  respond_with(valid, 200, "", 1300);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(client->Get(path)->status, 503);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(publisher.calls.size(), calls_before);
}

// Fixed harmless source bytes for the separate consumer fixture.
constexpr const char* export_justfile = "test-unit:\n  @printf 'fixture build\\n'\n";
constexpr const char* export_lock = "version = 1\nrevision = 3\nrequires-python = \">=3.13\"\n";

class FleetBuildExportRoutes : public FleetConfiguredBuildRoutes {
 protected:
  std::optional<json> snapshot_input;

  json snapshot() {
    if (!snapshot_input) {
      snapshot_input = FleetBuildRoutes::submission().at("snapshot");
      if (const char* path = std::getenv("FLEET_BUILD_SNAPSHOT_INPUT")) {
        std::ifstream input(path, std::ios::binary);
        std::array<char, 4097> bytes{};
        input.read(bytes.data(), bytes.size());
        if (!input.eof() || input.gcount() <= 0 || input.gcount() > 4096)
          throw std::runtime_error("bounded snapshot fixture input is unreadable");
        snapshot_input = json::parse(bytes.data(), bytes.data() + input.gcount());
      }
    }
    return *snapshot_input;
  }

  json submission() {
    auto request = FleetBuildRoutes::submission();
    request["snapshot"] = snapshot();
    return request;
  }

  json build_catalog() override {
    auto catalog = FleetConfiguredBuildRoutes::build_catalog();
    catalog["workspaces"][0]["snapshotPolicyDigest"] = snapshot().at("policyDigest");
    catalog["recipes"][0]["recipeDigest"] =
        "4bf01456f923a0387eb9c5b4edb8a81faf8310a088ede8f43fea95e337ccc611";
    catalog["recipes"][0]["lockDigest"] =
        "96599229fba386c9268987794da072036c3f16443f7b828004fc56c8d9b00b27";
    return catalog;
  }
};

TEST_F(FleetBuildExportRoutes, ExportBuildContract) {
  active_parent();
  ASSERT_FALSE(HasFatalFailure());
  const auto task_before = hmas_task_to_json(*store.get_hmas_task("real-parent-task"));
  const auto parent_before = client->Get("/v1/fleet/sessions/parent-session")->body;
  const auto request = submission();
  const auto submitted = post("build-jobs/submit", request);
  ASSERT_TRUE(submitted);
  ASSERT_EQ(submitted->status, 202) << submitted->body;
  const auto admission = json::parse(submitted->body);
  const auto id = admission.at("record").at("id").get<std::string>();
  const auto path = "build-jobs/" + id;
  const auto claim = run_claim(admission);
  const auto claimed = supervisor_post(path + "/claim-run", claim);
  ASSERT_TRUE(claimed);
  ASSERT_EQ(claimed->status, 200) << claimed->body;
  const auto granted = json::parse(claimed->body);
  json persisted_grant;
  for (const auto& [number, issue] : github->created_issues) {
    if (issue.at("label") != "agamemnon-fleet") continue;
    const auto body = issue.at("body").get<std::string>();
    const auto begin = body.find("```json\n");
    ASSERT_NE(begin, std::string::npos);
    const auto end = body.find("\n```", begin + 8);
    ASSERT_NE(end, std::string::npos);
    const auto document = json::parse(body.substr(begin + 8, end - begin - 8));
    if (document.at("kind") == "build-jobs" && document.at("record").at("id") == id) {
      ASSERT_TRUE(persisted_grant.is_null());
      persisted_grant = document;
    }
  }
  ASSERT_FALSE(persisted_grant.is_null());
  ASSERT_EQ(persisted_grant.at("record").at("build").at("grant"), granted.at("grant"));
  ASSERT_EQ(persisted_grant.at("commands").at(0).at("command"), granted.at("command"));
  const auto cancellation = build_cancel();
  const auto cancelled = post(path + "/cancel", cancellation);
  ASSERT_TRUE(cancelled);
  ASSERT_EQ(cancelled->status, 202) << cancelled->body;
  const auto stop = json::parse(cancelled->body);
  const auto fact = terminal_fact(admission, true);
  const auto accepted = supervisor_post(path + "/facts", fact);
  ASSERT_TRUE(accepted);
  ASSERT_EQ(accepted->status, 200) << accepted->body;
  const auto terminal = json::parse(accepted->body);
  ASSERT_EQ(terminal.at("record").at("build").at("reservation"), "released");
  ASSERT_FALSE(terminal.at("record").at("collectionVerified").get<bool>());
  ASSERT_EQ(hmas_task_to_json(*store.get_hmas_task("real-parent-task")), task_before);
  ASSERT_EQ(client->Get("/v1/fleet/sessions/parent-session")->body, parent_before);
  json commands = json::array();
  for (const auto& publication : publisher.calls) {
    const auto command = json::parse(publication.payload);
    if (command.value("targetKind", "") == "build-jobs")
      commands.push_back({{"subject", publication.subject}, {"command", command}});
  }
  ASSERT_EQ(commands.size(), 2u);
  ASSERT_EQ(commands.at(0).at("command"), admission.at("command"));
  ASSERT_EQ(commands.at(1).at("command"), stop.at("command"));
  // Optional test-runner output. Synthetic operator/fact inputs are not a
  // qualified allocation, supervisor run, snapshot export or verified receipt.
  if (const char* output_path = std::getenv("FLEET_BUILD_CONTRACT_OUTPUT")) {
    const auto& build = admission.at("record").at("build");
    const json grant_input = {{"buildId", id}, {"claim", claim}};
    const json exported = {
        {"schema", "hi/fleet/build-contract/v1"},
        {"fixture", "synthetic-controller-boundaries"},
        {"sourceFiles", {{"justfile", export_justfile}, {"uv.lock", export_lock}}},
        {"submission", request},
        {"admission", admission},
        {"claimRequest", claim},
        {"grantResponse", granted},
        {"persistedGrantDocument", persisted_grant},
        {"cancelRequest", cancellation},
        {"cancelResponse", stop},
        {"terminalFact", fact},
        {"terminalResponse", terminal},
        {"commands", commands},
        {"digestInputs",
         {{"policy", build.at("policy").dump()},
          {"parameters", request.at("parameters").dump()},
          {"grant", grant_input.dump()}}}};
    std::ofstream output(output_path);
    ASSERT_TRUE(output.good());
    output << exported.dump(2) << '\n';
    output.flush();
    ASSERT_TRUE(output.good());
  }
}

}  // namespace
}  // namespace agamemnon::test
