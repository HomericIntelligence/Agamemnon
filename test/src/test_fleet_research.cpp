#include "agamemnon/auth.hpp"
#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/fleet.hpp"
#include "agamemnon/fleet_research.hpp"
#include "agamemnon/github_client.hpp"
#include "agamemnon/metrics.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/rate_limiter.hpp"
#include "agamemnon/routes.hpp"
#include "agamemnon/store.hpp"
#include "agamemnon/version.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "httplib.h"
#include <gtest/gtest.h>

namespace agamemnon::test {

namespace {
const std::string research_task_id =
    "research-fa881bdde8da181cf538c601534b2c1ea548bd7addcb0efdf48bc5b904b0cb95";

json canonical_intake() {
  return {{"schema", "hi/nestor/intake/v1"},
          {"intakeId", "research-01"},
          {"requestDigest", std::string(64, 'a')},
          {"bodyDigest", std::string(64, 'b')},
          {"workRepository", "homeric/research"},
          {"phase", "created"},
          {"generation", 1},
          {"createdAt", "2026-09-13T01:00:00Z"},
          {"attemptId", std::string(32, 'c')},
          {"issue",
           {{"repository", "homeric/research"},
            {"number", 42},
            {"url", "https://github.com/homeric/research/issues/42"}}},
          {"receipt", {{"kind", "confirmed_issue"}, {"observedAt", "2026-09-13T01:00:01Z"}}}};
}

json research_provenance() {
  const auto record = canonical_intake();
  return {{"schema", "hi/agamemnon/research-intake/v1"},
          {"namespace", "nestor-main"},
          {"intakeId", record["intakeId"]},
          {"requestDigest", record["requestDigest"]},
          {"bodyDigest", record["bodyDigest"]},
          {"generation", record["generation"]},
          {"attemptId", record["attemptId"]},
          {"issue", record["issue"]},
          {"createdAt", record["createdAt"]},
          {"confirmedAt", record["receipt"]["observedAt"]}};
}

HmasTask imported_task() {
  HmasTask task{};
  task.id = research_task_id;
  task.layer = HmasLayer::L3_TaskAgent;
  task.state = TaskState::Pending;
  task.subject = "Research intake";
  task.description = "Execute the referenced canonical research issue.";
  task.repo = "homeric/research";
  task.issue = 42;
  task.created_at = "2026-09-13T01:00:02Z";
  task.delivery["researchIntake"] = research_provenance();
  return task;
}

json backing_issue(const HmasTask& task, int number = 7) {
  return {{"number", number},
          {"state", "open"},
          {"body", "## AgamemnonEntity: hmas-tasks/" + task.id + "\n\n```json\n" +
                       hmas_task_to_json(task).dump() + "\n```\n"}};
}

class RetainedResearchGitHub : public MockGitHubClient {
 public:
  enum class CreateOutcome { Success, Rejected, CommittedLost, CommittedEmpty, InvalidAck };
  std::mutex mutex;
  CreateOutcome outcome = CreateOutcome::Success;
  bool unavailable = false;
  int scans = 0;
  std::vector<json> retained;

  std::vector<json> list_issues_including_closed(std::string_view label) override {
    std::lock_guard lock(mutex);
    ++scans;
    if (unavailable) throw std::runtime_error("PRIVATE-UPSTREAM-SECRET");
    auto result = label == "agamemnon-hmas-task" ? retained : std::vector<json>{};
    for (const auto& [number, record] : created_issues) {
      if (record["label"] != label) continue;
      result.push_back({{"number", std::stoi(number)},
                        {"state", record.value("state", "open")},
                        {"body", record["body"]}});
    }
    return result;
  }

  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    std::lock_guard lock(mutex);
    const auto selected = std::exchange(outcome, CreateOutcome::Success);
    if (selected == CreateOutcome::Rejected) throw std::runtime_error("PRIVATE-UPSTREAM-SECRET");
    const auto number = MockGitHubClient::create_issue(title, body, label);
    if (selected == CreateOutcome::CommittedLost)
      throw std::runtime_error("PRIVATE-UPSTREAM-SECRET");
    if (selected == CreateOutcome::CommittedEmpty) return "";
    if (selected == CreateOutcome::InvalidAck) return "not-an-issue";
    return number;
  }

  void update_issue_body(std::string_view number, std::string_view body) override {
    std::lock_guard lock(mutex);
    MockGitHubClient::update_issue_body(number, body);
    for (auto& record : retained)
      if (std::to_string(record["number"].get<int>()) == number) record["body"] = body;
  }
};

class ControlledNestor : public NestorIntakeSource {
 public:
  json record = canonical_intake();
  int status = 200;
  bool unavailable = false;
  std::atomic<int> reads{0};
  NestorIntakeResponse lookup(const std::string& id) override {
    ++reads;
    EXPECT_EQ(id, "research-01");
    if (unavailable) throw std::runtime_error("PRIVATE-UPSTREAM-SECRET");
    return {status, record.dump()};
  }
};
}  // namespace

class FleetResearchImportRoutes : public ::testing::Test {
 protected:
  std::shared_ptr<RetainedResearchGitHub> github = std::make_shared<RetainedResearchGitHub>();
  FakeNatsPublisher publisher;
  Store store{github};
  AuthMiddleware auth{"research-import-test-key"};
  RateLimiter limiter{10000, 10000};
  MetricsRegistry metrics;
  Orchestrator orchestrator{store, publisher};
  httplib::Server server;
  std::unique_ptr<httplib::Client> client;
  std::thread listener;

  int port = 0;
  virtual std::shared_ptr<FleetResearchService> research_service() { return nullptr; }

  void SetUp() override {
    register_routes(server, store, publisher, limiter, auth, metrics, orchestrator, nullptr,
                    research_service());
    port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    listener = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
    client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(2);
    client->set_read_timeout(2);
    client->set_write_timeout(2);
    client->set_default_headers({{"Authorization", "Bearer research-import-test-key"}});
  }

  void TearDown() override {
    server.stop();
    if (listener.joinable()) listener.join();
  }

  std::string valid_request() {
    return json{{"schema", "hi/agamemnon/research-import/v1"},
                {"intakeId", "research-01"},
                {"requestDigest", std::string(64, 'a')}}
        .dump();
  }

  httplib::Result post(const std::string& body) {
    return client->Post("/v1/fleet/research-intakes", body, "application/json");
  }

  void expect_no_effects() {
    EXPECT_TRUE(github->created_issues.empty());
    EXPECT_TRUE(publisher.calls.empty());
  }
};

TEST_F(FleetResearchImportRoutes, UnconfiguredImportFailsClosedWithVersionedResponse) {
  const auto response = post(valid_request());
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 503) << response->body;
  EXPECT_EQ(response->get_header_value("X-API-Version"), std::string(kVersion));
  expect_no_effects();
}

TEST_F(FleetResearchImportRoutes, MalformedImportCannotCreateRecordsOrDispatch) {
  const std::vector<std::string> invalid{
      "{",
      "null",
      "[]",
      "{}",
      R"({"schema":"hi/agamemnon/research-import/v1","intakeId":false,"requestDigest":"bad"})",
      valid_request().substr(0, valid_request().size() - 1) + R"(,"prompt":"private sentinel"})",
      valid_request().substr(0, valid_request().size() - 1) + R"(,"intakeId":"research-02"})",
      std::string(4097, 'x')};
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    const auto response = post(invalid[index]);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400) << response->body;
    expect_no_effects();
  }
}

TEST_F(FleetResearchImportRoutes, AuthenticationStillPrecedesImport) {
  client->set_default_headers({});
  const auto response = post(valid_request());
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 401);
  expect_no_effects();
}

class FleetResearchConfigured : public FleetResearchImportRoutes {
 protected:
  std::shared_ptr<ControlledNestor> source = std::make_shared<ControlledNestor>();
  std::shared_ptr<FleetResearchService> research_service() override {
    return std::make_shared<FleetResearchService>(store, source, "nestor-main", auth);
  }
};

TEST_F(FleetResearchConfigured, ConfirmedAuthorityCreatesOnePendingLeafWithoutDispatch) {
  const auto response = post(valid_request());
  ASSERT_TRUE(response);
  ASSERT_EQ(response->status, 201) << response->body;
  const auto receipt = json::parse(response->body);
  EXPECT_EQ(receipt["schema"], "hi/agamemnon/research-import-receipt/v1");
  EXPECT_EQ(receipt["taskId"], research_task_id);
  EXPECT_EQ(receipt["state"], "Pending");
  EXPECT_EQ(receipt["provenance"], research_provenance());
  EXPECT_EQ(receipt["routing"],
            (json{{"domain", "research"}, {"hmasRole", "task-agent"}, {"stage", "research"}}));
  ASSERT_EQ(github->created_issues.size(), 1u);
  EXPECT_EQ(github->created_issues.begin()->second["label"], "agamemnon-hmas-task");
  const auto task = store.get_hmas_task(research_task_id);
  ASSERT_TRUE(task);
  EXPECT_EQ(task->layer, HmasLayer::L3_TaskAgent);
  EXPECT_EQ(task->repo, "homeric/research");
  EXPECT_EQ(task->issue, 42);
  EXPECT_EQ(task->delivery["researchIntake"], research_provenance());
  EXPECT_TRUE(task->parent_task_id.empty() && task->brief_id.empty() && task->blocked_by.empty() &&
              task->child_task_ids.empty() && task->assigned_lead_id.empty() &&
              task->fleet_claim.is_null());
  EXPECT_TRUE(publisher.calls.empty());
  EXPECT_EQ(source->reads, 1);
}

TEST_F(FleetResearchConfigured, ReplayAcrossFreshStorePreservesCurrentStateAndCheckpoints) {
  ASSERT_EQ(post(valid_request())->status, 201);
  auto task = store.get_hmas_task(research_task_id).value();
  task.state = TaskState::Completed;
  task.completed_at = "2026-09-13T02:00:00Z";
  task.delivery["otherCheckpoint"] = {{"sequence", 3}};
  ASSERT_TRUE(store.update_hmas_task(task));
  EXPECT_EQ(post(valid_request())->status, 200);
  Store restarted(github);
  FleetResearchService service(restarted, source, "nestor-main", auth);
  const auto result = service.import_request(json::parse(valid_request()));
  EXPECT_EQ(result.status, 200);
  EXPECT_EQ(result.body["taskId"], research_task_id);
  EXPECT_EQ(result.body["state"], "Completed");
  EXPECT_EQ(hmas_task_to_json(restarted.get_hmas_task(research_task_id).value()),
            hmas_task_to_json(task));
  EXPECT_EQ(github->created_issues.size(), 1u);
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetResearchConfigured, ConcurrentImportsSerializeOneDurableIdentity) {
  constexpr int count = 8;
  std::barrier start(count);
  std::vector<int> statuses(count);
  std::vector<std::thread> threads;
  for (int index = 0; index < count; ++index) {
    threads.emplace_back([&, index] {
      httplib::Client peer("127.0.0.1", port);
      peer.set_read_timeout(5);
      peer.set_default_headers({{"Authorization", "Bearer research-import-test-key"}});
      start.arrive_and_wait();
      const auto response =
          peer.Post("/v1/fleet/research-intakes", valid_request(), "application/json");
      statuses[index] = response ? response->status : 0;
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(std::count(statuses.begin(), statuses.end(), 201), 1);
  EXPECT_EQ(std::count(statuses.begin(), statuses.end(), 200), count - 1);
  EXPECT_EQ(github->created_issues.size(), 1u);
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetResearchConfigured, LostOrInvalidAcknowledgmentReconcilesBeforeAnotherCreate) {
  for (const auto outcome : {RetainedResearchGitHub::CreateOutcome::CommittedLost,
                             RetainedResearchGitHub::CreateOutcome::CommittedEmpty,
                             RetainedResearchGitHub::CreateOutcome::InvalidAck}) {
    auto external = std::make_shared<RetainedResearchGitHub>();
    external->outcome = outcome;
    Store first(external);
    FleetResearchService initial(first, source, "nestor-main", auth);
    const auto request = json::parse(valid_request());
    const auto failed = initial.import_request(request);
    EXPECT_EQ(failed.status, 503);
    EXPECT_EQ(failed.body.dump().find("PRIVATE-UPSTREAM-SECRET"), std::string::npos);
    ASSERT_EQ(external->created_issues.size(), 1u);
    external->unavailable = true;
    EXPECT_EQ(initial.import_request(request).status, 503);
    EXPECT_EQ(external->created_issues.size(), 1u);
    external->unavailable = false;
    Store restarted(external);
    FleetResearchService after_restart(restarted, source, "nestor-main", auth);
    EXPECT_EQ(after_restart.import_request(request).status, 200);
    EXPECT_EQ(external->created_issues.size(), 1u);
    EXPECT_GE(external->scans, 3);
  }
}

TEST_F(FleetResearchConfigured, RejectedWriteAndIncompleteEnumerationCannotReportSuccess) {
  github->outcome = RetainedResearchGitHub::CreateOutcome::Rejected;
  EXPECT_EQ(post(valid_request())->status, 503);
  expect_no_effects();
  github->unavailable = true;
  EXPECT_EQ(post(valid_request())->status, 503);
  expect_no_effects();
  github->unavailable = false;
  EXPECT_EQ(post(valid_request())->status, 201);
  EXPECT_EQ(github->created_issues.size(), 1u);
}

TEST_F(FleetResearchConfigured, RetainedAmbiguousClosedOrChangedRecordsBlockImport) {
  const auto valid = backing_issue(imported_task());
  auto closed = valid;
  closed["state"] = "closed";
  auto duplicate = valid;
  duplicate["number"] = 8;
  auto altered_task = imported_task();
  altered_task.delivery["researchIntake"]["bodyDigest"] = std::string(64, 'd');
  const std::vector<std::vector<json>> histories{
      {closed},
      {valid, duplicate},
      {backing_issue(altered_task)},
      {{{"number", 9}, {"state", "open"}, {"body", "unparseable retained record"}}}};
  for (const auto& history : histories) {
    auto external = std::make_shared<RetainedResearchGitHub>();
    external->retained = history;
    Store fresh(external);
    FleetResearchService importer(fresh, source, "nestor-main", auth);
    EXPECT_EQ(importer.import_request(json::parse(valid_request())).status, 409);
    EXPECT_TRUE(external->created_issues.empty());
  }
  ASSERT_EQ(post(valid_request())->status, 201);
  github->created_issues.begin()->second["state"] = "closed";
  EXPECT_EQ(post(valid_request())->status, 409);
  EXPECT_EQ(github->created_issues.size(), 1u);
}

TEST_F(FleetResearchConfigured, RetainedRawIdentityCannotNormalizeIntoAValidLeaf) {
  const auto valid_entity = hmas_task_to_json(imported_task());
  auto without_module = valid_entity;
  without_module.erase("module");
  std::vector<std::pair<std::string, json>> invalid{{"missing module", without_module}};
  for (const auto& [pointer, value] :
       std::vector<std::pair<std::string, json>>{{"/child_task_ids", ""},
                                                 {"/child_task_ids", json::object()},
                                                 {"/blocked_by", nullptr},
                                                 {"/blocked_by", false},
                                                 {"/issue", 42.5},
                                                 {"/issue", 42.0},
                                                 {"/issue", 4294967338ULL},
                                                 {"/delivery/researchIntake/generation", 1.0},
                                                 {"/delivery/researchIntake/issue/number", 42.0}}) {
    auto changed = valid_entity;
    changed[json::json_pointer(pointer)] = value;
    invalid.emplace_back(pointer + "=" + value.dump(), std::move(changed));
  }
  auto replay = [&](const json& entity, int expected_status) {
    auto external = std::make_shared<RetainedResearchGitHub>();
    auto issue = backing_issue(imported_task());
    issue["body"] = "## AgamemnonEntity: hmas-tasks/" + research_task_id + "\n\n```json\n" +
                    entity.dump() + "\n```\n";
    external->retained = {issue};
    const auto before = external->retained;
    Store fresh(external);
    FleetResearchService importer(fresh, source, "nestor-main", auth);
    EXPECT_EQ(importer.import_request(json::parse(valid_request())).status, expected_status);
    EXPECT_EQ(external->retained, before);
    EXPECT_TRUE(external->created_issues.empty());
    EXPECT_TRUE(external->calls.empty());
  };
  {
    SCOPED_TRACE("valid retained record positive control");
    replay(valid_entity, 200);
  }
  for (const auto& [name, entity] : invalid) {
    SCOPED_TRACE(name);
    replay(entity, 409);
  }
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetResearchConfigured, UnconfirmedMismatchedAndAbsentAuthorityDoNotCreate) {
  for (const auto& phase : {"prepared", "creating"}) {
    source->record = canonical_intake();
    source->record["phase"] = phase;
    source->record.erase("issue");
    source->record.erase("receipt");
    if (std::string(phase) == "prepared") source->record.erase("attemptId");
    EXPECT_EQ(post(valid_request())->status, 409);
    expect_no_effects();
  }
  source->record = canonical_intake();
  source->record["requestDigest"] = std::string(64, 'd');
  EXPECT_EQ(post(valid_request())->status, 409);
  source->status = 404;
  EXPECT_EQ(post(valid_request())->status, 404);
  expect_no_effects();
}

TEST_F(FleetResearchConfigured, MalformedOrUnavailableAuthorityKeepsPrivateTextOutOfReplies) {
  const std::vector<std::pair<std::string, json>> mutations{
      {"/schema", "unsupported"},
      {"/generation", true},
      {"/generation", 2},
      {"/intakeId", "research-02"},
      {"/bodyDigest", "PRIVATE-UPSTREAM-SECRET"},
      {"/createdAt", "yesterday"},
      {"/attemptId", "x"},
      {"/workRepository", "Homeric/research"},
      {"/issue/number", 2147483648LL},
      {"/issue/number", 0},
      {"/issue/number", true},
      {"/issue/url", "https://example.com/PRIVATE-UPSTREAM-SECRET"},
      {"/receipt/kind", "uncertain"},
      {"/receipt/observedAt", "bad"},
      {"/privateText", "PRIVATE-UPSTREAM-SECRET"}};
  for (const auto& [pointer, value] : mutations) {
    SCOPED_TRACE(pointer);
    source->record = canonical_intake();
    source->record[json::json_pointer(pointer)] = value;
    const auto response = post(valid_request());
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503);
    EXPECT_EQ(response->body.find("PRIVATE-UPSTREAM-SECRET"), std::string::npos);
    expect_no_effects();
  }
  source->unavailable = true;
  EXPECT_EQ(post(valid_request())->status, 503);
  expect_no_effects();
}

TEST(ResearchIntakeProvenance, LegacyWritesCannotReplaceImportedIdentity) {
  const std::vector<std::function<void(HmasTask&)>> changes{
      [](auto& task) { task.delivery.erase("researchIntake"); },
      [](auto& task) { task.delivery["researchIntake"]["namespace"] = "another"; },
      [](auto& task) { task.repo = "another/repo"; },
      [](auto& task) { ++task.issue; },
      [](auto& task) { task.layer = HmasLayer::L0_ChiefArchitect; },
      [](auto& task) { task.parent_task_id = "another"; },
      [](auto& task) { task.brief_id = "another"; },
      [](auto& task) { task.child_task_ids.push_back("another"); },
      [](auto& task) { task.blocked_by.push_back("another"); }};
  for (std::size_t index = 0; index < changes.size(); ++index) {
    SCOPED_TRACE(index);
    auto external = std::make_shared<RetainedResearchGitHub>();
    external->retained = {backing_issue(imported_task())};
    Store store(external);
    auto altered = imported_task();
    changes[index](altered);
    EXPECT_THROW(store.update_hmas_task(altered), std::exception);
    EXPECT_TRUE(external->updated_bodies.empty());
    EXPECT_EQ(hmas_task_to_json(store.get_hmas_task(research_task_id).value()),
              hmas_task_to_json(imported_task()));
  }
}

TEST(ResearchIntakeProvenance, DeliveryReplacementAndLegacyCreationCannotBypassOwnership) {
  auto external = std::make_shared<RetainedResearchGitHub>();
  external->retained = {backing_issue(imported_task())};
  Store store(external);
  const auto task = imported_task();
  EXPECT_THROW(store.update_hmas_delivery(task.id, task.delivery, json::object()), std::exception);
  auto allowed = task.delivery;
  allowed["checkpoint"] = {{"attempt", 1}};
  EXPECT_TRUE(store.update_hmas_delivery(task.id, task.delivery, allowed));
  HmasTask child{};
  child.id = "child-task";
  child.layer = HmasLayer::L3_TaskAgent;
  child.state = TaskState::Pending;
  const auto current = store.get_hmas_task(task.id).value();
  EXPECT_THROW(store.append_hmas_children(current, {child}), std::exception);
  auto other_external = std::make_shared<RetainedResearchGitHub>();
  Store other(other_external);
  EXPECT_THROW(other.create_hmas_task(task), std::exception);
  EXPECT_TRUE(other_external->created_issues.empty());
}

TEST(ResearchIntakeProvenance, GenericUpdatesPreserveNumericTypesInImmutableProvenance) {
  for (const auto& operation : {"task", "delivery"}) {
    for (const auto& [pointer, value] : std::vector<std::pair<std::string, json>>{
             {"/researchIntake/generation", 1.0}, {"/researchIntake/issue/number", 42.0}}) {
      SCOPED_TRACE(std::string(operation) + ":" + pointer);
      auto external = std::make_shared<RetainedResearchGitHub>();
      external->retained = {backing_issue(imported_task())};
      const auto original = external->retained;
      Store store(external);
      auto task = imported_task();
      const auto original_delivery = task.delivery;
      task.delivery[json::json_pointer(pointer)] = value;
      if (std::string(operation) == "task") {
        EXPECT_THROW(store.update_hmas_task(task), std::invalid_argument);
      } else {
        EXPECT_THROW(store.update_hmas_delivery(task.id, original_delivery, task.delivery),
                     std::invalid_argument);
      }
      EXPECT_TRUE(external->updated_bodies.empty());
      EXPECT_TRUE(external->created_issues.empty());
      EXPECT_EQ(external->retained, original);
      EXPECT_EQ(store.get_hmas_task(task.id)->delivery.dump(), original_delivery.dump());
    }
  }
  auto external = std::make_shared<RetainedResearchGitHub>();
  external->retained = {backing_issue(imported_task())};
  Store store(external);
  auto task = imported_task();
  task.delivery["checkpoint"] = {{"attempt", 1}};
  EXPECT_TRUE(store.update_hmas_task(task));
  EXPECT_EQ(store.get_hmas_task(task.id)->delivery.dump(), task.delivery.dump());
  EXPECT_EQ(external->updated_bodies.size(), 1u);
}

TEST(ResearchIntakeConfiguration, ConstructorRequiresPersistenceAuthAndStableNamespace) {
  auto external = std::make_shared<RetainedResearchGitHub>();
  auto source = std::make_shared<ControlledNestor>();
  Store durable(external);
  Store memory;
  AuthMiddleware authenticated("test-key");
  AuthMiddleware permissive("");
  EXPECT_THROW(FleetResearchService(memory, source, "nestor-main", authenticated), std::exception);
  EXPECT_THROW(FleetResearchService(durable, source, "nestor-main", permissive), std::exception);
  for (const auto& invalid : {"", "UPPER", "a/b", "namespace with spaces"})
    EXPECT_THROW(FleetResearchService(durable, source, invalid, authenticated), std::exception);
  EXPECT_THROW(FleetResearchService(durable, nullptr, "nestor-main", authenticated),
               std::exception);
  EXPECT_NO_THROW(FleetResearchService(durable, source, "nestor-main", authenticated));
}

namespace {
json import_request() {
  return {{"schema", "hi/agamemnon/research-import/v1"},
          {"intakeId", "research-01"},
          {"requestDigest", std::string(64, 'a')}};
}

class ResearchOrigin {
 public:
  httplib::Server server;
  std::atomic<int> reads{0};
  std::atomic<int> requests{0};
  std::thread listener;
  int port;

  explicit ResearchOrigin(httplib::Server::Handler handler) {
    server.set_pre_routing_handler([this](const auto&, auto&) {
      ++requests;
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.Get("/v1/research/intakes/research-01", [this, handler](const auto& req, auto& res) {
      ++reads;
      handler(req, res);
    });
    port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw std::runtime_error("local fixture bind failed");
    listener = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
  }
  ~ResearchOrigin() {
    server.stop();
    if (listener.joinable()) listener.join();
  }
  std::string origin() const { return "http://127.0.0.1:" + std::to_string(port); }
  NestorResearchConfig config() const { return {origin(), "private-fixture-key", "nestor-main"}; }
};

class ScopedResearchProxy {
 public:
  explicit ScopedResearchProxy(const std::string& origin) {
    for (const auto* name :
         {"http_proxy", "HTTP_PROXY", "ALL_PROXY", "all_proxy", "no_proxy", "NO_PROXY"}) {
      const auto* previous = std::getenv(name);
      saved.emplace_back(name, previous ? std::optional<std::string>(previous) : std::nullopt);
      const bool bypass = std::string(name) == "no_proxy" || std::string(name) == "NO_PROXY";
      if (setenv(name, bypass ? "" : origin.c_str(), 1) != 0)
        throw std::runtime_error("fixture proxy setup failed");
    }
  }
  ~ScopedResearchProxy() {
    for (const auto& [name, previous] : saved) {
      if (previous)
        setenv(name.c_str(), previous->c_str(), 1);
      else
        unsetenv(name.c_str());
    }
  }

 private:
  std::vector<std::pair<std::string, std::optional<std::string>>> saved;
};
}  // namespace

TEST(ResearchIntakeTransport, RealLookupUsesOnlyConfiguredGetAndIgnoresAmbientProxy) {
  ResearchOrigin proxy([](const auto&, auto& res) { res.status = 502; });
  ResearchOrigin nestor([](const auto& req, auto& res) {
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/v1/research/intakes/research-01");
    EXPECT_EQ(req.get_header_value("Authorization"), "Bearer private-fixture-key");
    res.set_content(canonical_intake().dump(), "application/json");
  });
  ScopedResearchProxy environment(proxy.origin());
  auto source = std::make_shared<CurlNestorIntakeSource>(nestor.config());
  auto github = std::make_shared<RetainedResearchGitHub>();
  Store store(github);
  AuthMiddleware auth("operator-key");
  FleetResearchService importer(store, source, "nestor-main", auth);
  const auto response = importer.import_request(import_request());
  EXPECT_EQ(response.status, 201) << response.body;
  EXPECT_EQ(nestor.reads, 1);
  EXPECT_EQ(proxy.requests, 0);
  for (const auto& call : github->calls) {
    EXPECT_EQ(call.arg2.find("private-fixture-key"), std::string::npos);
  }
  EXPECT_EQ(response.body.dump().find("private-fixture-key"), std::string::npos);
}

TEST(ResearchIntakeTransport, RedirectDoesNotForwardTheConfiguredCredential) {
  ResearchOrigin destination([](const auto&, auto& res) {
    res.set_content(canonical_intake().dump(), "application/json");
  });
  ResearchOrigin origin([&](const auto&, auto& res) {
    res.set_redirect(destination.origin() + "/v1/research/intakes/research-01", 302);
  });
  auto source = std::make_shared<CurlNestorIntakeSource>(origin.config());
  auto github = std::make_shared<RetainedResearchGitHub>();
  Store store(github);
  AuthMiddleware auth("operator-key");
  FleetResearchService importer(store, source, "nestor-main", auth);
  EXPECT_EQ(importer.import_request(import_request()).status, 503);
  EXPECT_EQ(origin.reads, 1);
  EXPECT_EQ(destination.requests, 0);
  EXPECT_TRUE(github->created_issues.empty());
}

TEST(ResearchIntakeTransport, MalformedOversizeTruncatedAndSlowRepliesStayUnavailable) {
  for (const auto& mode : {"malformed", "oversize", "truncated", "slow"}) {
    SCOPED_TRACE(mode);
    ResearchOrigin origin([mode](const auto&, auto& res) {
      const std::string selected(mode);
      if (selected == "truncated") {
        res.set_content_provider(1000, "application/json",
                                 [](std::size_t, std::size_t, httplib::DataSink& sink) {
                                   sink.write("{}", 2);
                                   return false;
                                 });
      } else if (selected == "oversize") {
        res.set_content(std::string(65537, 'x'), "application/json");
      } else if (selected == "slow") {
        std::this_thread::sleep_for(std::chrono::seconds(6));
        res.set_content(canonical_intake().dump(), "application/json");
      } else {
        res.set_content("{PRIVATE-UPSTREAM-SECRET", "application/json");
      }
    });
    auto source = std::make_shared<CurlNestorIntakeSource>(origin.config());
    auto github = std::make_shared<RetainedResearchGitHub>();
    Store store(github);
    AuthMiddleware auth("operator-key");
    FleetResearchService importer(store, source, "nestor-main", auth);
    const auto started = std::chrono::steady_clock::now();
    const auto response = importer.import_request(import_request());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(origin.reads, 1);
    if (std::string(mode) == "slow") EXPECT_LT(elapsed, std::chrono::milliseconds(5800));
    EXPECT_EQ(response.body.dump().find("PRIVATE-UPSTREAM-SECRET"), std::string::npos);
    EXPECT_TRUE(github->created_issues.empty());
  }
}

TEST(ResearchIntakeTransport, LookupRejectsPathInjectionBeforeNetwork) {
  ResearchOrigin origin([](const auto&, auto& res) { res.status = 500; });
  CurlNestorIntakeSource source(origin.config());
  for (const auto& id : {"../research-01", "research-01?secret", "research-01/other", "SHORT"})
    EXPECT_THROW(source.lookup(id), std::exception);
  EXPECT_EQ(origin.reads, 0);
}

TEST(ResearchIntakeConfiguration, OnlyCompleteBoundedOperatorConfigurationEnablesImport) {
  EXPECT_FALSE(research_import_configuration(std::nullopt, std::nullopt, std::nullopt, false));
  const auto configured =
      research_import_configuration("http://127.0.0.1:12345", "test-key", "nestor-main", true);
  EXPECT_TRUE(configured);
  if (configured) {
    EXPECT_EQ(configured->origin, "http://127.0.0.1:12345");
    EXPECT_EQ(configured->authority_namespace, "nestor-main");
  }
  const std::vector<std::tuple<std::optional<std::string>, std::optional<std::string>,
                               std::optional<std::string>, bool>>
      invalid{{"http://127.0.0.1", std::nullopt, "nestor-main", true},
              {std::nullopt, "key", std::nullopt, true},
              {"", "", "", true},
              {"http://127.0.0.1", "key", "nestor-main", false},
              {"http://127.0.0.1", "key\r\nInjected: value", "nestor-main", true},
              {"http://127.0.0.1", "", "nestor-main", true},
              {"http://127.0.0.1", "key", "UPPER", true}};
  for (const auto& [origin, key, authority, persistence] : invalid)
    EXPECT_THROW(research_import_configuration(origin, key, authority, persistence),
                 std::exception);
  for (const auto& invalid_origin :
       {"http://example.com", "http://localhost", "https://user:pass@example.com",
        "https://example.com/path", "https://example.com?key=private",
        "https://example.com#private", "file:///private", "https://example.com:99999"}) {
    EXPECT_THROW(research_import_configuration(invalid_origin, "key", "nestor-main", true),
                 std::exception);
    EXPECT_THROW(
        CurlNestorIntakeSource((NestorResearchConfig{invalid_origin, "key", "nestor-main"})),
        std::exception);
  }
  EXPECT_TRUE(
      research_import_configuration("https://nestor.example.com", "key", "nestor-main", true));
  EXPECT_TRUE(research_import_configuration("http://[::1]:12345", "key", "nestor-main", true));
}

TEST(ResearchIntakeConfiguration, EnabledRouteRejectsPermissiveMiddleware) {
  auto github = std::make_shared<RetainedResearchGitHub>();
  Store store(github);
  FakeNatsPublisher publisher;
  Orchestrator orchestrator(store, publisher);
  MetricsRegistry metrics;
  RateLimiter limiter(10000, 10000);
  AuthMiddleware configured("key");
  AuthMiddleware permissive("");
  auto source = std::make_shared<ControlledNestor>();
  auto importer = std::make_shared<FleetResearchService>(store, source, "nestor-main", configured);
  httplib::Server server;
  EXPECT_THROW(register_routes(server, store, publisher, limiter, permissive, metrics, orchestrator,
                               nullptr, importer),
               std::exception);
  EXPECT_TRUE(github->created_issues.empty());
  EXPECT_TRUE(publisher.calls.empty());
}

TEST_F(FleetResearchConfigured, ExistingFleetControlsKeepTheImportedTaskAndProvenance) {
  ASSERT_EQ(post(valid_request())->status, 201);
  FleetService fleet(store, publisher, &orchestrator, "operator-resolution-key");
  fleet.create("pools", {{"id", "pool"}, {"capacity", 1}});
  fleet.create("workers",
               {{"id", "worker"}, {"poolId", "pool"}, {"capacity", 1}, {"host", "laptop"}});
  fleet.create("sessions", {{"id", "session"},
                            {"workerId", "worker"},
                            {"agentId", "researcher"},
                            {"workspace", "/work/research"},
                            {"taskId", research_task_id},
                            {"domain", "research"},
                            {"hmasRole", "task-agent"},
                            {"stage", "research"}});
  ASSERT_TRUE(publisher.calls.empty());
  auto command = [](const std::string& id, const json& payload) {
    return json{{"commandId", id}, {"idempotencyKey", id}, {"generation", 1}, {"payload", payload}};
  };
  auto acknowledge = [&](const std::string& id, const json& receipt) {
    fleet.acknowledge("sessions", "session",
                      {{"schema", "hi/fleet/v1"},
                       {"eventId", id + "-ack"},
                       {"workerId", "worker"},
                       {"commandId", id},
                       {"generation", 1},
                       {"status", "completed"},
                       {"receipt", receipt}});
  };
  fleet.command("sessions", "session", "start", command("start", json::object()));
  ASSERT_EQ(publisher.calls.size(), 1u);
  EXPECT_EQ(publisher.calls[0].subject, "hi.myrmidon.research.task-agent.task." + research_task_id);
  EXPECT_EQ(store.get_hmas_task(research_task_id)->state, TaskState::Delegated);
  acknowledge("start", json::object());
  fleet.command("sessions", "session", "input",
                command("input", {{"promptRef", std::string(32, 'a') + ".json"}}));
  acknowledge("input", {{"providerTurnId", "fixture-turn"}});
  fleet.command("sessions", "session", "respond",
                command("respond", {{"requestId", "approval-1"},
                                    {"responseRef", std::string(32, 'b') + ".json"}}));
  acknowledge("respond", json::object());
  EXPECT_EQ(publisher.calls.size(), 3u);
  EXPECT_EQ(publisher.calls[1].subject, "hi.fleet.control.worker");
  EXPECT_EQ(publisher.calls[2].subject, "hi.fleet.control.worker");
  auto task = store.get_hmas_task(research_task_id).value();
  EXPECT_EQ(task.delivery["researchIntake"], research_provenance());
  ASSERT_TRUE(store.observe_hmas_fleet_start(task.id, task.fleet_claim));
  const json decision{{"outcome", "completed"}, {"decisionId", "resolved"}};
  ASSERT_TRUE(store.resolve_hmas_fleet_task(task.id, task.fleet_claim, decision));
  Store restarted(github);
  FleetResearchService importer(restarted, source, "nestor-main", auth);
  const auto replay = importer.import_request(import_request());
  EXPECT_EQ(replay.status, 200);
  EXPECT_EQ(replay.body["state"], "Completed");
  const auto retained = restarted.get_hmas_task(task.id).value();
  EXPECT_EQ(retained.fleet_claim, task.fleet_claim);
  EXPECT_EQ(retained.fleet_resolution, decision);
  EXPECT_EQ(retained.delivery["researchIntake"], research_provenance());
  EXPECT_EQ(publisher.calls.size(), 3u);
}

}  // namespace agamemnon::test
