#include "agamemnon/github_client.hpp"
#include "agamemnon/nats_client.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/store.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nats.h>
#include <thread>

using namespace agamemnon;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
class Authority : public MockGitHubClient {
 public:
  bool reject_first_create = true;
  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    if (reject_first_create) {
      reject_first_create = false;
      throw std::runtime_error("fixture_unconfirmed_creation");
    }
    return MockGitHubClient::create_issue(title, body, label);
  }
  std::vector<json> list_issues_including_closed(std::string_view label) override {
    std::vector<json> result;
    for (const auto& [number, issue] : created_issues)
      if (issue["label"] == label)
        result.push_back({{"number", std::stoi(number)}, {"body", issue["body"]}});
    return result;
  }
};
class Transport : public NatsClient {
 public:
  using NatsClient::NatsClient;
  std::atomic<int> publications{0};
  bool publish_durable(const std::string& subject, const std::string& payload,
                       const std::string& id) override {
    ++publications;
    return NatsClient::publish_durable(subject, payload, id);
  }
};
struct BrokerInspection {
  natsConnection* connection = nullptr;
  jsCtx* js = nullptr;
  ~BrokerInspection() {
    jsCtx_Destroy(js);
    natsConnection_Destroy(connection);
  }
  std::uint64_t ack_floor(const std::string& consumer) const {
    jsConsumerInfo* info = nullptr;
    if (js_GetConsumerInfo(&info, js, "homeric-pipeline", consumer.c_str(), nullptr, nullptr) !=
        NATS_OK)
      return 0;
    auto sequence = info->NumAckPending == 0 ? info->AckFloor.Stream : 0;
    jsConsumerInfo_Destroy(info);
    return sequence;
  }
};
bool wait_for(const std::function<bool()>& predicate) {
  for (int i = 0; i < 150; ++i) {
    if (predicate()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}
std::string bytes(const natsMsg* message) {
  return {natsMsg_GetData(message), static_cast<std::size_t>(natsMsg_GetDataLength(message))};
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 3, "usage: fleet_epic_import CAPTURE_JSON LOOPBACK_NATS_URL");
    const std::string url = argv[2];
    const std::string prefix = "nats://127.0.0.1:";
    require(url.starts_with(prefix), "private loopback broker required");
    const auto port = url.substr(prefix.size());
    require(!port.empty() && port.find_first_not_of("0123456789") == std::string::npos,
            "invalid private broker port");
    std::ifstream input(argv[1]);
    const auto capture = json::parse(input);
    const auto subject = capture.at("subject").get<std::string>();
    const auto payload = capture.at("payload").get<std::string>();
    const auto envelope = json::parse(payload);
    const auto source_sequence = capture.at("receipt").at("seq").get<std::uint64_t>();
    BrokerInspection inspection;
    require(natsConnection_ConnectTo(&inspection.connection, url.c_str()) == NATS_OK,
            "private broker connection failed");
    require(natsConnection_JetStream(&inspection.js, inspection.connection, nullptr) == NATS_OK,
            "JetStream unavailable");
    natsMsg* raw = nullptr;
    require(js_GetMsg(&raw, inspection.js, "homeric-pipeline", source_sequence, nullptr, nullptr) ==
                NATS_OK,
            "producer message absent");
    const bool exact = subject == natsMsg_GetSubject(raw) && payload == bytes(raw);
    natsMsg_Destroy(raw);
    require(exact, "producer capture differs from actual broker bytes");

    auto github = std::make_shared<Authority>();
    std::string brief;
    const std::string consumer = "producer-native-contract";
    std::atomic<int> deliveries{0};
    std::atomic<bool> failed_write_fenced{false};
    {
      Store store(github);
      Transport transport(url);
      Orchestrator orchestrator(store, transport);
      require(transport.connect(), "consumer connection failed");
      transport.ensure_streams(true);
      require(transport.subscribe_durable(
                  "homeric-pipeline", subject, consumer,
                  [&](const auto& incoming, const auto& data) {
                    const int attempt = ++deliveries;
                    require(incoming == subject && data == payload, "consumer bytes changed");
                    try {
                      brief = orchestrator.on_epic_registered(incoming, data, true);
                    } catch (...) {
                      if (attempt == 1)
                        failed_write_fenced = github->created_issues.empty() &&
                                              transport.publications == 0 &&
                                              inspection.ack_floor(consumer) == 0;
                      throw;
                    }
                  },
                  25),
              "durable consumer attachment failed");
      require(wait_for([&] {
                return deliveries == 2 && inspection.ack_floor(consumer) >= source_sequence;
              }),
              "confirmed processing was not acknowledged");
      transport.close();
      require(failed_write_fenced, "failed GitHub write escaped the publication/ACK gate");
      require(transport.publications == 1, "unexpected initial dispatch count");
    }
    require(!brief.empty() && github->created_issues.size() == 2, "durable graph not unique");

    Transport publisher(url);
    require(publisher.connect(), "replay publisher connection failed");
    // Change only the transport dedup header to exercise receiver replay without
    // waiting for the broker window. The producer body remains byte-for-byte intact.
    require(publisher.publish_durable(subject, payload,
                                      envelope.at("msg_id").get<std::string>() + "-wire-replay"),
            "replay publication failed");
    Store restarted(github);
    Transport transport(url);
    Orchestrator orchestrator(restarted, transport);
    require(transport.connect(), "restarted consumer connection failed");
    std::atomic<int> replayed{0};
    require(transport.subscribe_durable(
                "homeric-pipeline", subject, consumer,
                [&](const auto& incoming, const auto& data) {
                  require(data == payload, "replayed producer bytes changed");
                  require(orchestrator.on_epic_registered(incoming, data, true) == brief,
                          "restart created a different graph");
                  ++replayed;
                },
                25),
            "restart durable attachment failed");
    require(
        wait_for([&] { return replayed == 1 && inspection.ack_floor(consumer) > source_sequence; }),
        "replay not durably acknowledged");
    transport.close();
    require(transport.publications == 0 && github->created_issues.size() == 2,
            "replay duplicated graph or dispatch");

    const auto parent = restarted.list_hmas_tasks_by_brief(brief).at(0);
    require(parent.repo == envelope["epic"]["repo"].get<std::string>() &&
                parent.issue == envelope["epic"]["issue"].get<int>(),
            "canonical issue identity differs from producer");
    HmasTask child{};
    child.id = "producer-contract-reviewed-child";
    child.brief_id = brief;
    child.parent_task_id = parent.id;
    child.layer = HmasLayer::L3_TaskAgent;
    child.state = TaskState::Completed;
    child.completed_at = now_iso8601();
    restarted.create_hmas_task(child);  // Controlled canonical decision, not worker completion.
    Orchestrator wakeup(restarted, publisher);
    wakeup.reconcile_parent_wakeups();
    const auto dispatch = mesh_dispatch_subject("pipeline", "chief-architect", parent.id);
    require(js_GetLastMsg(&raw, inspection.js, "homeric-myrmidon", dispatch.c_str(), nullptr,
                          nullptr) == NATS_OK,
            "parent wakeup absent");
    const auto event = json::parse(bytes(raw));
    natsMsg_Destroy(raw);
    require(event["operation"] == "child_completed" && event["completed_child_id"] == child.id,
            "canonical child did not wake the parent");
    require(restarted.get_hmas_task(parent.id)->state == TaskState::Decomposing,
            "child completion incorrectly completed parent");
    std::cout << json({{"schema", "hi/fleet/native-producer-proof/v1"},
                       {"exactBytes", payload.size()},
                       {"deliveries", deliveries.load()},
                       {"replays", replayed.load()},
                       {"initialBackingIssues", 2},
                       {"failedWriteFenced", failed_write_fenced.load()},
                       {"briefId", brief},
                       {"parentWakeup", event},
                       {"authority", "controlled-github-fixture"}})
                     .dump()
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "native producer contract failed: " << error.what() << '\n';
    return 1;
  }
}
