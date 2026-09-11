#include "agamemnon/github_client.hpp"
#include "agamemnon/nats_client.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/store.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <nats.h>
#include <thread>

#include <gtest/gtest.h>

namespace agamemnon::test {
using namespace std::chrono_literals;
static bool wait_for(const std::function<bool()>& predicate) {
  for (int i = 0; i < 100; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}
class PrivateJetStream : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* configured = std::getenv("AGAMEMNON_TEST_NATS_URL");
    ASSERT_NE(configured, nullptr) << "Use just fleet-jetstream-test; no default broker allowed";
    url = configured;
    ASSERT_TRUE(url.starts_with("nats://127.0.0.1:"));
    ASSERT_EQ(natsConnection_ConnectTo(&connection, url.c_str()), NATS_OK);
    ASSERT_EQ(natsConnection_JetStream(&js, connection, nullptr), NATS_OK);
  }
  void TearDown() override {
    jsCtx_Destroy(js);
    natsConnection_Destroy(connection);
  }
  std::string url;
  natsConnection* connection = nullptr;
  jsCtx* js = nullptr;
};
TEST_F(PrivateJetStream, OfflineReplayRetryAndDurableAcknowledgment) {
  const std::string subject = "hi.pipeline.epic.offline.registered";
  NatsClient publisher(url);
  ASSERT_TRUE(publisher.connect());
  publisher.ensure_streams(true);
  ASSERT_TRUE(publisher.publish_durable(subject, "{}", "offline-1"));
  std::atomic<int> calls{0};
  {
    NatsClient first(url);
    ASSERT_TRUE(first.connect());
    ASSERT_TRUE(first.subscribe_durable(
        "homeric-pipeline", subject, "test-offline",
        [&](auto&, auto&) {
          if (++calls == 1) throw std::runtime_error("GitHub write unavailable");
        },
        25));
    ASSERT_TRUE(wait_for([&] {
      jsConsumerInfo* info = nullptr;
      if (js_GetConsumerInfo(&info, js, "homeric-pipeline", "test-offline", nullptr, nullptr) !=
          NATS_OK)
        return false;
      const bool acked = calls == 2 && info->NumAckPending == 0 && info->AckFloor.Stream > 0;
      jsConsumerInfo_Destroy(info);
      return acked;
    }));
  }
  NatsClient second(url);
  ASSERT_TRUE(second.connect());
  ASSERT_TRUE(second.subscribe_durable(
      "homeric-pipeline", subject, "test-offline", [&](auto&, auto&) { ++calls; }, 25));
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(calls, 2);
}
TEST_F(PrivateJetStream, InvalidInputRequiresDurableQuarantine) {
  const std::string subject = "hi.pipeline.epic.invalid.registered";
  NatsClient client(url);
  ASSERT_TRUE(client.connect());
  client.ensure_streams(true);
  std::atomic<int> calls{0};
  ASSERT_TRUE(client.subscribe_durable(
      "homeric-pipeline", subject, "test-invalid",
      [&](auto&, auto&) {
        ++calls;
        throw std::invalid_argument("invalid_epic_envelope");
      },
      25));
  ASSERT_TRUE(client.publish_durable(subject, "malformed", "invalid-1"));
  ASSERT_TRUE(wait_for([&] {
    natsMsg* message = nullptr;
    auto status = js_GetLastMsg(&message, js, "homeric-pipeline",
                                "hi.pipeline.quarantine.test-invalid", nullptr, nullptr);
    if (status != NATS_OK) return false;
    auto value = nlohmann::json::parse(natsMsg_GetData(message),
                                       natsMsg_GetData(message) + natsMsg_GetDataLength(message));
    natsMsg_Destroy(message);
    return value["status"] == "quarantined" && value["sourceSubject"] == subject &&
           value["reason"] == "invalid_input";
  }));
  EXPECT_EQ(calls, 1);
}
TEST_F(PrivateJetStream, StrictPublishNeverFallsBackAndDeduplicates) {
  NatsClient client(url);
  ASSERT_TRUE(client.connect());
  client.ensure_streams(true);
  EXPECT_FALSE(client.publish_durable("no.stream", "{}", "missing-stream"));
  EXPECT_TRUE(client.publish_durable("hi.pipeline.epic.dedup.registered", "{}", "same-identity"));
  EXPECT_TRUE(client.publish_durable("hi.pipeline.epic.dedup.registered", "{}", "same-identity"));
}

class QuarantineUnavailable : public NatsClient {
 public:
  using NatsClient::NatsClient;
  bool publish_durable(const std::string& subject, const std::string& payload,
                       const std::string& id) override {
    return !subject.starts_with("hi.pipeline.quarantine.") &&
           NatsClient::publish_durable(subject, payload, id);
  }
};
TEST_F(PrivateJetStream, FailedQuarantineLeavesDeliveryUnacknowledgedAtBound) {
  QuarantineUnavailable client(url);
  ASSERT_TRUE(client.connect());
  client.ensure_streams(true);
  std::atomic<int> calls{0};
  const std::string subject = "hi.pipeline.epic.quarantine-unavailable.registered";
  ASSERT_TRUE(client.subscribe_durable(
      "homeric-pipeline", subject, "test-quarantine-unavailable",
      [&](auto&, auto&) {
        ++calls;
        throw std::invalid_argument("bad input");
      },
      25));
  ASSERT_TRUE(client.publish_durable(subject, "{}", "quarantine-unavailable-1"));
  ASSERT_TRUE(wait_for([&] { return calls == 3; }));
  std::this_thread::sleep_for(200ms);
  jsConsumerInfo* info = nullptr;
  ASSERT_EQ(js_GetConsumerInfo(&info, js, "homeric-pipeline", "test-quarantine-unavailable",
                               nullptr, nullptr),
            NATS_OK);
  // Processing is bounded, while transport can still redeliver until the
  // quarantine persistence acknowledgment is available.
  EXPECT_EQ(info->NumAckPending, 1);
  EXPECT_EQ(info->AckFloor.Stream, 0u);
  natsMsg* retained = nullptr;
  ASSERT_EQ(js_GetMsg(&retained, js, "homeric-pipeline", info->Delivered.Stream, nullptr, nullptr),
            NATS_OK);
  EXPECT_STREQ(natsMsg_GetSubject(retained), subject.c_str());
  natsMsg_Destroy(retained);
  EXPECT_EQ(calls, 3);
  jsConsumerInfo_Destroy(info);
  client.close();
  NatsClient recovered(url);
  ASSERT_TRUE(recovered.connect());
  std::atomic<int> extra_processing{0};
  ASSERT_TRUE(recovered.subscribe_durable(
      "homeric-pipeline", subject, "test-quarantine-unavailable",
      [&](auto&, auto&) {
        ++extra_processing;
        throw std::invalid_argument("bad input");
      },
      25));
  ASSERT_TRUE(wait_for([&] {
    natsMsg* quarantine = nullptr;
    if (js_GetLastMsg(&quarantine, js, "homeric-pipeline",
                      "hi.pipeline.quarantine.test-quarantine-unavailable", nullptr,
                      nullptr) != NATS_OK)
      return false;
    natsMsg_Destroy(quarantine);
    return true;
  }));
  EXPECT_EQ(extra_processing, 0);
}

TEST_F(PrivateJetStream, EphemeralOrEvictingStreamCannotBackDurableConsumer) {
  const char* subjects[] = {"hi.retention.>"};
  jsStreamConfig config;
  jsStreamConfig_Init(&config);
  config.Name = "test-retention";
  config.Subjects = subjects;
  config.SubjectsLen = 1;
  config.Storage = js_FileStorage;
  config.Retention = js_LimitsPolicy;
  config.MaxAge = 3600000000000LL;
  config.Duplicates = 120000000000LL;
  ASSERT_EQ(js_AddStream(nullptr, js, &config, nullptr, nullptr), NATS_OK);
  NatsClient client(url);
  ASSERT_TRUE(client.connect());
  EXPECT_FALSE(client.subscribe_durable("test-retention", "hi.retention.>", "unsafe-retention",
                                        [](auto&, auto&) {}));
}

TEST_F(PrivateJetStream, IncompatibleConsumerIsNotSilentlyReconfigured) {
  NatsClient client(url);
  ASSERT_TRUE(client.connect());
  client.ensure_streams(true);
  jsConsumerConfig config;
  jsConsumerConfig_Init(&config);
  config.Durable = "test-incompatible";
  config.FilterSubject = "hi.pipeline.epic.incompatible.registered";
  config.AckPolicy = js_AckNone;
  ASSERT_EQ(js_AddConsumer(nullptr, js, "homeric-pipeline", &config, nullptr, nullptr), NATS_OK);
  EXPECT_FALSE(client.subscribe_durable("homeric-pipeline", config.FilterSubject, config.Durable,
                                        [](auto&, auto&) {}));
}

class AuthorityFixture : public MockGitHubClient {
 public:
  bool reject_first_create = true;
  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    if (reject_first_create) {
      reject_first_create = false;
      return "";
    }
    return MockGitHubClient::create_issue(title, body, label);
  }
  std::vector<json> list_issues_including_closed(std::string_view label) override {
    std::vector<json> result;
    for (auto& [id, issue] : created_issues)
      if (issue["label"] == label)
        result.push_back({{"number", std::stoi(id)}, {"body", issue["body"]}});
    return result;
  }
};
TEST_F(PrivateJetStream, RealBrokerEpicReplayAndCanonicalParentWakeWithFixtureAuthority) {
  NatsClient publisher(url);
  ASSERT_TRUE(publisher.connect());
  publisher.ensure_streams(true);
  const std::string subject = "hi.pipeline.epic.homeric-composite-42.registered";
  json envelope = {
      {"schema", "hi/v1"},
      {"msg_id", "composite-1"},
      {"workflow", "feature"},
      {"epic", {{"repo", "Homeric/composite"}, {"issue", 42}, {"key", "homeric-composite-42"}}},
      {"children", {43}}};
  ASSERT_TRUE(publisher.publish_durable(subject, envelope.dump(), "composite-1"));
  auto gh = std::make_shared<AuthorityFixture>();
  std::string brief_id;
  std::atomic<int> calls{0};
  {
    Store store(gh);
    NatsClient transport(url);
    Orchestrator orch(store, transport);
    ASSERT_TRUE(transport.connect());
    ASSERT_TRUE(transport.subscribe_durable(
        "homeric-pipeline", subject, "test-composite",
        [&](auto& incoming, auto& data) {
          ++calls;
          brief_id = orch.on_epic_registered(incoming, data, true);
        },
        25));
    ASSERT_TRUE(wait_for([&] {
      jsConsumerInfo* info = nullptr;
      if (js_GetConsumerInfo(&info, js, "homeric-pipeline", "test-composite", nullptr, nullptr) !=
          NATS_OK)
        return false;
      bool ready = calls == 2 && info->NumAckPending == 0 && info->AckFloor.Stream > 0;
      jsConsumerInfo_Destroy(info);
      return ready;
    }));
    transport.close();
  }
  ASSERT_FALSE(brief_id.empty());
  EXPECT_EQ(gh->created_issues.size(), 2u);
  Store restarted(gh);
  Orchestrator orch(restarted, publisher);
  auto parent = restarted.list_hmas_tasks_by_brief(brief_id).at(0);
  envelope["msg_id"] = "composite-2";
  envelope["epic"]["repo"] = "homeric/COMPOSITE";
  EXPECT_EQ(orch.on_epic_registered(subject, envelope.dump(), true), brief_id);
  HmasTask child;
  child.id = "composite-reviewed-child";
  child.brief_id = brief_id;
  child.parent_task_id = parent.id;
  child.layer = HmasLayer::L3_TaskAgent;
  child.state = TaskState::Completed;
  child.completed_at = now_iso8601();
  restarted.create_hmas_task(
      child);  // fake GitHub acknowledged canonical outcome, not a worker fact
  orch.reconcile_parent_wakeups();
  natsMsg* message = nullptr;
  const auto dispatch_subject = mesh_dispatch_subject("pipeline", "chief-architect", parent.id);
  ASSERT_EQ(
      js_GetLastMsg(&message, js, "homeric-myrmidon", dispatch_subject.c_str(), nullptr, nullptr),
      NATS_OK);
  auto wake = json::parse(natsMsg_GetData(message),
                          natsMsg_GetData(message) + natsMsg_GetDataLength(message));
  natsMsg_Destroy(message);
  EXPECT_EQ(wake["operation"], "child_completed");
  EXPECT_EQ(wake["completed_child_id"], child.id);
  EXPECT_EQ(restarted.get_hmas_task(parent.id)->state, TaskState::Decomposing);
  Store final_store(gh);
  Orchestrator final_orch(final_store, publisher);
  EXPECT_NO_THROW(final_orch.reconcile_parent_wakeups());
}
}  // namespace agamemnon::test
