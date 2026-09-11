#include "agamemnon/fleet.hpp"

#include "agamemnon/nats_publisher.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/projects.hpp"
#include "agamemnon/store.hpp"

#include <algorithm>
#include <openssl/crypto.h>
#include <regex>
#include <set>

#include "httplib.h"

namespace agamemnon {
namespace {
const std::set<std::string> kinds{"pools", "workers", "sessions", "executions", "build-jobs"};
const std::regex identifier("[a-zA-Z0-9][a-zA-Z0-9_-]{0,127}");

std::string string_field(const json& body, const std::string& name, bool required = true) {
  if (!body.contains(name)) {
    if (!required) return "";
    throw FleetError(400, "missing " + name);
  }
  if (!body[name].is_string() || body[name].get_ref<const std::string&>().empty())
    throw FleetError(400, name + " must be a nonempty string");
  auto value = body[name].get<std::string>();
  if (value.size() > 1024) throw FleetError(400, name + " is too long");
  return value;
}

void check_kind(const std::string& kind) {
  if (!kinds.contains(kind)) throw FleetError(404, "unknown Fleet resource");
}

json canonical_claim(const json& record) {
  return {{"schema", "hi/fleet/claim/v1"},      {"targetKind", record.at("kind")},
          {"targetId", record.at("id")},        {"workerId", record.at("workerId")},
          {"agentId", record.at("agentId")},    {"generation", record.at("generation")},
          {"workspace", record.at("workspace")}};
}

json request_body(const httplib::Request& request) {
  if (request.body.size() > 16384) throw FleetError(413, "Fleet request exceeds 16 KiB");
  auto body = json::parse(request.body, nullptr, false);
  if (!body.is_object()) throw FleetError(400, "request must be a JSON object");
  return body;
}

template <typename Function>
void reply(httplib::Response& response, int status, Function function) {
  json result;
  try {
    result = function();
    response.status = status;
  } catch (const FleetError& error) {
    response.status = error.status;
    result = {{"error", error.what()}};
  } catch (const json::exception&) {
    response.status = 400;
    result = {{"error", "invalid Fleet field type"}};
  } catch (const std::exception&) {
    response.status = 503;
    result = {{"error", "durable Fleet operation unavailable; reconcile before retry"}};
  }
  response.set_content(result.dump(), "application/json");
}
}  // namespace

FleetService::FleetService(Store& store, NatsPublisher& publisher, Orchestrator* orchestrator,
                           std::string resolution_key, std::shared_ptr<ProjectProjection> projects)
    : store_(store),
      publisher_(publisher),
      orchestrator_(orchestrator),
      resolution_key_(std::move(resolution_key)),
      github_(store.github_client()),
      projects_(projects ? std::move(projects) : std::make_shared<ProjectProjection>(github_)) {}

json FleetService::projects_health() const { return projects_->health(); }
json FleetService::reconcile_projects() { return projects_->reconcile(); }

std::string FleetService::body_(const json& document) {
  auto body = "## AgamemnonEntity: fleet\n\n```json\n" + document.dump() + "\n```\n";
  if (body.size() > 60000) throw FleetError(507, "Fleet record history requires archival");
  return body;
}

void FleetService::load_() {
  if (!github_) throw FleetError(503, "Fleet requires GitHub-backed persistence");
  if (loaded_) return;
  std::map<std::string, Entry> hydrated;
  std::uint64_t sequence = 0;
  // Fetch completely before publishing a cache. Errors keep hydration retryable.
  try {
    for (const auto& issue : github_->list_issues_including_closed("agamemnon-fleet")) {
      const auto body = issue.at("body").get<std::string>();
      auto start = body.find("```json\n");
      auto end = start == std::string::npos ? start : body.find("\n```", start + 8);
      if (end == std::string::npos) throw FleetError(503, "malformed Fleet backing issue");
      auto document = json::parse(body.substr(start + 8, end - start - 8));
      if (document.at("schema") != "hi/fleet/v1")
        throw FleetError(503, "unsupported Fleet record schema");
      auto kind = document.at("kind").get<std::string>();
      check_kind(kind);
      auto id = document.at("record").at("id").get<std::string>();
      document["record"]["activity"] = "unknown";
      document["record"]["observationState"] = "reconciliation_required";
      for (const auto& event : document.at("events"))
        sequence = std::max(sequence, event.at("seq").get<std::uint64_t>());
      if (!hydrated
               .emplace(kind + "/" + id,
                        Entry{std::to_string(issue.at("number").get<int>()), document})
               .second)
        throw FleetError(503, "duplicate Fleet backing record; reconcile first");
    }
  } catch (const json::exception&) {
    throw FleetError(503, "malformed Fleet backing issue; reconciliation required");
  }
  entries_ = std::move(hydrated);
  sequence_ = sequence;
  loaded_ = true;
}

FleetService::Entry& FleetService::find_(const std::string& kind, const std::string& id) {
  check_kind(kind);
  auto found = entries_.find(kind + "/" + id);
  if (found == entries_.end()) throw FleetError(404, "Fleet resource not found");
  return found->second;
}

void FleetService::persist_(Entry& entry, json document, const std::string& event) {
  auto& record = document["record"];
  record["updatedAt"] = now_iso8601();
  document["events"].push_back({{"seq", sequence_ + 1},
                                {"kind", document["kind"]},
                                {"targetId", record["id"]},
                                {"event", event},
                                {"at", record["updatedAt"]},
                                {"record", record}});
  auto body = body_(document);
  try {
    if (entry.issue.empty()) {
      auto issue = github_->create_issue(
          "fleet: " + document["kind"].get<std::string>() + "/" + record["id"].get<std::string>(),
          body, "agamemnon-fleet");
      if (issue.empty()) throw FleetError(503, "GitHub did not acknowledge Fleet record");
      entry.issue = issue;
    } else {
      github_->update_issue_body(entry.issue, body);
    }
  } catch (...) {
    // A response can be lost after GitHub commits. Reconcile before another
    // admission can inspect ownership or reuse an apparently free slot.
    loaded_ = false;
    throw;
  }
  entry.document = std::move(document);
  ++sequence_;
}

json FleetService::create(const std::string& kind, const json& body) {
  std::lock_guard lock(mutex_);
  load_();
  check_kind(kind);
  auto id = string_field(body, "id");
  if (!std::regex_match(id, identifier)) throw FleetError(400, "invalid Fleet id");
  if (entries_.contains(kind + "/" + id)) throw FleetError(409, "Fleet id already exists");
  json record = {{"id", id},
                 {"kind", kind},
                 {"schema", "hi/fleet/v1"},
                 {"status", "created"},
                 {"generation", 1},
                 {"createdAt", now_iso8601()},
                 {"claimStatus", "unclaimed"},
                 {"lastActivityAt", nullptr},
                 {"waitingReason", nullptr}};
  static const std::set<std::string> fields{
      "name", "workerId", "agentId",      "taskId",   "issueUrl",  "executionId", "sessionId",
      "host", "poolId",   "allocationId", "stage",    "workspace", "domain",      "hmasRole",
      "mode", "backend",  "purpose",      "capacity", "resources"};
  for (const auto& [key, value] : body.items()) {
    if (fields.contains(key))
      record[key] = value;
    else if (key != "id")
      throw FleetError(400, "unknown or server-owned Fleet field: " + key);
  }
  if (kind == "pools" || kind == "workers") {
    if (!body.contains("capacity") || !body["capacity"].is_number_integer() ||
        body["capacity"].get<int>() < 1 || body["capacity"].get<int>() > 108)
      throw FleetError(400, "capacity must be between 1 and 108");
  }
  if (kind == "workers") {
    auto& pool = find_("pools", string_field(body, "poolId")).document["record"];
    auto capacity = body["capacity"].get<int>();
    for (const auto& [key, entry] : entries_) {
      const auto& other = entry.document["record"];
      if (entry.document["kind"] == "workers" && other["poolId"] == pool["id"])
        capacity += other["capacity"].get<int>();
    }
    if (capacity > pool["capacity"].get<int>()) throw FleetError(409, "pool capacity exceeded");
  }
  if (kind == "sessions" || kind == "executions" || kind == "build-jobs") {
    auto& worker = find_("workers", string_field(body, "workerId")).document["record"];
    record["poolId"] = worker["poolId"];
    record["host"] = worker.value("host", json(nullptr));
    record["allocationId"] = worker.value("allocationId", json(nullptr));
    record["generation"] = worker["generation"];
    record["agentId"] = string_field(body, "agentId");
    record["workspace"] = string_field(body, "workspace");
    if (!record.contains("stage"))
      record["stage"] = body.contains("taskId") ? "implementation" : "interactive";
    if (kind == "sessions") {
      record["sessionId"] = id;
      if (!record.contains("executionId")) record["executionId"] = generate_uuid();
    }
    if (kind == "executions") record["executionId"] = id;
  }
  Entry entry;
  persist_(entry,
           {{"schema", "hi/fleet/v1"},
            {"kind", kind},
            {"record", record},
            {"commands", json::array()},
            {"events", json::array()}},
           "created");
  auto result = entry.document["record"];
  entries_.emplace(kind + "/" + id, std::move(entry));
  return result;
}

json FleetService::list(const std::string& kind) {
  std::lock_guard lock(mutex_);
  load_();
  check_kind(kind);
  json items = json::array();
  for (const auto& [key, entry] : entries_)
    if (entry.document["kind"] == kind) items.push_back(entry.document["record"]);
  return {{"items", items}, {"total", items.size()}};
}

json FleetService::get(const std::string& kind, const std::string& id) {
  std::lock_guard lock(mutex_);
  load_();
  return find_(kind, id).document["record"];
}

json FleetService::get_command(const std::string& command_id) {
  std::lock_guard lock(mutex_);
  load_();
  for (const auto& [key, entry] : entries_)
    for (const auto& intent : entry.document["commands"])
      if (intent["command"]["commandId"] == command_id)
        return {{"command", intent["command"]},
                {"status", intent["status"]},
                {"record", entry.document["record"]}};
  throw FleetError(404, "Fleet command not found");
}

json FleetService::events(std::uint64_t after) {
  std::lock_guard lock(mutex_);
  load_();
  if (after > sequence_) throw FleetError(409, "event cursor is ahead; resynchronize");
  json events = json::array();
  for (const auto& [key, entry] : entries_)
    for (const auto& event : entry.document["events"])
      if (event["seq"].get<std::uint64_t>() > after) events.push_back(event);
  std::sort(events.begin(), events.end(), [](const json& a, const json& b) {
    return a["seq"].get<std::uint64_t>() < b["seq"].get<std::uint64_t>();
  });
  return {{"events", events}, {"cursor", sequence_}};
}

json FleetService::command(const std::string& kind, const std::string& id,
                           const std::string& operation, const json& body) {
  std::lock_guard lock(mutex_);
  load_();
  auto& entry = find_(kind, id);
  auto document = entry.document;
  auto& record = document["record"];
  if (kind == "pools") throw FleetError(409, "pool control requires an allocation adapter");
  static const std::set<std::string> operations{"start",  "input",  "respond", "interrupt",
                                                "cancel", "resume", "drain"};
  if (!operations.contains(operation)) throw FleetError(400, "unsupported Fleet operation");
  auto command_id = string_field(body, "commandId");
  auto key = string_field(body, "idempotencyKey");
  if (!std::regex_match(command_id, identifier)) throw FleetError(400, "invalid commandId");
  if (!body.contains("generation") || !body["generation"].is_number_integer() ||
      body["generation"] != record["generation"])
    throw FleetError(409, "stale execution generation");
  if (record.contains("taskId") && record["status"] != "created") {
    auto task = store_.get_hmas_task(record["taskId"]);
    if (!task || task->fleet_claim != canonical_claim(record) ||
        task->state == TaskState::Completed || task->state == TaskState::Failed)
      throw FleetError(409, "canonical task does not authorize further execution");
  }
  auto payload = body.value("payload", json::object());
  if (!payload.is_object()) throw FleetError(400, "payload must be an object");
  // Private conversation content is referenced, never persisted in GitHub.
  static const std::set<std::string> payload_fields{"inputRef",  "promptRef",  "responseRef",
                                                    "requestId", "approvalId", "decision",
                                                    "reason",    "recipe",     "snapshotRef"};
  for (const auto& [field, value] : payload.items()) {
    if (!payload_fields.contains(field) || !value.is_string() ||
        value.get_ref<const std::string&>().size() > 1024)
      throw FleetError(400, "control payload must contain bounded metadata references");
  }
  auto worker_id = kind == "workers" ? id : string_field(record, "workerId");
  json envelope = {{"schema", "hi/fleet/v1"}, {"commandId", command_id},
                   {"idempotencyKey", key},   {"generation", record["generation"]},
                   {"operation", operation},  {"targetId", id},
                   {"targetKind", kind},      {"workerId", worker_id},
                   {"payload", payload}};
  for (const auto* field : {"taskId", "agentId", "sessionId", "executionId", "workspace", "stage"})
    if (record.contains(field)) envelope[field] = record[field];
  std::string subject = "hi.fleet.control." + worker_id;
  if (operation == "start" && kind != "workers" && record.contains("taskId")) {
    auto task_id = string_field(record, "taskId");
    auto domain = string_field(record, "domain");
    auto role = string_field(record, "hmasRole");
    if (!std::regex_match(task_id, identifier) || !std::regex_match(domain, identifier) ||
        !std::regex_match(role, identifier))
      throw FleetError(400, "invalid task routing fields");
    if (!store_.get_hmas_task(task_id)) throw FleetError(409, "canonical task does not exist");
    subject = "hi.myrmidon." + domain + "." + role + ".task." + task_id;
  }
  for (const auto& previous : document["commands"]) {
    const auto& prior = previous["command"];
    if (prior["idempotencyKey"] == key || prior["commandId"] == command_id) {
      if (prior != envelope) throw FleetError(409, "command identity reused with different intent");
      if (previous["status"] == "pending" && !publisher_.publish(subject, envelope.dump()))
        throw FleetError(503, "command persisted; delivery uncertain; retry the same command");
      return {{"command", envelope}, {"status", previous["status"]}};
    }
  }
  for (const auto& [entry_key, other] : entries_)
    for (const auto& previous : other.document["commands"])
      if (previous["command"]["commandId"] == command_id)
        throw FleetError(409, "commandId already belongs to another target");
  const auto status = record["status"].get<std::string>();
  if (status == "cancelling" || status == "interrupting")
    throw FleetError(409, "stop confirmation is pending; new control is fenced");
  if (operation != "cancel" && operation != "interrupt") {
    for (const auto& prior : document["commands"])
      if (prior["status"] == "pending" || prior["status"] == "accepted")
        throw FleetError(409, "prior command acknowledgment is pending");
  }
  const int input_refs = static_cast<int>(payload.contains("inputRef")) +
                         static_cast<int>(payload.contains("promptRef"));
  if (operation == "start" && (input_refs > 0 || payload.contains("responseRef")))
    throw FleetError(400, "start creates a session; submit private input separately");
  if (operation == "input" && (input_refs != 1 || payload.contains("responseRef")))
    throw FleetError(400, "input requires exactly one inputRef or promptRef");
  if (operation == "respond" &&
      (input_refs != 0 || !payload.contains("responseRef") || !payload.contains("requestId")))
    throw FleetError(400, "respond requires responseRef and requestId");
  if (operation == "start" || operation == "resume") {
    if (kind == "workers") throw FleetError(409, "worker start requires an allocation adapter");
    if ((operation == "start" && status != "created") ||
        (operation == "resume" && status != "interrupted"))
      throw FleetError(409, "target is not eligible for this operation");
    auto& worker = find_("workers", worker_id).document["record"];
    if (worker["status"] == "draining") throw FleetError(409, "worker is draining");
    int occupied = 0;
    for (const auto& [entry_key, other] : entries_) {
      const auto& peer = other.document["record"];
      if (entry_key == kind + "/" + id) continue;
      if (peer.value("status", "") == "interrupted" &&
          (peer.value("agentId", "") == record.value("agentId", "") ||
           peer.value("workspace", "") == record.value("workspace", "")))
        throw FleetError(409, "interrupted conversation retains its agent and workspace");
      if (peer.value("claimStatus", "unclaimed") == "unclaimed" ||
          peer.value("claimStatus", "unclaimed") == "released")
        continue;
      if (peer.value("workerId", "") == worker_id) ++occupied;
      if (peer.value("agentId", "") == record.value("agentId", "") ||
          peer.value("workspace", "") == record.value("workspace", "") ||
          (record.contains("taskId") && peer.value("taskId", "") == record.value("taskId", "")))
        throw FleetError(409, "task, agent, or workspace already has an owner");
    }
    if (occupied >= worker["capacity"].get<int>())
      throw FleetError(409, "worker capacity exhausted");
    if (record.contains("taskId")) {
      if (!store_.reserve_hmas_fleet_claim(record["taskId"], canonical_claim(record)))
        throw FleetError(409, "canonical task is ineligible or already owned");
    }
    record["status"] = "admitted";
    record["claimStatus"] = "reserved";
  } else if (operation == "drain") {
    if (kind != "workers") throw FleetError(409, "drain operates on a worker");
    record["status"] = "draining";
  } else {
    if (record["claimStatus"] != "reserved" && record["claimStatus"] != "claimed")
      throw FleetError(409, "target has no active admission");
    if (operation == "cancel" || operation == "interrupt") {
      record["status"] = operation == "cancel" ? "cancelling" : "interrupting";
      record["stopCommandId"] = command_id;
      for (auto& previous : document["commands"]) {
        if (previous["status"] == "pending" || previous["status"] == "accepted") {
          previous["status"] = "superseded";
          previous["supersededBy"] = command_id;
        }
      }
    }
  }
  record["commandId"] = command_id;
  if (operation == "input") {
    record["latestInputCommandId"] = command_id;
    record["expectedProviderTurnId"] = nullptr;
  }
  if (operation == "input" || operation == "resume") record["backgroundCleanup"] = nullptr;
  if (operation == "input" || operation == "respond") {
    record["activity"] = "unknown";
    record["observationState"] = "awaiting_activity";
  }
  document["commands"].push_back(
      {{"command", envelope}, {"status", "pending"}, {"facts", json::array()}});
  // Leave room for the current command's terminal confirmation.
  if (body_(document).size() > 45000 && operation != "cancel" && operation != "interrupt")
    throw FleetError(507, "Fleet record requires archival before new work");
  persist_(entry, std::move(document), operation + ".requested");
  if (!publisher_.publish(subject, envelope.dump()))
    throw FleetError(503, "command persisted; delivery uncertain; retry the same command");
  return {{"command", envelope}, {"status", "pending"}};
}

json FleetService::acknowledge(const std::string& kind, const std::string& id, const json& fact) {
  std::lock_guard lock(mutex_);
  load_();
  if (fact.value("schema", "") != "hi/fleet/v1") throw FleetError(400, "unsupported Fleet schema");
  static const std::set<std::string> fact_fields{"schema",    "eventId",    "workerId",
                                                 "commandId", "generation", "targetKind",
                                                 "targetId",  "status",     "receipt"};
  for (const auto& [field, value] : fact.items())
    if (!fact_fields.contains(field)) throw FleetError(400, "unsupported acknowledgment field");
  auto& entry = find_(kind, id);
  auto document = entry.document;
  auto& record = document["record"];
  auto worker_id = kind == "workers" ? id : string_field(record, "workerId");
  if (string_field(fact, "workerId") != worker_id || !fact.contains("generation") ||
      fact["generation"] != record["generation"])
    throw FleetError(409, "worker identity or generation does not own this target");
  auto event_id = string_field(fact, "eventId");
  auto command_id = string_field(fact, "commandId");
  auto status = string_field(fact, "status");
  static const std::set<std::string> statuses{"accepted", "completed", "failed"};
  static const std::set<std::string> terminal{"completed", "failed"};
  if (!statuses.contains(status)) throw FleetError(400, "invalid worker status");
  auto receipt = fact.value("receipt", json::object());
  if (!receipt.is_object() || receipt.dump().size() > 4096)
    throw FleetError(400, "receipt must contain bounded metadata");
  static const std::set<std::string> receipt_fields{
      "stage",      "waitingReason", "threadId",  "turnId",           "artifactRef",
      "receiptRef", "observedAt",    "sessionId", "providerThreadId", "providerTurnId",
      "requestId",  "draining",      "error"};
  for (const auto& [field, value] : receipt.items())
    if (!receipt_fields.contains(field) ||
        (!value.is_string() && !value.is_null() && !(field == "draining" && value.is_boolean())))
      throw FleetError(400, "unsupported receipt metadata field");
  for (auto& pending : document["commands"]) {
    for (const auto& seen : pending["facts"]) {
      if (seen["eventId"] == event_id) {
        if (seen != fact) throw FleetError(409, "eventId reused with different fact");
        return {{"record", record}, {"eventId", event_id}, {"acknowledged", true}};
      }
    }
    if (pending["command"]["commandId"] != command_id) continue;
    if (terminal.contains(pending["status"].get<std::string>()) && pending["status"] != status)
      throw FleetError(409, "terminal command cannot regress");
    pending["facts"].push_back(fact);
    if (pending["status"] != "superseded") pending["status"] = status;
    // A completed app-server command does not establish model activity, turn
    // completion, process exit, or issue completion.
    record["lastCommandReceipt"] = receipt;
    if (record.value("latestInputCommandId", "") == command_id && status == "completed" &&
        receipt.contains("providerTurnId") && receipt["providerTurnId"].is_string())
      record["expectedProviderTurnId"] = receipt["providerTurnId"];
    persist_(entry, std::move(document), "worker." + status);
    return {{"record", entry.document["record"]}, {"eventId", event_id}, {"acknowledged", true}};
  }
  throw FleetError(404, "command does not belong to target");
}

json FleetService::on_worker_event(const std::string& subject, const json& fact) {
  if (fact.value("schema", "") != "hi/fleet/v1") throw FleetError(400, "unsupported Fleet schema");
  if (subject != "hi.fleet.events." + string_field(fact, "workerId"))
    throw FleetError(409, "event subject does not match worker");
  auto kind = string_field(fact, "targetKind");
  auto id = string_field(fact, "targetId");
  if (fact.value("kind", "") != "activity") return acknowledge(kind, id, fact);
  std::lock_guard lock(mutex_);
  load_();
  auto& entry = find_(kind, id);
  auto document = entry.document;
  auto& record = document["record"];
  if (fact.at("generation") != record["generation"] ||
      string_field(fact, "workerId") != string_field(record, "workerId"))
    throw FleetError(409, "activity does not belong to this worker generation");
  auto event = fact.at("event");
  if (!event.is_object() || event.dump().size() > 4096)
    throw FleetError(400, "activity must contain bounded metadata");
  for (const auto* field :
       {"taskId", "agentId", "sessionId", "executionId", "workerId", "generation"})
    if (event.contains(field) && event[field] != record.value(field, json(nullptr)))
      throw FleetError(409, "activity linkage does not match the logical execution");
  if (!fact.contains("sourceSequence") || !fact["sourceSequence"].is_number_integer() ||
      fact["sourceSequence"].get<std::int64_t>() < 1)
    throw FleetError(400, "activity requires a positive sourceSequence");
  auto event_id = string_field(fact, "eventId");
  auto sequence = fact["sourceSequence"].get<std::uint64_t>();
  auto previous = record.value("sourceSequence", std::uint64_t{0});
  if (sequence <= previous) {
    if (sequence < previous)
      return {
          {"record", record}, {"eventId", event_id}, {"acknowledged", true}, {"superseded", true}};
    if (sequence == previous && record.value("lastActivityEventId", "") == event_id)
      return {{"record", record}, {"eventId", event_id}, {"acknowledged", true}};
    throw FleetError(409, "stale worker activity; reconcile source cursor");
  }
  auto activity = string_field(event, "activity");
  static const std::set<std::string> activities{"model_working", "tool_running", "waiting_approval",
                                                "waiting_input", "idle",         "disconnected",
                                                "unknown"};
  if (!activities.contains(activity)) throw FleetError(400, "unsupported worker activity");
  if (activity != "unknown" && activity != "disconnected" && record["claimStatus"] != "claimed" &&
      record["claimStatus"] != "reserved")
    throw FleetError(409, "worker activity has no current admission");
  auto observed_at = string_field(event, "observedAt");
  const bool cleaned =
      activity == "idle" && event.value("backgroundCleanup", json(nullptr)) == "confirmed_empty";
  const auto expected_turn = record.value("expectedProviderTurnId", json(nullptr));
  if (cleaned && !expected_turn.is_null() &&
      event.value("providerTurnId", json(nullptr)) != expected_turn)
    throw FleetError(409, "cleanup does not confirm the latest admitted provider turn");
  record["sourceSequence"] = sequence;
  record["lastActivityEventId"] = event_id;
  record["lastActivityAt"] = observed_at;
  record["activity"] = activity;
  record["observationState"] = "observed";
  record["lastActivityReceivedAt"] = now_iso8601();
  record["waitingReason"] = event.value("waitingReason", json(nullptr));
  // Cleanup evidence applies only to this observation. A later turn or unknown
  // observation must not inherit a previous turn's empty process inventory.
  record["backgroundCleanup"] = cleaned ? json("confirmed_empty") : json(nullptr);
  for (const auto* field : {"stage", "providerThreadId", "providerTurnId", "outcome"})
    if (event.contains(field)) record[field] = event[field];
  bool control_transition = false;
  const auto status = record["status"].get<std::string>();
  if ((activity == "model_working" || activity == "tool_running") &&
      (record["claimStatus"] == "reserved" || record["claimStatus"] == "claimed")) {
    if (record.contains("taskId") &&
        !store_.observe_hmas_fleet_start(record["taskId"], canonical_claim(record)))
      throw FleetError(409, "worker does not own the canonical task claim");
    control_transition = record["claimStatus"] == "reserved";
    record["claimStatus"] = "claimed";
    if (status != "cancelling" && status != "interrupting") record["status"] = "running";
  } else if (activity == "idle") {
    const auto outcome = event.value("outcome", "");
    if ((status == "cancelling" && outcome == "cancelled") ||
        (status == "interrupting" && outcome == "interrupted")) {
      if (event.value("commandId", "") != record.value("stopCommandId", ""))
        throw FleetError(409, "stop outcome does not match the current stop command");
      if (record["backgroundCleanup"] != "confirmed_empty")
        throw FleetError(409, "stop requires confirmed empty background process inventory");
      record["status"] = outcome;
      record["claimStatus"] = "released";
      for (auto& command : document["commands"])
        if (command["command"]["commandId"] == record["stopCommandId"])
          command["status"] = "completed";
      control_transition = true;
    } else if (status != "cancelling" && status != "interrupting") {
      record["status"] = "idle";
    }
  } else if (activity == "waiting_input" || activity == "waiting_approval") {
    if (status != "cancelling" && status != "interrupting") record["status"] = "waiting";
  }
  if (control_transition) {
    persist_(entry, std::move(document), "worker.claim");
  } else {
    // Observability is not a GitHub-per-packet write. The next confirmed
    // control transition checkpoints the latest observed metadata.
    entry.document["record"] = std::move(record);
  }
  return {{"record", entry.document["record"]}, {"eventId", event_id}, {"acknowledged", true}};
}

json FleetService::resolve(const std::string& kind, const std::string& id, const json& body,
                           const std::string& resolution_key) {
  if (resolution_key_.empty() || !orchestrator_)
    throw FleetError(503, "manual Fleet resolution is not enabled");
  if (resolution_key.size() != resolution_key_.size() ||
      CRYPTO_memcmp(resolution_key.data(), resolution_key_.data(), resolution_key.size()) != 0)
    throw FleetError(403, "operator resolution credential required");
  std::lock_guard lock(mutex_);
  load_();
  auto& entry = find_(kind, id);
  auto document = entry.document;
  auto& record = document["record"];
  if (!record.contains("taskId")) throw FleetError(409, "resolution requires a canonical task");
  if (!body.contains("generation") || body["generation"] != record["generation"])
    throw FleetError(409, "stale resolution generation");
  json decision = {{"provenance", "manual"}, {"verifiedApproval", false}};
  for (const auto* field : {"decisionId", "outcome", "decision", "reviewerId", "evidenceRef"})
    decision[field] = string_field(body, field);
  decision["generation"] = record["generation"];
  const auto outcome = decision["outcome"].get<std::string>();
  if ((outcome != "completed" && outcome != "failed") ||
      decision["decision"] !=
          (outcome == "completed" ? "approve_completion" : "reject_completion") ||
      decision["reviewerId"] == record["agentId"])
    throw FleetError(400, "an explicit independent manual review decision is required");
  if (record.contains("resolution")) {
    if (record["resolution"] != decision)
      throw FleetError(409, "resolution identity or decision changed");
    return record;
  }
  auto task = store_.get_hmas_task(record["taskId"]);
  if (!task || task->fleet_claim != canonical_claim(record))
    throw FleetError(409, "canonical task claim does not match");
  const bool confirmed = task->fleet_resolution == decision;
  if (!confirmed && (record.value("activity", "unknown") != "idle" ||
                     record.value("observationState", "") != "observed" ||
                     record.value("backgroundCleanup", json(nullptr)) != "confirmed_empty" ||
                     record["status"] == "cancelling" || record["status"] == "interrupting"))
    throw FleetError(409, "resolution requires confirmed inactive execution");
  for (const auto& command : document["commands"])
    if (command["status"] == "pending" || command["status"] == "accepted")
      throw FleetError(409, "resolution cannot overtake an unacknowledged command");
  auto receipt = record.value("lastCommandReceipt", json::object());
  if (!confirmed && receipt.contains("providerTurnId") &&
      record.value("providerTurnId", json(nullptr)) != receipt["providerTurnId"])
    throw FleetError(409, "inactive observation does not match the latest turn");
  if (!orchestrator_->resolve_fleet_task(record["taskId"], canonical_claim(record), decision))
    throw FleetError(409, "canonical task claim or resolution does not match");
  record["resolution"] = decision;
  record["status"] = outcome;
  record["claimStatus"] = "released";
  persist_(entry, std::move(document), "task.resolved");
  return entry.document["record"];
}

void register_fleet_routes(httplib::Server& server, std::shared_ptr<FleetService> fleet) {
  server.Get("/v1/fleet/projects", [fleet](const httplib::Request&, httplib::Response& response) {
    reply(response, 200, [&] { return fleet->projects_health(); });
  });
  server.Post("/v1/fleet/projects/reconcile",
              [fleet](const httplib::Request&, httplib::Response& response) {
                reply(response, 200, [&] { return fleet->reconcile_projects(); });
              });
  server.Post(R"(/v1/fleet/(sessions|executions|build-jobs)/([A-Za-z0-9_-]+)/resolve)",
              [fleet](const httplib::Request& request, httplib::Response& response) {
                reply(response, 200, [&] {
                  return fleet->resolve(request.matches[1], request.matches[2],
                                        request_body(request),
                                        request.get_header_value("X-Fleet-Resolution-Key"));
                });
              });
  server.Get(R"(/v1/fleet/commands/([A-Za-z0-9_-]+))",
             [fleet](const httplib::Request& request, httplib::Response& response) {
               reply(response, 200, [&] { return fleet->get_command(request.matches[1]); });
             });
  server.Post(
      "/v1/fleet/events", [fleet](const httplib::Request& request, httplib::Response& response) {
        reply(response, 200, [&] {
          auto fact = request_body(request);
          return fleet->on_worker_event("hi.fleet.events." + string_field(fact, "workerId"), fact);
        });
      });
  server.Get("/v1/fleet/events",
             [fleet](const httplib::Request& request, httplib::Response& response) {
               reply(response, 200, [&] {
                 auto cursor = request.has_param("after") ? request.get_param_value("after") : "0";
                 if (cursor.empty() || cursor.find_first_not_of("0123456789") != std::string::npos)
                   throw FleetError(400, "invalid event cursor");
                 try {
                   return fleet->events(std::stoull(cursor));
                 } catch (const std::out_of_range&) {
                   throw FleetError(400, "invalid event cursor");
                 }
               });
             });
  server.Get(R"(/v1/fleet/(pools|workers|sessions|executions|build-jobs))",
             [fleet](const httplib::Request& request, httplib::Response& response) {
               reply(response, 200, [&] { return fleet->list(request.matches[1]); });
             });
  server.Get(R"(/v1/fleet/(pools|workers|sessions|executions|build-jobs)/([A-Za-z0-9_-]+))",
             [fleet](const httplib::Request& request, httplib::Response& response) {
               reply(response, 200,
                     [&] { return fleet->get(request.matches[1], request.matches[2]); });
             });
  server.Post(R"(/v1/fleet/(pools|workers|sessions|executions|build-jobs))",
              [fleet](const httplib::Request& request, httplib::Response& response) {
                reply(response, 201,
                      [&] { return fleet->create(request.matches[1], request_body(request)); });
              });
  server.Post(
      R"(/v1/fleet/(pools|workers|sessions|executions|build-jobs)/([A-Za-z0-9_-]+)/(start|input|respond|interrupt|cancel|resume|drain|ack))",
      [fleet](const httplib::Request& request, httplib::Response& response) {
        auto operation = request.matches[3].str();
        reply(response, operation == "ack" ? 200 : 202, [&] {
          auto body = request_body(request);
          if (operation == "ack")
            return fleet->acknowledge(request.matches[1], request.matches[2], body);
          return fleet->command(request.matches[1], request.matches[2], operation, body);
        });
      });
}
}  // namespace agamemnon
