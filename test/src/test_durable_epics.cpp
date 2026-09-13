#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/fleet_issue.hpp"
#include "agamemnon/github_client.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/store.hpp"

#include <gtest/gtest.h>

namespace agamemnon::test {
class EpicGitHub : public MockGitHubClient {
 public:
  bool fail_create = false;
  bool lose_create_response = false;
  int fail_update = 0;
  int updates = 0;
  bool reserved_work = false;
  int work_reads = 0;
  int fence_reads = 0;
  json import_work_issue(const std::string& owner, const std::string& name, int number,
                         ImportContext& context) override {
    context.checkpoint();
    ++work_reads;
    EXPECT_EQ(owner + "/" + name, "homeric/repo");
    EXPECT_EQ(number, 42);
    return {{"repository",
             {{"id", "R_epic_fixture"},
              {"nameWithOwner", "homeric/repo"},
              {"issue",
               {{"__typename", "Issue"},
                {"id", "I_epic_fixture"},
                {"number", 42},
                {"url", "https://github.com/homeric/repo/issues/42"},
                {"state", "OPEN"},
                {"title", "Controlled epic"},
                {"body", "Explicit unreserved work namespace"}}}}}};
  }
  std::vector<json> import_list_issues(ImportContext& context) override {
    context.checkpoint();
    return list_issues_including_closed("agamemnon-hmas-task");
  }
  std::optional<ImportFence> import_read_fence(const std::string& branch, const std::string&,
                                               ImportContext& context) override {
    context.checkpoint();
    ++fence_reads;
    EXPECT_EQ(branch, "import-state");
    if (reserved_work) return ImportFence{std::string(40, 'a'), {{"phase", "creating"}}};
    return std::nullopt;
  }
  std::vector<json> list_issues_including_closed(std::string_view label) override {
    if (fail_list_on_label == label) throw std::runtime_error("unavailable");
    std::vector<json> result;
    for (auto& [id, issue] : created_issues)
      if (issue["label"] == label)
        result.push_back({{"number", std::stoi(id)}, {"body", issue["body"]}});
    return result;
  }
  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    if (fail_create) return "";
    auto id = MockGitHubClient::create_issue(title, body, label);
    if (lose_create_response) {
      lose_create_response = false;
      throw std::runtime_error("lost create response");
    }
    return id;
  }
  void update_issue_body(std::string_view id, std::string_view body) override {
    if (++updates == fail_update) throw std::runtime_error("update unavailable");
    MockGitHubClient::update_issue_body(id, body);
  }
};
static std::shared_ptr<IssueImportConfiguration> epic_configuration() {
  auto config = std::make_shared<IssueImportConfiguration>();
  config->state_branch = "import-state";
  config->repositories = json::array(
      {{{"key", "epic"}, {"repository", "homeric/repo"}, {"repositoryId", "R_epic_fixture"}}});
  return config;
}
static json epic() {
  return {{"schema", "hi/v1"},
          {"msg_id", "registration-1"},
          {"epic", {{"repo", "Homeric/repo"}, {"issue", 42}, {"key", "homeric-repo-42"}}},
          {"children", {43, 44}},
          {"workflow", "feature"}};
}
static const std::string subject = "hi.pipeline.epic.homeric-repo-42.registered";

TEST(DurableEpics, ReplayUsesOneBriefAndTaskAcrossRestart) {
  auto gh = std::make_shared<EpicGitHub>();
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  Orchestrator first(store, bus);
  const auto id = first.on_epic_registered(subject, epic().dump(), true);
  ASSERT_FALSE(id.empty());
  Store restarted(gh, epic_configuration());
  Orchestrator second(restarted, bus);
  EXPECT_EQ(second.on_epic_registered(subject, epic().dump(), true), id);
  EXPECT_EQ(gh->created_issues.size(), 2u);
  EXPECT_EQ(bus.calls.size(), 1u);
}

TEST(DurableEpics, FailedBriefWriteCannotPublishOrCache) {
  auto gh = std::make_shared<EpicGitHub>();
  gh->fail_create = true;
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  Orchestrator orch(store, bus);
  EXPECT_THROW(orch.on_epic_registered(subject, epic().dump(), true), std::runtime_error);
  EXPECT_TRUE(bus.calls.empty());
  EXPECT_TRUE(store.list_task_briefs().empty());
}

TEST(DurableEpics, MissingConfigurationOrRetainedImportFenceCannotAcquireWork) {
  for (const bool configured : {false, true}) {
    SCOPED_TRACE(configured);
    auto gh = std::make_shared<EpicGitHub>();
    gh->reserved_work = true;
    FakeNatsPublisher bus;
    Store store(gh, configured ? epic_configuration() : nullptr);
    Orchestrator orchestrator(store, bus);
    EXPECT_THROW(orchestrator.on_epic_registered(subject, epic().dump(), true), std::runtime_error);
    EXPECT_TRUE(bus.calls.empty());
    EXPECT_TRUE(store.list_hmas_tasks_by_layer(HmasLayer::L0_ChiefArchitect).empty());
    EXPECT_EQ(gh->work_reads, configured ? 1 : 0);
    EXPECT_EQ(gh->fence_reads, configured ? 1 : 0);
    ASSERT_EQ(gh->created_issues.size(), 1u);
    EXPECT_EQ(gh->created_issues.begin()->second.at("label"), "agamemnon-brief");
  }
}

TEST(DurableEpics, MemoryOnlyAndSubjectMismatchAreRejected) {
  FakeNatsPublisher bus;
  Store memory;
  Orchestrator orch(memory, bus);
  EXPECT_THROW(orch.on_epic_registered(subject, epic().dump(), true), std::runtime_error);
  auto gh = std::make_shared<EpicGitHub>();
  Store store(gh, epic_configuration());
  Orchestrator durable(store, bus);
  EXPECT_THROW(durable.on_epic_registered("hi.pipeline.epic.other.registered", epic().dump(), true),
               std::invalid_argument);
  EXPECT_TRUE(gh->created_issues.empty());
}

TEST(DurableEpics, ChangedMessageIdReplaysButChangedPlanConflicts) {
  auto gh = std::make_shared<EpicGitHub>();
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  Orchestrator orch(store, bus);
  auto id = orch.on_epic_registered(subject, epic().dump(), true);
  auto replay = epic();
  replay["msg_id"] = "new-publisher-uuid";
  EXPECT_EQ(orch.on_epic_registered(subject, replay.dump(), true), id);
  replay["children"].push_back(45);
  EXPECT_THROW(orch.on_epic_registered(subject, replay.dump(), true), std::invalid_argument);
  EXPECT_EQ(gh->created_issues.size(), 2u);
  EXPECT_EQ(bus.calls.size(), 1u);
}

TEST(DurableEpics, RepositoryCaseVariantsConvergeAcrossRestart) {
  auto gh = std::make_shared<EpicGitHub>();
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  Orchestrator first(store, bus);
  auto original = epic();
  original["workflow"] = "BuildFeature";
  const auto id = first.on_epic_registered(subject, original.dump(), true);
  Store restarted(gh, epic_configuration());
  Orchestrator second(restarted, bus);
  auto replay = original;
  replay["msg_id"] = "other-producer-uuid";
  replay["epic"]["repo"] = "homeric/REPO";
  EXPECT_EQ(second.on_epic_registered(subject, replay.dump(), true), id);
  EXPECT_EQ(gh->created_issues.size(), 2u);
  ASSERT_EQ(bus.calls.size(), 1u);
  const auto sent = json::parse(bus.calls[0].payload);
  EXPECT_EQ(sent["epic"]["repo"], "homeric/repo");
  EXPECT_EQ(sent["workflow"], "BuildFeature");
  EXPECT_EQ(restarted.list_hmas_tasks_by_brief(id).at(0).repo, "homeric/repo");
  replay["workflow"] = "buildfeature";
  EXPECT_THROW(second.on_epic_registered(subject, replay.dump(), true), std::invalid_argument);
  EXPECT_EQ(gh->created_issues.size(), 2u);
  EXPECT_EQ(bus.calls.size(), 1u);
}

TEST(DurableEpics, PriorCaseSensitiveRootRequiresExplicitMigration) {
  auto gh = std::make_shared<EpicGitHub>();
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  HmasTask legacy;
  legacy.id = "epic-root-prior-uppercase-hash";
  legacy.layer = HmasLayer::L0_ChiefArchitect;
  legacy.state = TaskState::Decomposing;
  legacy.repo = "Homeric/repo";
  legacy.issue = 42;
  legacy.delivery["registration"] = {{"schema", "hi/epic-registration/v1"},
                                     {"epic", epic()["epic"]},
                                     {"children", {43, 44}},
                                     {"workflow", "feature"},
                                     {"team_id", "mesh"}};
  store.create_hmas_task(legacy);
  Store restarted(gh, epic_configuration());
  Orchestrator orch(restarted, bus);
  auto incoming = epic();
  incoming["epic"]["repo"] = "homeric/repo";
  EXPECT_THROW(orch.on_epic_registered(subject, incoming.dump(), true), std::invalid_argument);
  EXPECT_EQ(gh->created_issues.size(), 1u);
  EXPECT_TRUE(bus.calls.empty());
}

TEST(DurableEpics, UncertainBriefCreateRecoversFromAcknowledgedRead) {
  auto gh = std::make_shared<EpicGitHub>();
  gh->lose_create_response = true;
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  Orchestrator orch(store, bus);
  EXPECT_THROW(orch.on_epic_registered(subject, epic().dump(), true), std::runtime_error);
  EXPECT_TRUE(bus.calls.empty());
  EXPECT_FALSE(orch.on_epic_registered(subject, epic().dump(), true).empty());
  EXPECT_EQ(gh->created_issues.size(), 2u);
}

TEST(DurableEpics, UncertainPublicationBeyondDedupWindowRequiresReconciliation) {
  auto gh = std::make_shared<EpicGitHub>();
  gh->fail_update = 2;  // pending intent persisted; publication receipt write fails
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  Orchestrator orch(store, bus);
  EXPECT_THROW(orch.on_epic_registered(subject, epic().dump(), true), std::runtime_error);
  ASSERT_EQ(bus.calls.size(), 1u);
  auto task = store.list_hmas_tasks_by_layer(HmasLayer::L0_ChiefArchitect).at(0);
  auto delivery = task.delivery;
  delivery["dispatch"]["attemptedAt"] = 0;
  ASSERT_TRUE(store.update_hmas_delivery(task.id, task.delivery, delivery));
  EXPECT_THROW(orch.on_epic_registered(subject, epic().dump(), true), std::invalid_argument);
  EXPECT_EQ(bus.calls.size(), 1u);
}

TEST(DurableEpics, CanonicalChildCompletionWakesParentOnceWithoutCompletingIt) {
  auto gh = std::make_shared<EpicGitHub>();
  FakeNatsPublisher bus;
  Store store(gh, epic_configuration());
  Orchestrator orch(store, bus);
  const auto brief = orch.on_epic_registered(subject, epic().dump(), true);
  auto parent = store.list_hmas_tasks_by_brief(brief).at(0);
  HmasTask child;
  child.id = "reviewed-child";
  child.brief_id = brief;
  child.parent_task_id = parent.id;
  child.layer = HmasLayer::L3_TaskAgent;
  child.state = TaskState::InProgress;
  store.create_hmas_task(child);
  bus.clear();
  orch.reconcile_parent_wakeups();
  EXPECT_TRUE(bus.calls.empty());
  child.state = TaskState::Completed;
  child.completed_at = now_iso8601();
  ASSERT_TRUE(store.update_hmas_task(child));
  orch.reconcile_parent_wakeups();
  ASSERT_EQ(bus.calls.size(), 1u);
  EXPECT_EQ(json::parse(bus.calls[0].payload)["operation"], "child_completed");
  EXPECT_EQ(store.get_hmas_task(parent.id)->state, TaskState::Decomposing);
  Store restarted(gh, epic_configuration());
  Orchestrator after_restart(restarted, bus);
  after_restart.reconcile_parent_wakeups();
  EXPECT_EQ(bus.calls.size(), 1u);
}
}  // namespace agamemnon::test
