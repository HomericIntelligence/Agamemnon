#include "agamemnon/fake_nats_publisher.hpp"
#include "agamemnon/fleet.hpp"
#include "agamemnon/store.hpp"

#include <fstream>
#include <iostream>
#include <regex>

using namespace agamemnon;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

// Consumes an independently captured, sanitized fixture artifact. No live
// GitHub, broker, provider, or workspace access occurs in this native check.
int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: fleet_fact_import grouped-worker-facts.json");
    std::ifstream input(argv[1]);
    require(input.good(), "captured fact artifact is required");
    json bundle;
    input >> bundle;
    require(bundle.at("schema") == "hi/fleet/bridge-fixture-facts/v1",
            "unsupported capture schema");
    const auto& originals = bundle.at("originalCommands");
    const auto& groups = bundle.at("groups");
    require(originals.is_array() && originals.size() == 5 && groups.size() == originals.size(),
            "expected captured start/input/interrupt/resume/cancel lifecycle");
    const auto& first = originals[0].at("command");
    const std::string subject = originals[0].at("subject");
    std::smatch role;
    require(std::regex_match(subject, role,
                             std::regex(R"(^hi\.myrmidon\.([^.]+)\.([^.]+)\.task\.(.+)$)")),
            "start must use the canonical role task subject");
    require(first.at("taskId") == role[3].str(), "subject task identity mismatch");
    auto github = std::make_shared<MockGitHubClient>();
    Store store(github);
    FakeNatsPublisher publisher;
    FleetService fleet(store, publisher);
    HmasTask task;
    task.id = first.at("taskId");
    task.layer = HmasLayer::L3_TaskAgent;
    task.state = TaskState::Pending;
    store.create_hmas_task(task);
    fleet.create("pools", {{"id", "import-pool"}, {"capacity", 1}});
    fleet.create("workers",
                 {{"id", first.at("workerId")}, {"poolId", "import-pool"}, {"capacity", 1}});
    fleet.create("sessions", {{"id", first.at("targetId")},
                              {"workerId", first.at("workerId")},
                              {"agentId", first.at("agentId")},
                              {"taskId", first.at("taskId")},
                              {"workspace", first.at("workspace")},
                              {"executionId", first.at("executionId")},
                              {"stage", first.at("stage")},
                              {"domain", role[1].str()},
                              {"hmasRole", role[2].str()}});
    std::size_t accepted = 0;
    for (std::size_t index = 0; index < groups.size(); ++index) {
      const auto& original = originals[index].at("command");
      const auto& group = groups[index];
      auto physical = group.at("command");
      require(physical.at("workspace") == bundle.at("workspaceMapping").at("to"),
              "undeclared fixture workspace");
      require(original.at("workspace") == bundle.at("workspaceMapping").at("from"),
              "undeclared original workspace");
      physical["workspace"] = original.at("workspace");
      require(physical == original && group.at("commandId") == original.at("commandId"),
              "capture changed an exported command beyond the declared workspace mapping");
      require(group.at("subject") == originals[index].at("subject"),
              "capture changed the transport subject");
      auto created = fleet.command(original.at("targetKind"), original.at("targetId"),
                                   original.at("operation"),
                                   {{"commandId", original.at("commandId")},
                                    {"idempotencyKey", original.at("idempotencyKey")},
                                    {"generation", original.at("generation")},
                                    {"payload", original.at("payload")}});
      require(created.at("command") == original,
              "native durable command diverged from captured producer envelope");
      require(publisher.calls.back().subject == group.at("subject").get<std::string>(),
              "native dispatch subject diverged");
      require(group.at("facts").is_array() && !group["facts"].empty(),
              "command has no captured facts");
      for (const auto& fact : group["facts"]) {
        const auto result = fleet.on_worker_event(
            "hi.fleet.events." + fact.at("workerId").get<std::string>(), fact);
        require(result.at("acknowledged") == true && result.at("eventId") == fact.at("eventId"),
                "native owner did not acknowledge the original captured fact");
        if (original.at("operation") == "interrupt" || original.at("operation") == "cancel") {
          if (fact.value("kind", "") != "activity")
            require(result.at("record").at("claimStatus") != "released",
                    "command receipt released active ownership");
          else if (fact.at("event").value("outcome", "") == "interrupted" ||
                   fact.at("event").value("outcome", "") == "cancelled") {
            require(fact.at("event").at("backgroundCleanup") == "confirmed_empty",
                    "captured stop lacks cleanup proof");
            require(result.at("record").at("claimStatus") == "released",
                    "confirmed captured stop was not reconciled");
          }
        }
        ++accepted;
      }
    }
    const auto before = fleet.get("sessions", first.at("targetId"));
    require(before.at("status") == "cancelled" && before.at("claimStatus") == "released",
            "final cancellation was not confirmed");
    require(store.get_hmas_task(task.id)->state == TaskState::InProgress,
            "worker facts completed canonical issue work");
    std::size_t replayed = 0;
    for (const auto& group : groups)
      for (const auto& fact : group["facts"]) {
        require(
            fleet.on_worker_event("hi.fleet.events." + fact.at("workerId").get<std::string>(), fact)
                    .at("acknowledged") == true,
            "captured fact replay was not acknowledged");
        ++replayed;
      }
    require(fleet.get("sessions", first.at("targetId")) == before,
            "captured replay changed terminal state");
    std::cout << "Native FleetService accepted " << accepted << " captured bridge facts and "
              << replayed << " unchanged replays across " << groups.size()
              << " exported commands; canonical task remains InProgress.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Native captured-fact check failed: " << error.what() << '\n';
    return 1;
  }
}
