#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/github_client.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/store.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <mutex>

#include <gtest/gtest.h>

namespace agamemnon::test {
class EpicGitHub : public MockGitHubClient {
 public:
  bool fail_create = false;
  bool lose_create_response = false;
  int fail_update = 0;
  int updates = 0;
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
  Store store(gh);
  Orchestrator first(store, bus);
  const auto id = first.on_epic_registered(subject, epic().dump(), true);
  ASSERT_FALSE(id.empty());
  Store restarted(gh);
  Orchestrator second(restarted, bus);
  EXPECT_EQ(second.on_epic_registered(subject, epic().dump(), true), id);
  EXPECT_EQ(gh->created_issues.size(), 2u);
  EXPECT_EQ(bus.calls.size(), 1u);
}

TEST(DurableEpics, FailedBriefWriteCannotPublishOrCache) {
  auto gh = std::make_shared<EpicGitHub>();
  gh->fail_create = true;
  FakeNatsPublisher bus;
  Store store(gh);
  Orchestrator orch(store, bus);
  EXPECT_THROW(orch.on_epic_registered(subject, epic().dump(), true), std::runtime_error);
  EXPECT_TRUE(bus.calls.empty());
  EXPECT_TRUE(store.list_task_briefs().empty());
}

TEST(DurableEpics, MemoryOnlyAndSubjectMismatchAreRejected) {
  FakeNatsPublisher bus;
  Store memory;
  Orchestrator orch(memory, bus);
  EXPECT_THROW(orch.on_epic_registered(subject, epic().dump(), true), std::runtime_error);
  auto gh = std::make_shared<EpicGitHub>();
  Store store(gh);
  Orchestrator durable(store, bus);
  EXPECT_THROW(durable.on_epic_registered("hi.pipeline.epic.other.registered", epic().dump(), true),
               std::invalid_argument);
  EXPECT_TRUE(gh->created_issues.empty());
}

TEST(DurableEpics, ChangedMessageIdReplaysButChangedPlanConflicts) {
  auto gh = std::make_shared<EpicGitHub>();
  FakeNatsPublisher bus;
  Store store(gh);
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
  Store store(gh);
  Orchestrator first(store, bus);
  auto original = epic();
  original["workflow"] = "BuildFeature";
  const auto id = first.on_epic_registered(subject, original.dump(), true);
  Store restarted(gh);
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
  Store store(gh);
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
  Store restarted(gh);
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
  Store store(gh);
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
  Store store(gh);
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
  Store store(gh);
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
  Store restarted(gh);
  Orchestrator after_restart(restarted, bus);
  after_restart.reconcile_parent_wakeups();
  EXPECT_EQ(bus.calls.size(), 1u);
}

class ConcurrentEpicBus : public FakeNatsPublisher {
 public:
  std::function<void()> before_wake;
  std::function<void()> record_wake;

  bool publish(const std::string& channel, const std::string& payload) override {
    const bool wake = json::parse(payload).value("operation", "") == "child_completed";
    if (wake && before_wake) before_wake();
    std::lock_guard lock(mutex_);
    if (wake && record_wake) record_wake();
    return FakeNatsPublisher::publish(channel, payload);
  }

  void publish_log(const std::string& channel, const std::string& level, const std::string& message,
                   const json& metadata) override {
    std::lock_guard lock(mutex_);
    FakeNatsPublisher::publish_log(channel, level, message, metadata);
  }

 private:
  std::mutex mutex_;
};

enum class ParentMutation { Started, Assigned, Completed };
class ParentWakeupRace : public ::testing::TestWithParam<ParentMutation> {};

struct WakeupOrder {
  std::mutex mutex;
  int sequence = 0;
  int mutation = 0;
  int publication = 0;
};

class OrderedEpicGitHub : public EpicGitHub {
 public:
  explicit OrderedEpicGitHub(WakeupOrder& order) : order_(order) {}
  std::string parent_issue;
  void update_issue_body(std::string_view id, std::string_view body) override {
    if (id != parent_issue) {
      EpicGitHub::update_issue_body(id, body);
      return;
    }
    // The fixture commit and its order record share the publication trace lock.
    // Preemption after this write cannot let the publication appear earlier.
    std::lock_guard lock(order_.mutex);
    EpicGitHub::update_issue_body(id, body);
    order_.mutation = ++order_.sequence;
  }

 private:
  WakeupOrder& order_;
};

TEST_P(ParentWakeupRace, CanonicalMutationCannotCommitDuringWakePublication) {
  WakeupOrder order;
  auto gh = std::make_shared<OrderedEpicGitHub>(order);
  ConcurrentEpicBus bus;
  Store store(gh);
  Orchestrator orch(store, bus);
  const auto brief = orch.on_epic_registered(subject, epic().dump(), true);
  const auto parent = store.list_hmas_tasks_by_brief(brief).at(0);
  for (const auto& [id, issue] : gh->created_issues)
    if (issue["title"] == "hmas-task: " + parent.id) gh->parent_issue = id;
  ASSERT_FALSE(gh->parent_issue.empty());
  HmasTask child;
  child.id = "completed-race-child";
  child.brief_id = brief;
  child.parent_task_id = parent.id;
  child.layer = HmasLayer::L3_TaskAgent;
  child.state = TaskState::Completed;
  child.completed_at = now_iso8601();
  store.create_hmas_task(child);
  bus.clear();

  std::future<bool> mutation;
  bus.before_wake = [&] {
    mutation = std::async(std::launch::async, [&] {
      bool changed = false;
      if (GetParam() == ParentMutation::Started) {
        orch.on_myrmidon_started(
            "hi.pipeline.chief-architect.started",
            json{{"task_id", parent.id}, {"agent_id", "planner"}, {"exec_host", "worker-1"}}
                .dump());
        const auto actual = store.get_hmas_task(parent.id);
        changed = actual && actual->state == TaskState::InProgress &&
                  actual->assigned_lead_id == "planner@worker-1";
      } else {
        auto updated = parent;
        if (GetParam() == ParentMutation::Assigned)
          updated.assigned_lead_id = "planner@worker-1";
        else
          updated.state = TaskState::Completed;
        changed = store.update_hmas_task(updated);
      }
      return changed;
    });
    // Give the competing operation a finite opportunity to finish. The oracle
    // is the actual committed mutation/publication order, not this wait result.
    mutation.wait_for(std::chrono::milliseconds(500));
  };
  bus.record_wake = [&] {
    std::lock_guard lock(order.mutex);
    order.publication = ++order.sequence;
  };
  orch.reconcile_parent_wakeups();
  ASSERT_TRUE(mutation.valid());
  ASSERT_TRUE(mutation.get());
  ASSERT_GT(order.publication, 0);
  ASSERT_GT(order.mutation, 0);
  ASSERT_EQ(store.get_hmas_task(child.id)->delivery["parentWake"]["phase"], "published");
  std::cout << "parent wake sequence: publish=" << order.publication
            << " canonical mutation=" << order.mutation << '\n';
  EXPECT_LT(order.publication, order.mutation)
      << "A parent became assigned or ineligible before its wake was published";
}

INSTANTIATE_TEST_SUITE_P(CanonicalWriters, ParentWakeupRace,
                         ::testing::Values(ParentMutation::Started, ParentMutation::Assigned,
                                           ParentMutation::Completed),
                         [](const auto& info) {
                           switch (info.param) {
                             case ParentMutation::Started:
                               return "Started";
                             case ParentMutation::Assigned:
                               return "Assigned";
                             case ParentMutation::Completed:
                               return "Completed";
                           }
                           return "Unknown";
                         });

class ReplayEpicBus : public FakeNatsPublisher {
 public:
  bool lose_ack = false;
  bool publish_durable(const std::string& channel, const std::string& payload,
                       const std::string& message_id) override {
    FakeNatsPublisher::publish_durable(channel, payload, message_id);
    return !std::exchange(lose_ack, false);
  }
};

TEST(DurableEpics, PendingWakeRejectsChangedCanonicalSnapshots) {
  using Change = std::function<void(Store&, HmasTask&, HmasTask&, HmasTask&)>;
  const std::vector<std::pair<std::string, Change>> changes = {
      {"child state",
       [](auto& store, auto& child, auto&, auto&) {
         child.state = TaskState::Failed;
         ASSERT_TRUE(store.update_hmas_task(child));
       }},
      {"child parent",
       [](auto& store, auto& child, auto&, auto&) {
         child.parent_task_id = "another-parent";
         ASSERT_TRUE(store.update_hmas_task(child));
       }},
      {"child completion",
       [](auto& store, auto& child, auto&, auto&) {
         child.completed_at = "another-completion";
         ASSERT_TRUE(store.update_hmas_task(child));
       }},
      {"child checkpoint",
       [](auto& store, auto& child, auto&, auto&) {
         auto changed = child.delivery;
         changed["parentWake"]["phase"] = "published";
         ASSERT_TRUE(store.update_hmas_delivery(child.id, child.delivery, changed));
       }},
      {"parent assignment",
       [](auto& store, auto&, auto& parent, auto&) {
         parent.assigned_lead_id = "other-planner";
         ASSERT_TRUE(store.update_hmas_task(parent));
       }},
      {"parent state",
       [](auto& store, auto&, auto& parent, auto&) {
         parent.state = TaskState::Completed;
         ASSERT_TRUE(store.update_hmas_task(parent));
       }},
      {"parent role",
       [](auto& store, auto&, auto& parent, auto&) {
         parent.layer = HmasLayer::L2_ModuleLead;
         ASSERT_TRUE(store.update_hmas_task(parent));
       }},
      {"durable root",
       [](auto& store, auto&, auto&, auto& root) {
         root.delivery.erase("registration");
         ASSERT_TRUE(store.update_hmas_task(root));
       }},
  };
  for (const auto& [name, change] : changes) {
    SCOPED_TRACE(name);
    auto gh = std::make_shared<EpicGitHub>();
    ReplayEpicBus bus;
    Store store(gh);
    Orchestrator orch(store, bus);
    const auto brief = orch.on_epic_registered(subject, epic().dump(), true);
    auto root = store.list_hmas_tasks_by_brief(brief).at(0);
    HmasTask parent;
    parent.id = "parked-parent";
    parent.parent_task_id = root.id;
    parent.brief_id = brief;
    parent.layer = HmasLayer::L1_ComponentLead;
    parent.state = TaskState::Delegated;
    store.create_hmas_task(parent);
    HmasTask child;
    child.id = "snapshot-child";
    child.parent_task_id = parent.id;
    child.brief_id = brief;
    child.layer = HmasLayer::L3_TaskAgent;
    child.state = TaskState::Completed;
    child.completed_at = now_iso8601();
    store.create_hmas_task(child);
    bus.clear();
    bus.lose_ack = true;
    EXPECT_THROW(orch.reconcile_parent_wakeups(), std::runtime_error);
    ASSERT_EQ(bus.calls.size(), 1u);
    const auto expected_child = *store.get_hmas_task(child.id);
    const auto expected_parent = *store.get_hmas_task(parent.id);
    child = expected_child;
    ASSERT_EQ(child.delivery["parentWake"]["phase"], "pending");
    change(store, child, parent, root);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    int publications = 0;
    EXPECT_FALSE(
        store.publish_hmas_parent_wakeup(expected_child, expected_parent, [&] { ++publications; }));
    EXPECT_EQ(publications, 0);
    EXPECT_EQ(bus.calls.size(), 1u);
  }
}

TEST(DurableEpics, ParentPublicationLossReplaysSameIntentAcrossRestart) {
  for (const bool broker_ack_lost : {true, false}) {
    SCOPED_TRACE(broker_ack_lost ? "broker acknowledgment" : "GitHub receipt");
    auto gh = std::make_shared<EpicGitHub>();
    ReplayEpicBus bus;
    Store store(gh);
    Orchestrator orch(store, bus);
    const auto brief = orch.on_epic_registered(subject, epic().dump(), true);
    const auto parent = store.list_hmas_tasks_by_brief(brief).at(0);
    HmasTask child;
    child.id = "replayed-child";
    child.parent_task_id = parent.id;
    child.brief_id = brief;
    child.layer = HmasLayer::L3_TaskAgent;
    child.state = TaskState::Completed;
    child.completed_at = now_iso8601();
    store.create_hmas_task(child);
    bus.clear();
    bus.lose_ack = broker_ack_lost;
    if (!broker_ack_lost) gh->fail_update = gh->updates + 2;
    EXPECT_THROW(orch.reconcile_parent_wakeups(), std::runtime_error);
    ASSERT_EQ(bus.calls.size(), 1u);
    Store restarted(gh);
    Orchestrator resumed(restarted, bus);
    ASSERT_EQ(restarted.get_hmas_task(child.id)->delivery["parentWake"]["phase"], "pending");
    resumed.reconcile_parent_wakeups();
    ASSERT_EQ(bus.calls.size(), 2u);
    EXPECT_EQ(bus.calls[0].subject, bus.calls[1].subject);
    EXPECT_EQ(bus.calls[0].payload, bus.calls[1].payload);
    EXPECT_EQ(restarted.get_hmas_task(child.id)->delivery["parentWake"]["phase"], "published");
    EXPECT_EQ(restarted.get_hmas_task(parent.id)->state, TaskState::Decomposing);
    resumed.reconcile_parent_wakeups();
    EXPECT_EQ(bus.calls.size(), 2u);
  }
}
}  // namespace agamemnon::test
