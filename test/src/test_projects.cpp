#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/fleet.hpp"
#include "agamemnon/hmas_types.hpp"
#include "agamemnon/projects.hpp"
#include "agamemnon/store.hpp"

#include <map>

#include <gtest/gtest.h>

namespace agamemnon::test {
namespace {
json config() {
  json options;
  for (const auto* state :
       {"Pending", "Decomposing", "Delegated", "InProgress", "Escalated", "Completed", "Failed"})
    options[state] = "test-option-" + std::string(state);
  return {{"schema", "hi/projects-projection/v1"},
          {"projectId", "test-project"},
          {"stateFieldId", "test-state-field"},
          {"stateOptions", options}};
}
json connection(json nodes) {
  return {{"nodes", nodes}, {"pageInfo", {{"hasNextPage", false}, {"endCursor", nullptr}}}};
}
class ProjectGitHub : public MockGitHubClient {
 public:
  bool fail_updates = false;
  bool fail_after_update = false;
  bool fail_work_read = false;
  bool paginate = false;
  int mutations = 0;
  std::map<std::string, json> items;
  std::vector<json> requests;
  json labels = json::array({{{"name", "state:review"}}});

  void seed() {
    HmasTask task{};
    task.id = "task-1";
    task.layer = HmasLayer::L3_TaskAgent;
    task.state = TaskState::InProgress;
    task.repo = "example/work";
    task.issue = 7;
    seed_issues["agamemnon-hmas-task"] = {
        {{"number", 20},
         {"node_id", "test-issue-node"},
         {"html_url", "https://github.com/example/control/issues/20"},
         {"body", "## AgamemnonEntity: hmas-tasks/task-1\n\n```json\n" +
                      hmas_task_to_json(task).dump() + "\n```\n"}}};
  }
  json graphql(const std::string& query, const json& variables) override {
    requests.push_back({{"query", query}, {"variables", variables}});
    if (query.find("FleetProjectFields") != std::string::npos) {
      json options = json::array();
      auto configured = config();
      for (const auto& [state, id] : configured["stateOptions"].items())
        options.push_back({{"id", id}});
      return {{"node",
               {{"id", "test-project"},
                {"fields", connection(json::array(
                               {{{"__typename", "ProjectV2SingleSelectField"},
                                 {"id", "test-state-field"},
                                 {"options", options}},
                                {{"__typename", "ProjectV2SingleSelectField"},
                                 {"id", "test-stage-field"},
                                 {"options", json::array({{{"id", "test-review-option"}}})}},
                                {{"__typename", "ProjectV2Field"},
                                 {"id", "test-links-field"},
                                 {"dataType", "TEXT"}}}))}}}};
    }
    if (query.find("FleetProjectItems") != std::string::npos) {
      if (paginate && variables.at("after").is_null())
        return {{"node",
                 {{"items",
                   {{"nodes", json::array()},
                    {"pageInfo", {{"hasNextPage", true}, {"endCursor", "next-page"}}}}}}}};
      json nodes = json::array();
      for (const auto& [content, item] : items) nodes.push_back(item);
      return {{"node", {{"items", connection(nodes)}}}};
    }
    if (query.find("FleetWorkIssue") != std::string::npos) {
      if (fail_work_read) throw std::runtime_error("private diagnostic must not leave client");
      return {
          {"repository",
           {{"issue",
             {{"url", "https://github.com/example/work/issues/7"},
              {"labels", connection(labels)},
              {"closedByPullRequestsReferences",
               connection(json::array({{{"url", "https://github.com/example/work/pull/8"}}}))}}}}}};
    }
    if (query.find("FleetProjectClear") != std::string::npos) {
      ++mutations;
      auto& nodes = items.begin()->second["fieldValues"]["nodes"];
      for (auto it = nodes.begin(); it != nodes.end();)
        if ((*it)["field"]["id"] == variables.at("fieldId"))
          it = nodes.erase(it);
        else
          ++it;
      return {{"clearProjectV2ItemFieldValue", {{"projectV2Item", {{"id", "test-item"}}}}}};
    }
    if (query.find("FleetProjectAdd") != std::string::npos) {
      ++mutations;
      const auto content = variables.at("contentId").get<std::string>();
      items[content] = {{"id", "test-item"},
                        {"content", {{"id", content}}},
                        {"fieldValues", connection(json::array())},
                        {"isArchived", false}};
      return {{"addProjectV2ItemById", {{"item", {{"id", "test-item"}}}}}};
    }
    if (query.find("FleetProjectUpdate") != std::string::npos) {
      if (fail_updates) throw std::runtime_error("simulated projection failure");
      ++mutations;
      const auto field = variables.at("fieldId");
      auto value = variables.at("value");
      json node = {{"field", {{"id", field}}}};
      if (value.contains("singleSelectOptionId")) node["optionId"] = value["singleSelectOptionId"];
      if (value.contains("text")) node["text"] = value["text"];
      auto& nodes = items.begin()->second["fieldValues"]["nodes"];
      bool replaced = false;
      for (auto& prior : nodes)
        if (prior["field"]["id"] == field) {
          prior = node;
          replaced = true;
        }
      if (!replaced) nodes.push_back(node);
      if (fail_after_update) throw std::runtime_error("response lost after projection commit");
      return {{"updateProjectV2ItemFieldValue", {{"projectV2Item", {{"id", "test-item"}}}}}};
    }
    throw std::runtime_error("unexpected GraphQL operation");
  }
};
}  // namespace

TEST(ProjectsProjection, ConfiguredMappingRepairsBoardWithoutMutatingIssues) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  ProjectProjection projection(github, config());
  auto status = projection.reconcile();
  EXPECT_EQ(status["state"], "healthy");
  ASSERT_EQ(github->items.size(), 1u);
  EXPECT_EQ(github->items.begin()->second["fieldValues"]["nodes"][0]["optionId"],
            "test-option-InProgress");
  EXPECT_TRUE(github->updated_bodies.empty());
  EXPECT_TRUE(github->created_issues.empty());
  auto mutations = github->mutations;
  ProjectProjection restarted(github, config());
  EXPECT_EQ(restarted.reconcile()["state"], "healthy");
  EXPECT_EQ(github->mutations, mutations);
}

TEST(ProjectsProjection, ProjectionFailureIsVisibleAndRetryableAfterUncertainCommit) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  ProjectProjection projection(github, config());
  github->fail_after_update = true;
  auto failed = projection.reconcile();
  EXPECT_EQ(failed["state"], "degraded");
  EXPECT_EQ(failed["failed"], 1);
  auto mutations = github->mutations;
  github->fail_after_update = false;
  EXPECT_EQ(projection.reconcile()["state"], "healthy");
  EXPECT_EQ(github->mutations, mutations);
  EXPECT_TRUE(github->updated_bodies.empty());
}

TEST(ProjectsProjection, StageUsesActualLabelsAndKeepsWorkAndPullRequestLinks) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  auto configuration = config();
  configuration["stageFieldId"] = "test-stage-field";
  configuration["stageOptions"] = {{"state:review", "test-review-option"}};
  configuration["workLinksFieldId"] = "test-links-field";
  ProjectProjection projection(github, configuration);
  auto status = projection.reconcile();
  ASSERT_EQ(status["state"], "healthy");
  EXPECT_EQ(status["items"][0]["stageLabel"], "state:review");
  EXPECT_EQ(status["stageProjection"], "available");
  EXPECT_EQ(status["items"][0]["workIssueUrl"], "https://github.com/example/work/issues/7");
  EXPECT_EQ(status["items"][0]["pullRequestUrls"][0], "https://github.com/example/work/pull/8");
  EXPECT_TRUE(github->updated_bodies.empty());
  github->labels = json::array({{{"name", "state:review"}}, {{"name", "state:implementation"}}});
  status = projection.reconcile();
  EXPECT_EQ(status["items"][0]["stageProjection"], "unavailable");
  EXPECT_EQ(status["state"], "degraded");
  for (const auto& field : github->items.begin()->second["fieldValues"]["nodes"])
    EXPECT_NE(field["field"]["id"], "test-stage-field");
}

TEST(ProjectsProjection, RebuildPagesBeforeAddingAndRepairsExternalBoardDrift) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  ProjectProjection projection(github, config());
  ASSERT_EQ(projection.reconcile()["state"], "healthy");
  auto count = github->mutations;
  github->paginate = true;
  EXPECT_EQ(projection.reconcile()["state"], "healthy");
  EXPECT_EQ(github->mutations, count);
  github->items.begin()->second["fieldValues"]["nodes"][0]["optionId"] = "test-option-Completed";
  EXPECT_EQ(projection.reconcile()["state"], "healthy");
  EXPECT_EQ(github->mutations, count + 1);
  EXPECT_EQ(github->items.begin()->second["fieldValues"]["nodes"][0]["optionId"],
            "test-option-InProgress");
  EXPECT_TRUE(github->updated_bodies.empty());
}

TEST(ProjectsProjection, WorkMetadataFailureRemainsVisibleWithoutBlockingCanonicalStateProjection) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  github->fail_work_read = true;
  ProjectProjection projection(github, config());
  auto status = projection.reconcile();
  EXPECT_EQ(status["state"], "degraded");
  EXPECT_EQ(status["unavailable"], 1);
  EXPECT_EQ(status["retryable"], true);
  EXPECT_EQ(github->items.begin()->second["fieldValues"]["nodes"][0]["optionId"],
            "test-option-InProgress");
  EXPECT_EQ(status.dump().find("private diagnostic"), std::string::npos);
}

TEST(ProjectsProjection, MissingConfigurationOrBackingStoreCannotMutateProjects) {
  auto github = std::make_shared<ProjectGitHub>();
  auto invalid = config();
  invalid["stateOptions"]["Pending"] = "unconfigured-option";
  ProjectProjection projection(github, invalid);
  EXPECT_EQ(projection.reconcile()["state"], "misconfigured");
  EXPECT_EQ(github->mutations, 0);
  ProjectProjection missing(nullptr, config());
  EXPECT_EQ(missing.reconcile()["state"], "unavailable");
}

TEST(ProjectsProjection, FailedRebuildDoesNotClaimFreshStageAvailabilityFromPriorPass) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  auto configured = config();
  configured["stageFieldId"] = "test-stage-field";
  configured["stageOptions"] = {{"state:review", "test-review-option"}};
  ProjectProjection projection(github, configured);
  ASSERT_EQ(projection.reconcile()["stageProjection"], "available");
  github->fail_list_on_label = "agamemnon-hmas-task";
  auto result = projection.reconcile();
  EXPECT_EQ(result["state"], "degraded");
  EXPECT_EQ(result["stageProjection"], "unavailable");
}

TEST(ProjectsProjection, FailedDerivedBoardDoesNotChangeDurableAdmissionAuthority) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  github->fail_updates = true;
  auto projection = std::make_shared<ProjectProjection>(github, config());
  ASSERT_EQ(projection->reconcile()["state"], "degraded");
  Store store(github);
  FakeNatsPublisher publisher;
  FleetService fleet(store, publisher, nullptr, "", projection);
  fleet.create("pools", {{"id", "pool"}, {"capacity", 1}});
  fleet.create("workers", {{"id", "worker"}, {"poolId", "pool"}, {"capacity", 1}});
  fleet.create("sessions", {{"id", "session"},
                            {"workerId", "worker"},
                            {"agentId", "agent"},
                            {"workspace", "/work/agent"}});
  auto result = fleet.command("sessions", "session", "start",
                              {{"commandId", "start"},
                               {"idempotencyKey", "start"},
                               {"generation", 1},
                               {"payload", json::object()}});
  EXPECT_EQ(result["status"], "pending");
  EXPECT_FALSE(github->updated_bodies.empty());
  EXPECT_EQ(fleet.projects_health()["state"], "degraded");
  EXPECT_EQ(store.get_hmas_task("task-1")->state, TaskState::InProgress);
}

TEST(ProjectsProjection, MalformedCanonicalRecordPreventsPartialBoardWrites) {
  auto github = std::make_shared<ProjectGitHub>();
  github->seed();
  github->seed_issues["agamemnon-hmas-task"].push_back({{"body", "malformed"}});
  ProjectProjection projection(github, config());
  EXPECT_EQ(projection.reconcile()["state"], "degraded");
  EXPECT_EQ(github->mutations, 0);
}

TEST(ProjectsGraphQL, TransportAndGraphQLErrorsAreNotSuccessfulAcknowledgments) {
  class ResponseClient : public CurlGitHubClient {
   public:
    ResponseClient() : CurlGitHubClient("example/control", "test-token") {}
    Response response{200, R"({"data":{"ok":true}})", ""};

   protected:
    Response do_post(const std::string&, const std::string&) const override { return response; }
  } client;
  EXPECT_EQ(client.graphql("query{viewer{login}}", json::object())["ok"], true);
  client.response.body = R"({"data":{"ok":true},"errors":[{"message":"denied"}]})";
  EXPECT_THROW(client.graphql("query{viewer{login}}", json::object()), std::runtime_error);
  client.response = {503, "{}", ""};
  EXPECT_THROW(client.graphql("query{viewer{login}}", json::object()), std::runtime_error);
}
}  // namespace agamemnon::test
