#include "agamemnon/auth.hpp"
#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/fleet.hpp"
#include "agamemnon/fleet_issue.hpp"
#include "agamemnon/github_client.hpp"
#include "agamemnon/metrics.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/rate_limiter.hpp"
#include "agamemnon/routes.hpp"
#include "agamemnon/store.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "httplib.h"
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

namespace agamemnon::test {
namespace {
const std::string plan_body = "Implement the selected plan.\n";
const std::string plan_digest = "70a0e27a0bf3ca22a0f113df2b8911379ca0f5cc84f10c7fc8c2e71bbfa98878";
const std::string direct_task =
    "issue-2be92942689bad6ddb5bf5b8a8d930926496abcc969a908daa2fada5bb5ab481";

std::shared_ptr<IssueImportConfiguration> configuration() {
  auto result = std::make_shared<IssueImportConfiguration>();
  result->state_branch = "import-state";
  result->repositories = json::array({{{"key", "implementation"},
                                       {"repository", "Example/Project"},
                                       {"repositoryId", "R_fixture"}}});
  return result;
}

class RetainedIssueGitHub : public MockGitHubClient {
 public:
  int lookups = 0;
  int fence_writes = 0;
  int creates = 0;
  bool lose_create_response = false;
  bool lose_creating_ack = false;
  bool lose_prepared_ack = false;
  bool reject_prepared = false;
  bool reject_creating = false;
  std::function<void()> before_update;
  json work = {{"__typename", "Issue"}, {"id", "I_fixture"},
               {"number", 42},          {"url", "https://github.com/Example/Project/issues/42"},
               {"state", "OPEN"},       {"title", "Planned implementation"},
               {"body", plan_body},     {"updatedAt", "2026-09-13T01:00:00Z"}};
  json repository = {{"id", "R_fixture"}, {"nameWithOwner", "Example/Project"}};
  json comment = {{"__typename", "IssueComment"},
                  {"id", "IC_fixture"},
                  {"body", plan_body},
                  {"updatedAt", "2026-09-13T01:00:00Z"},
                  {"issue", {{"id", "I_fixture"}}}};
  std::map<std::string, ImportFence> fences;

  json import_work_issue(const std::string& owner, const std::string& name, int number,
                         ImportContext& context) override {
    context.checkpoint();
    ++lookups;
    EXPECT_EQ(owner + "/" + name, "Example/Project");
    EXPECT_EQ(number, 42);
    auto result = repository;
    result["issue"] = work;
    return {{"repository", result}};
  }
  json import_plan_comment(const std::string& id, ImportContext& context) override {
    context.checkpoint();
    ++lookups;
    EXPECT_EQ(id, "IC_fixture");
    return {{"node", comment}};
  }
  std::vector<json> list_issues_including_closed(std::string_view label) override {
    std::vector<json> result;
    for (const auto& [number, record] : created_issues)
      if (record["label"] == label)
        result.push_back(
            {{"number", std::stoi(number)}, {"state", "open"}, {"body", record["body"]}});
    return result;
  }
  std::vector<json> import_list_issues(ImportContext& context) override {
    context.checkpoint();
    return list_issues_including_closed("agamemnon-hmas-task");
  }
  std::optional<ImportFence> import_read_fence(const std::string& branch, const std::string& key,
                                               ImportContext& context) override {
    context.checkpoint();
    EXPECT_EQ(branch, "import-state");
    if (fences.contains(key)) return fences.at(key);
    return std::nullopt;
  }
  ImportFence import_write_fence(const std::string& branch, const std::string& key,
                                 const json& document, const std::optional<std::string>& sha,
                                 ImportContext& context) override {
    context.checkpoint();
    EXPECT_EQ(branch, "import-state");
    if ((sha && (!fences.contains(key) || fences.at(key).sha != *sha)) ||
        (!sha && fences.contains(key)))
      throw std::runtime_error("conditional conflict");
    if (reject_prepared && document.at("phase") == "prepared")
      throw std::runtime_error("confirmed no dispatch before intent write");
    if (reject_creating && document.at("phase") == "creating")
      throw std::runtime_error("confirmed no dispatch before grant write");
    const auto result =
        ImportFence{std::string(39, 'a') + std::to_string(++fence_writes), document};
    fences[key] = result;
    if (lose_prepared_ack && document.at("phase") == "prepared")
      throw std::runtime_error("lost prepared response");
    if (lose_creating_ack && document.at("phase") == "creating")
      throw std::runtime_error("lost conditional response");
    return result;
  }
  std::string import_create_issue(std::string_view title, std::string_view body,
                                  ImportContext& context) override {
    context.checkpoint();
    ++creates;
    if (lose_create_response) throw std::runtime_error("possible dispatched create");
    return MockGitHubClient::create_issue(title, body, "agamemnon-hmas-task");
  }
  void update_issue_body(std::string_view number, std::string_view body) override {
    if (before_update) before_update();
    MockGitHubClient::update_issue_body(number, body);
  }
};

HmasTask ordinary_task(const std::string& id) {
  HmasTask result{};
  result.id = id;
  result.layer = HmasLayer::L3_TaskAgent;
  result.state = TaskState::Pending;
  result.subject = "Controlled legacy leaf";
  return result;
}
}  // namespace

class FleetIssueRoutes : public ::testing::Test {
 protected:
  std::shared_ptr<RetainedIssueGitHub> github = std::make_shared<RetainedIssueGitHub>();
  std::shared_ptr<IssueImportConfiguration> config = configuration();
  Store store{github, config};
  FakeNatsPublisher publisher;
  AuthMiddleware auth{"fixture-issue-key"};
  RateLimiter limiter{10000, 10000};
  MetricsRegistry metrics;
  Orchestrator orchestrator{store, publisher};
  httplib::Server server;
  std::thread listener;
  std::unique_ptr<httplib::Client> client;
  virtual std::shared_ptr<FleetIssueService> issue_service() { return nullptr; }

  void SetUp() override {
    register_routes(server, store, publisher, limiter, auth, metrics, orchestrator, nullptr,
                    nullptr, issue_service());
    const auto port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    listener = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
    client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2);
    client->set_read_timeout(2);
    client->set_write_timeout(2);
    client->set_default_headers({{"Authorization", "Bearer fixture-issue-key"}});
  }

  void TearDown() override {
    server.stop();
    if (listener.joinable()) listener.join();
  }

  std::string request() {
    return json{{"schema", "hi/agamemnon/issue-import/v1"},
                {"repositoryKey", "implementation"},
                {"issueNumber", 42},
                {"repositoryId", "R_fixture"},
                {"issueId", "I_fixture"},
                {"plan", {{"kind", "issue_body"}, {"digest", plan_digest}}}}
        .dump();
  }

  void no_effects() {
    EXPECT_TRUE(github->calls.empty());
    EXPECT_TRUE(publisher.calls.empty());
    EXPECT_TRUE(github->created_issues.empty());
  }
};

class FleetIssueConfigured : public FleetIssueRoutes {
 protected:
  std::shared_ptr<FleetIssueService> issue_service() override {
    return std::make_shared<FleetIssueService>(store, config, auth);
  }
  void check_busy_store_deadline(bool fail_rendezvous = false, bool throw_before_unblock = false);
};

TEST(FleetIssueConfiguration, RejectsMissingAuthorityAndMalformedClosedRegistry) {
  auto github = std::make_shared<RetainedIssueGitHub>();
  AuthMiddleware auth{"fixture-issue-key"};
  AuthMiddleware absent_auth{""};
  const auto valid = configuration();
  Store store{github, valid};
  Store memory;
  EXPECT_THROW(FleetIssueService(memory, valid, auth), std::invalid_argument);
  EXPECT_THROW(FleetIssueService(store, valid, absent_auth), std::invalid_argument);
  for (int n = 0; n < 10; ++n) {
    auto bad = configuration();
    if (n == 0) bad->state_branch.clear();
    if (n == 1) bad->repositories = json::array();
    if (n == 2) bad->repositories[0]["url"] = "https://unregistered.invalid";
    if (n == 3) bad->repositories[0]["repository"] = "../traversal";
    if (n == 4) bad->repositories[0]["repositoryId"] = false;
    if (n == 5) bad->repositories[0]["key"] = "../arbitrary";
    if (n == 6) bad->repositories.push_back(bad->repositories[0]);
    if (n == 7) {
      auto alias = bad->repositories[0];
      alias["key"] = "other";
      alias["repositoryId"] = "R_other";
      bad->repositories.push_back(alias);
    }
    if (n == 8) bad->repositories[0]["repository"] = "Example/.";
    if (n == 9) bad->repositories[0]["repository"] = "./Project";
    Store configured{github, bad};
    EXPECT_THROW(FleetIssueService(configured, bad, auth), std::invalid_argument) << n;
  }
  EXPECT_EQ(github->lookups, 0);
  EXPECT_TRUE(github->calls.empty());
}

TEST_F(FleetIssueConfigured, RegistryAndInspectionAreReadOnlyExactProjections) {
  const auto registry = client->Get("/v1/fleet/issue-intakes/repositories");
  ASSERT_TRUE(registry);
  ASSERT_EQ(registry->status, 200) << registry->body;
  EXPECT_EQ(json::parse(registry->body), (json{{"schema", "hi/agamemnon/issue-repositories/v1"},
                                               {"repositories", config->repositories}}));
  EXPECT_EQ(github->lookups, 0);
  const auto inspected = client->Get("/v1/fleet/issue-intakes/implementation/42");
  ASSERT_TRUE(inspected);
  ASSERT_EQ(inspected->status, 200) << inspected->body;
  const auto result = json::parse(inspected->body);
  EXPECT_EQ(result.size(), 9u);
  EXPECT_EQ(result["schema"], "hi/agamemnon/issue-inspection/v1");
  EXPECT_EQ(result["repositoryId"], "R_fixture");
  EXPECT_EQ(result["issueId"], "I_fixture");
  EXPECT_EQ(result["state"], "open");
  EXPECT_EQ(result["plan"], (json{{"kind", "issue_body"}, {"digest", plan_digest}}));
  EXPECT_FALSE(result.contains("body"));
  EXPECT_EQ(github->lookups, 1);
  EXPECT_EQ(github->fence_writes, 0);
  no_effects();
}

TEST_F(FleetIssueConfigured, OnePendingLeafThenReplayPreservesClaimAndCheckpoints) {
  auto response = client->Post("/v1/fleet/issue-intakes", request(), "application/json");
  ASSERT_TRUE(response);
  ASSERT_EQ(response->status, 201) << response->body;
  const auto receipt = json::parse(response->body);
  EXPECT_EQ(receipt.size(), 6u);
  EXPECT_EQ(receipt["schema"], "hi/agamemnon/issue-import-receipt/v1");
  EXPECT_EQ(receipt["taskId"], direct_task);
  EXPECT_EQ(
      receipt["routing"],
      (json{{"domain", "pipeline"}, {"hmasRole", "task-agent"}, {"stage", "implementation"}}));
  auto task = store.get_hmas_task(direct_task).value();
  EXPECT_EQ(task.state, TaskState::Pending);
  EXPECT_EQ(task.layer, HmasLayer::L3_TaskAgent);
  EXPECT_TRUE(task.assigned_lead_id.empty() && task.fleet_claim.is_null() &&
              task.parent_task_id.empty());
  EXPECT_EQ(task.delivery["issueIntake"], receipt["provenance"]);
  EXPECT_FALSE(task.delivery.contains("researchIntake"));
  task.delivery["checkpoint"] = {{"version", 4}};
  task.state = TaskState::Completed;
  ASSERT_TRUE(store.update_hmas_task(task));
  Store restarted{github, config};
  FleetIssueService replay(restarted, config, auth);
  github->work["state"] = "CLOSED";
  github->work["updatedAt"] = "2026-09-13T03:00:00Z";
  const auto replayed = replay.import_request(json::parse(request()));
  EXPECT_EQ(replayed.status, 200) << replayed.body;
  EXPECT_EQ(replayed.body["state"], "Completed");
  EXPECT_EQ(replayed.body["provenance"], receipt["provenance"]);
  EXPECT_EQ(hmas_task_to_json(restarted.get_hmas_task(direct_task).value()),
            hmas_task_to_json(task));
  EXPECT_EQ(github->creates, 1);
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetIssueConfigured, UnknownSelectionAndExpectedRepositoryMismatchNeverReadGithub) {
  for (const auto field : {"repositoryKey", "repositoryId"}) {
    auto body = json::parse(request());
    body[field] = "unknown";
    const auto response = client->Post("/v1/fleet/issue-intakes", body.dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400) << response->body;
  }
  EXPECT_EQ(github->lookups, 0);
  EXPECT_EQ(github->fence_writes, 0);
  no_effects();
}

TEST_F(FleetIssueConfigured, WorkAndCommentIdentityMismatchCannotPersist) {
  auto body = json::parse(request());
  body["plan"] = {{"kind", "issue_comment"}, {"nodeId", "IC_fixture"}, {"digest", plan_digest}};
  github->comment["issue"]["id"] = "I_other";
  const auto wrong_parent =
      client->Post("/v1/fleet/issue-intakes", body.dump(), "application/json");
  ASSERT_TRUE(wrong_parent);
  EXPECT_EQ(wrong_parent->status, 503);
  github->work["__typename"] = "PullRequest";
  const auto pr = client->Post("/v1/fleet/issue-intakes", request(), "application/json");
  ASSERT_TRUE(pr);
  EXPECT_EQ(pr->status, 503);
  EXPECT_GT(github->lookups, 0);
  EXPECT_EQ(github->fence_writes, 0);
  no_effects();
}

TEST_F(FleetIssueConfigured, ChangedSnapshotAndClosedFirstIssueNeverCreate) {
  github->work["body"] = "Changed plan";
  const auto changed = client->Post("/v1/fleet/issue-intakes", request(), "application/json");
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->status, 409);
  github->work["body"] = plan_body;
  github->work["state"] = "CLOSED";
  const auto closed = client->Post("/v1/fleet/issue-intakes", request(), "application/json");
  ASSERT_TRUE(closed);
  EXPECT_EQ(closed->status, 409);
  EXPECT_EQ(github->fence_writes, 0);
  no_effects();
}

void FleetIssueConfigured::check_busy_store_deadline(bool fail_rendezvous,
                                                     bool throw_before_unblock) {
  const auto admitted = issue_service()->import_request(json::parse(request()));
  ASSERT_EQ(admitted.status, 201) << admitted.body;
  auto proposed = store.get_hmas_task(direct_task).value();
  ImportContext lookup;
  const auto work = resolve_work_issue(*config, *github, "Example/Project", 42, lookup);
  std::promise<void> entered;
  std::promise<void> release;
  std::promise<void> begin;
  auto begin_update = begin.get_future().share();
  auto released = release.get_future().share();
  std::once_flag begin_once;
  std::once_flag release_once;
  const auto start_update = [&] { std::call_once(begin_once, [&] { begin.set_value(); }); };
  const auto release_update = [&] { std::call_once(release_once, [&] { release.set_value(); }); };
  std::future<bool> holding;
  std::future<void> unblock;
  struct Cleanup {
    std::function<void()> run;
    ~Cleanup() { run(); }
  } cleanup{[&] {
    start_update();
    release_update();
    if (unblock.valid()) unblock.wait();
    if (holding.valid()) holding.wait();
    github->before_update = nullptr;
  }};
  github->before_update = [&] {
    entered.set_value();
    released.wait();
  };
  holding = std::async(std::launch::async, [&] {
    begin_update.wait();
    return store.update_hmas_task_state(direct_task, TaskState::Completed);
  });
  if (!fail_rendezvous) start_update();
  const auto rendezvous = entered.get_future().wait_for(fail_rendezvous ? std::chrono::seconds(0)
                                                                        : std::chrono::seconds(1));
  if (fail_rendezvous) start_update();
  ASSERT_EQ(rendezvous, std::future_status::ready);
  if (throw_before_unblock) throw std::runtime_error("Controlled failure before unblock launch");
  unblock = std::async(std::launch::async, [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    release_update();
  });
  ImportContext short_request{std::chrono::milliseconds(40)};
  const auto started = std::chrono::steady_clock::now();
  EXPECT_THROW(store.import_issue_task(proposed, work, short_request), std::runtime_error);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, std::chrono::milliseconds(180));
  unblock.get();
  EXPECT_TRUE(holding.get());
  github->before_update = nullptr;
  EXPECT_EQ(github->creates, 1);
  EXPECT_EQ(store.get_hmas_task(direct_task)->state, TaskState::Completed);
}

TEST_F(FleetIssueConfigured, SharedImportDeadlineIncludesBusyStoreLock) {
  check_busy_store_deadline();
}

TEST_F(FleetIssueConfigured, FailedLockRendezvousReleasesAndJoinsUpdate) {
  ::testing::TestPartResultArray failures;
  {
    ::testing::ScopedFakeTestPartResultReporter reporter(
        ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
    check_busy_store_deadline(true);
  }
  ASSERT_EQ(failures.size(), 1);
  EXPECT_TRUE(failures.GetTestPartResult(0).fatally_failed());
  EXPECT_EQ(store.get_hmas_task(direct_task)->state, TaskState::Completed);
  EXPECT_FALSE(github->before_update);
}

TEST_F(FleetIssueConfigured, ExceptionBeforeUnblockReleasesAndJoinsUpdate) {
  EXPECT_THROW(check_busy_store_deadline(false, true), std::runtime_error);
  EXPECT_EQ(store.get_hmas_task(direct_task)->state, TaskState::Completed);
  EXPECT_FALSE(github->before_update);
}

TEST_F(FleetIssueConfigured, CreatingFenceBlocksGenericAcquisitionAfterRestartAndConfigRemoval) {
  store.create_hmas_task(ordinary_task("existing-unrelated"));
  store.create_hmas_task(ordinary_task("split-parent"));
  github->lose_create_response = true;
  ASSERT_EQ(issue_service()->import_request(json::parse(request())).status, 503);
  ASSERT_EQ(github->creates, 1);
  ASSERT_EQ(github->fences.size(), 1u);
  ASSERT_EQ(github->fences.begin()->second.document.at("phase"), "creating");
  for (const bool configured : {true, false}) {
    SCOPED_TRACE(configured);
    Store restarted{github, configured ? config : nullptr};
    const auto before = github->created_issues;
    const auto fence_before = github->fences.begin()->second.document.dump();
    auto candidate = ordinary_task(configured ? "replacement-configured" : "replacement-disabled");
    candidate.repo = "Example/Project";
    candidate.issue = 42;
    EXPECT_THROW(restarted.create_hmas_task(candidate), std::runtime_error);
    auto retarget = restarted.get_hmas_task("existing-unrelated").value();
    retarget.repo = candidate.repo;
    retarget.issue = candidate.issue;
    EXPECT_THROW(restarted.update_hmas_task(retarget), std::runtime_error);
    auto child = candidate;
    child.id = configured ? "child-configured" : "child-disabled";
    const auto parent = restarted.get_hmas_task("split-parent").value();
    EXPECT_THROW(restarted.append_hmas_children(parent, {child}), std::runtime_error);
    EXPECT_EQ(github->created_issues, before);
    EXPECT_EQ(github->fences.begin()->second.document.dump(), fence_before);
    EXPECT_TRUE(restarted.update_hmas_task_state("existing-unrelated", TaskState::Completed));
    EXPECT_EQ(restarted.get_hmas_task("existing-unrelated")->state, TaskState::Completed);
  }
  EXPECT_EQ(github->creates, 1);
}

TEST_F(FleetIssueConfigured, LinkedTaskBlocksGenericDuplicateAndProvenanceInjection) {
  ASSERT_EQ(issue_service()->import_request(json::parse(request())).status, 201);
  const auto original = store.get_hmas_task(direct_task).value();
  const auto before = github->created_issues;
  auto duplicate = ordinary_task("generic-duplicate");
  duplicate.repo = original.repo;
  duplicate.issue = original.issue;
  EXPECT_THROW(store.create_hmas_task(duplicate), std::runtime_error);
  auto injected = original;
  injected.id = "injected-import";
  EXPECT_THROW(store.create_hmas_task(injected), std::invalid_argument);
  auto removed = original;
  removed.delivery = json::object();
  EXPECT_THROW(store.update_hmas_task(removed), std::invalid_argument);
  EXPECT_EQ(github->created_issues, before);
  EXPECT_EQ(github->creates, 1);
}

TEST_F(FleetIssueConfigured, ForbiddenQueryParametersAreRejectedBeforeGithubIo) {
  const auto registry = client->Get("/v1/fleet/issue-intakes/repositories?digest=forbidden");
  ASSERT_TRUE(registry);
  EXPECT_EQ(registry->status, 400);
  const auto imported =
      client->Post("/v1/fleet/issue-intakes?unknown=forbidden", request(), "application/json");
  ASSERT_TRUE(imported);
  EXPECT_EQ(imported->status, 400);
  const auto comment = client->Get("/v1/fleet/issue-intakes/implementation/42?planCommentId=%FF");
  ASSERT_TRUE(comment);
  EXPECT_EQ(comment->status, 400);
  EXPECT_EQ(github->lookups, 0);
  EXPECT_EQ(github->fence_writes, 0);
  no_effects();
}

TEST_F(FleetIssueConfigured, VerifiedUnreservedLegacyWorkAndDisabledReadRemainAvailable) {
  auto legacy = ordinary_task("original-legacy-owner");
  legacy.repo = "Example/Project";
  legacy.issue = 42;
  ASSERT_NO_THROW(store.create_hmas_task(legacy));
  ASSERT_EQ(github->created_issues.size(), 1u);
  EXPECT_TRUE(github->fences.empty());
  Store disabled{github};
  ASSERT_EQ(disabled.get_hmas_task(legacy.id)->issue, 42);
  EXPECT_TRUE(disabled.update_hmas_task_state(legacy.id, TaskState::Completed));
  EXPECT_EQ(disabled.get_hmas_task(legacy.id)->state, TaskState::Completed);
  EXPECT_EQ(issue_service()->import_request(json::parse(request())).status, 409);
  EXPECT_EQ(github->created_issues.size(), 1u);
  EXPECT_EQ(github->creates, 0);
}

TEST_F(FleetIssueConfigured, LostCreatingAcknowledgmentNeverGrantsFreshProcessAReplacementAttempt) {
  github->lose_creating_ack = true;
  EXPECT_EQ(issue_service()->import_request(json::parse(request())).status, 503);
  ASSERT_EQ(github->fences.size(), 1u);
  EXPECT_EQ(github->fences.begin()->second.document.at("phase"), "creating");
  EXPECT_EQ(github->creates, 0);
  github->lose_creating_ack = false;
  Store restarted{github, config};
  FleetIssueService retry{restarted, config, auth};
  EXPECT_EQ(retry.import_request(json::parse(request())).status, 503);
  EXPECT_EQ(github->creates, 0);
  EXPECT_EQ(github->fence_writes, 2);
  EXPECT_TRUE(github->created_issues.empty());
}

TEST_F(FleetIssueConfigured, LostPreparedAcknowledgmentAllowsOnlyObservationAcrossRestart) {
  github->lose_prepared_ack = true;
  EXPECT_EQ(issue_service()->import_request(json::parse(request())).status, 503);
  ASSERT_EQ(github->fences.size(), 1u);
  const auto retained = github->fences.begin()->second;
  ASSERT_EQ(retained.document.at("phase"), "prepared");
  EXPECT_TRUE(github->created_issues.empty());
  EXPECT_EQ(github->creates, 0);
  github->lose_prepared_ack = false;
  EXPECT_EQ(issue_service()->import_request(json::parse(request())).status, 503);
  Store restarted{github, config};
  FleetIssueService retry{restarted, config, auth};
  EXPECT_EQ(retry.import_request(json::parse(request())).status, 503);
  EXPECT_EQ(github->fences.begin()->second.sha, retained.sha);
  EXPECT_EQ(github->fences.begin()->second.document.dump(), retained.document.dump());
  EXPECT_EQ(github->fence_writes, 1);
  EXPECT_EQ(github->creates, 0);
  EXPECT_TRUE(github->created_issues.empty());
}

TEST_F(FleetIssueConfigured, FailureBeforeIntentDispatchCanRecoverByItsFirstActualGrant) {
  github->reject_prepared = true;
  EXPECT_EQ(issue_service()->import_request(json::parse(request())).status, 503);
  EXPECT_TRUE(github->fences.empty());
  EXPECT_EQ(github->creates, 0);
  github->reject_prepared = false;
  Store restarted{github, config};
  FleetIssueService retry{restarted, config, auth};
  EXPECT_EQ(retry.import_request(json::parse(request())).status, 201);
  EXPECT_EQ(github->creates, 1);
  EXPECT_EQ(github->created_issues.size(), 1u);
}

TEST_F(FleetIssueConfigured, MalformedImportedRawIdentityCannotHydrateOrBeRewrittenAsLegacy) {
  ASSERT_EQ(issue_service()->import_request(json::parse(request())).status, 201);
  const auto original = store.get_hmas_task(direct_task).value();
  for (int mutation = 0; mutation != 9; ++mutation) {
    SCOPED_TRACE(mutation);
    auto raw = hmas_task_to_json(original);
    if (mutation == 0) raw["child_task_ids"] = json::object();
    if (mutation == 1) raw["issue"] = 42.5;
    if (mutation == 2) raw["delivery"]["issueIntake"]["issue"]["number"] = 42.0;
    if (mutation == 3) raw["delivery"]["issueIntake"]["schema"] = "unknown";
    if (mutation == 4) raw["delivery"]["issueIntake"]["plan"]["digest"] = "wrong";
    if (mutation == 5) raw["delivery"].erase("issueIntake");
    if (mutation == 6) raw["delivery"]["researchIntake"] = json::object();
    if (mutation == 7) raw["delivery"]["issueIntake"]["issueId"] = "I_changed";
    if (mutation == 8) raw["module"] = "not-a-standalone-leaf";
    github->created_issues.at("1")["body"] =
        "## AgamemnonEntity: hmas-tasks/" + direct_task + "\n\n```json\n" + raw.dump() + "\n```\n";
    const auto retained = github->created_issues;
    Store restarted{github, config};
    EXPECT_THROW(restarted.get_hmas_task(direct_task), std::exception);
    EXPECT_THROW(restarted.update_hmas_task_state(direct_task, TaskState::Completed),
                 std::exception);
    EXPECT_EQ(github->created_issues, retained);
  }
}

TEST_F(FleetIssueConfigured, MalformedPreparedFenceCannotGrantBackingCreation) {
  github->reject_creating = true;
  EXPECT_EQ(issue_service()->import_request(json::parse(request())).status, 503);
  ASSERT_EQ(github->fences.size(), 1u);
  const auto original = github->fences.begin()->second;
  ASSERT_EQ(original.document.at("phase"), "prepared");
  github->reject_creating = false;
  for (int mutation = 0; mutation != 3; ++mutation) {
    SCOPED_TRACE(mutation);
    auto changed = original;
    if (mutation == 0) {
      changed.document.erase("backingIssue");
      changed.document["unknown"] = nullptr;
    }
    if (mutation == 1) changed.document["attemptId"] = "not-a-canonical-attempt";
    if (mutation == 2) changed.document["provenance"]["observedAt"] = false;
    github->fences.begin()->second = changed;
    const auto before = github->created_issues;
    Store restarted{github, config};
    FleetIssueService retry{restarted, config, auth};
    EXPECT_EQ(retry.import_request(json::parse(request())).status, 503);
    EXPECT_EQ(github->created_issues, before);
    EXPECT_EQ(github->fences.begin()->second.document, changed.document);
    EXPECT_EQ(github->creates, 0);
  }
}

TEST_F(FleetIssueConfigured, ExportsActualImportAndCanonicalOwnerTransition) {
  const auto registry = client->Get("/v1/fleet/issue-intakes/repositories");
  const auto inspection = client->Get("/v1/fleet/issue-intakes/implementation/42");
  ASSERT_TRUE(registry && inspection);
  ASSERT_EQ(registry->status, 200);
  ASSERT_EQ(inspection->status, 200);
  const auto imported = client->Post("/v1/fleet/issue-intakes", request(), "application/json");
  ASSERT_TRUE(imported);
  ASSERT_EQ(imported->status, 201) << imported->body;
  const auto pending = client->Get("/v1/tasks/" + direct_task + "/state");
  ASSERT_TRUE(pending);
  ASSERT_EQ(pending->status, 200);
  ASSERT_EQ(json::parse(pending->body).at("state"), "Pending");
  ASSERT_TRUE(publisher.calls.empty());
  FleetService fleet{store, publisher, &orchestrator, "fixture-operator-key"};
  fleet.create("pools", {{"id", "fixture-laptop"}, {"capacity", 1}});
  fleet.create("workers", {{"id", "fixture-worker"},
                           {"poolId", "fixture-laptop"},
                           {"capacity", 1},
                           {"host", "fixture-host"}});
  fleet.create("sessions", {{"id", "fixture-session"},
                            {"workerId", "fixture-worker"},
                            {"agentId", "fixture-agent"},
                            {"executionId", "fixture-execution"},
                            {"workspace", "/work/fixture-issue"},
                            {"taskId", direct_task},
                            {"domain", "pipeline"},
                            {"hmasRole", "task-agent"},
                            {"stage", "implementation"}});
  const auto command = fleet.command("sessions", "fixture-session", "start",
                                     {{"commandId", "fixture-start"},
                                      {"idempotencyKey", "fixture-start"},
                                      {"generation", 1},
                                      {"payload", json::object()}});
  const auto resource = fleet.get("sessions", "fixture-session");
  const auto claimed = client->Get("/v1/tasks/" + direct_task + "/state");
  ASSERT_TRUE(claimed);
  ASSERT_EQ(claimed->status, 200);
  const auto owned = json::parse(claimed->body);
  EXPECT_EQ(owned.at("state"), "Delegated");
  const auto& claim = owned.at("task").at("fleet_claim");
  EXPECT_EQ(claim.at("targetId"), resource.at("id"));
  EXPECT_EQ(claim.at("targetKind"), "sessions");
  for (const auto* field : {"workerId", "agentId", "workspace", "generation"})
    EXPECT_EQ(claim.at(field), resource.at(field));
  EXPECT_EQ(owned.at("task").at("delivery").at("issueIntake"),
            json::parse(imported->body).at("provenance"));
  ASSERT_EQ(publisher.calls.size(), 1u);
  EXPECT_EQ(publisher.calls[0].subject, "hi.myrmidon.pipeline.task-agent.task." + direct_task);
  Store restarted{github, config};
  FleetIssueService retry{restarted, config, auth};
  const auto replay = retry.import_request(json::parse(request()));
  ASSERT_EQ(replay.status, 200);
  EXPECT_EQ(replay.body.at("state"), "Delegated");
  EXPECT_EQ(replay.body.at("provenance"), json::parse(imported->body).at("provenance"));
  EXPECT_EQ(hmas_task_to_json(restarted.get_hmas_task(direct_task).value()), owned.at("task"));
  EXPECT_EQ(github->creates, 1);
  if (const char* output_path = std::getenv("FLEET_ISSUE_CONTRACT_OUTPUT")) {
    const json output{{"schema", "hi/agamemnon/issue-contract-fixture/v1"},
                      {"fixture", "controlled-controller-import-and-claim"},
                      {"registry", json::parse(registry->body)},
                      {"inspection", json::parse(inspection->body)},
                      {"importRequest", json::parse(request())},
                      {"importReceipt", json::parse(imported->body)},
                      {"pendingTask", json::parse(pending->body)},
                      {"claimCommand", command},
                      {"claimedTask", owned},
                      {"session", resource},
                      {"replayReceipt", replay.body}};
    std::ofstream stream(output_path, std::ios::binary);
    ASSERT_TRUE(stream);
    stream << output.dump(2) << '\n';
    stream.flush();
    ASSERT_TRUE(stream);
  }
}

TEST(FleetIssueIdentity, SameIssueNumberAcrossRepositoriesAndRegistryReorderingKeepNativeKeys) {
  class MultiRepository : public RetainedIssueGitHub {
   public:
    json import_work_issue(const std::string& owner, const std::string& name, int number,
                           ImportContext& context) override {
      context.checkpoint();
      EXPECT_EQ(owner, "Example");
      EXPECT_EQ(number, 42);
      EXPECT_TRUE(name == "Project" || name == "Other");
      auto selected = repository;
      auto selected_work = work;
      if (name == "Other") {
        selected["id"] = "R_other";
        selected["nameWithOwner"] = "Example/Other";
        selected_work["id"] = "I_other";
        selected_work["url"] = "https://github.com/Example/Other/issues/42";
      }
      selected["issue"] = selected_work;
      return {{"repository", selected}};
    }
  };
  auto github = std::make_shared<MultiRepository>();
  auto config = configuration();
  config->repositories.push_back(
      {{"key", "other"}, {"repository", "Example/Other"}, {"repositoryId", "R_other"}});
  Store store{github, config};
  AuthMiddleware auth{"fixture-issue-key"};
  FleetIssueService service{store, config, auth};
  auto request = [](const char* key, const char* repository_id, const char* issue_id) {
    return json{{"schema", "hi/agamemnon/issue-import/v1"},
                {"repositoryKey", key},
                {"issueNumber", 42},
                {"repositoryId", repository_id},
                {"issueId", issue_id},
                {"plan", {{"kind", "issue_body"}, {"digest", plan_digest}}}};
  };
  const auto first = service.import_request(request("implementation", "R_fixture", "I_fixture"));
  const auto second = service.import_request(request("other", "R_other", "I_other"));
  ASSERT_EQ(first.status, 201);
  ASSERT_EQ(second.status, 201);
  EXPECT_NE(first.body.at("taskId"), second.body.at("taskId"));
  EXPECT_EQ(github->fences.size(), 2u);
  auto reordered = std::make_shared<IssueImportConfiguration>(*config);
  std::swap(reordered->repositories[0], reordered->repositories[1]);
  reordered->repositories[1]["key"] = "renamed";
  Store restarted{github, reordered};
  FleetIssueService retry{restarted, reordered, auth};
  const auto replay = retry.import_request(request("renamed", "R_fixture", "I_fixture"));
  EXPECT_EQ(replay.status, 200);
  EXPECT_EQ(replay.body, first.body);
  EXPECT_EQ(github->creates, 2);
  EXPECT_EQ(github->created_issues.size(), 2u);
}

TEST_F(FleetIssueConfigured, ExplicitCommentSnapshotAcceptsEditRevertWithoutChangingProvenance) {
  auto body = json::parse(request());
  body["plan"] = {{"kind", "issue_comment"}, {"nodeId", "IC_fixture"}, {"digest", plan_digest}};
  const auto first = issue_service()->import_request(body);
  ASSERT_EQ(first.status, 201);
  github->comment["body"] = "A different plan";
  EXPECT_EQ(issue_service()->import_request(body).status, 409);
  github->comment["body"] = plan_body;
  github->comment["updatedAt"] = "2026-09-13T06:00:00Z";
  Store restarted{github, config};
  FleetIssueService retry{restarted, config, auth};
  const auto replay = retry.import_request(body);
  EXPECT_EQ(replay.status, 200);
  EXPECT_EQ(replay.body, first.body);
  EXPECT_EQ(github->creates, 1);
}

TEST_F(FleetIssueRoutes, DisabledRoutesRemainAuthenticatedWithoutEffects) {
  const auto registry = client->Get("/v1/fleet/issue-intakes/repositories");
  ASSERT_TRUE(registry);
  EXPECT_EQ(registry->status, 503);
  const auto inspection = client->Get("/v1/fleet/issue-intakes/implementation/42");
  ASSERT_TRUE(inspection);
  EXPECT_EQ(inspection->status, 503);
  const auto imported = client->Post("/v1/fleet/issue-intakes", request(), "application/json");
  ASSERT_TRUE(imported);
  EXPECT_EQ(imported->status, 503);
  client->set_default_headers({});
  const auto unauthenticated =
      client->Post("/v1/fleet/issue-intakes", request(), "application/json");
  ASSERT_TRUE(unauthenticated);
  EXPECT_EQ(unauthenticated->status, 401);
  no_effects();
}

TEST_F(FleetIssueRoutes, RequestRejectionPrecedesDisabledServiceAndAnyGithubCall) {
  const auto invalid = client->Post("/v1/fleet/issue-intakes", "{}", "application/json");
  ASSERT_TRUE(invalid);
  EXPECT_EQ(invalid->status, 400);
  const auto oversized =
      client->Post("/v1/fleet/issue-intakes", std::string(4097, 'x'), "application/json");
  ASSERT_TRUE(oversized);
  EXPECT_EQ(oversized->status, 413);
  no_effects();
}
}  // namespace agamemnon::test
