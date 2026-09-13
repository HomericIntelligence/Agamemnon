#include "agamemnon/fleet_build.hpp"

#include "agamemnon/fleet.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <regex>
#include <set>

namespace agamemnon::fleet_build {
namespace {
void fields(const json& value, std::initializer_list<const char*> names) {
  if (!value.is_object() || value.size() != names.size())
    throw FleetError(400, "build object has missing or unknown fields");
  for (const auto* name : names)
    if (!value.contains(name)) throw FleetError(400, "build object is missing a required field");
}

std::string text(const json& object, const char* name, std::size_t maximum = 128) {
  const auto& value = object.at(name);
  if (!value.is_string()) throw FleetError(400, "build identity must be a string");
  const auto result = value.get<std::string>();
  if (result.empty() || result.size() > maximum)
    throw FleetError(400, "build identity exceeds its bound");
  return result;
}

void identifier(const json& object, const char* name) {
  static const std::regex allowed("[a-zA-Z0-9][a-zA-Z0-9_-]{0,127}");
  if (!std::regex_match(text(object, name), allowed))
    throw FleetError(400, "invalid build identity");
}

void hash(const json& object, const char* name, std::size_t length = 64) {
  const auto value = text(object, name);
  if (value.size() != length || value.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw FleetError(400, "invalid build digest");
}

std::uint64_t integer(const json& value, bool zero = false) {
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() >
           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) ||
      value.get<std::int64_t>() < (zero ? 0 : 1))
    throw FleetError(400, "build quantity must be a bounded exact integer");
  return value.get<std::uint64_t>();
}

void identities(const json& value) {
  if (text(value, "platform") != "linux/aarch64")
    throw FleetError(400, "unsupported build platform");
  hash(value, "toolchainDigest");
  const auto image = text(value, "imageDigest");
  if (!image.starts_with("sha256:") || image.size() != 71 ||
      image.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos)
    throw FleetError(400, "build image must use its immutable SHA256 identity");
}

void unique(const json& collection) {
  if (!collection.is_array() || collection.empty() || collection.size() > 64)
    throw FleetError(400, "build catalog requires a bounded nonempty array");
  std::set<std::string> seen;
  for (const auto& value : collection) {
    identifier(value, "id");
    if (!seen.insert(value.at("id").get<std::string>()).second)
      throw FleetError(400, "duplicate build catalog identity");
  }
}

void validate_authorities(const json& authorities) {
  fields(authorities, {"schema", "authorities"});
  if (authorities.at("schema") != "hi/fleet/build-authorities/v1")
    throw FleetError(400, "unsupported build authority schema");
  unique(authorities.at("authorities"));
  for (const auto& authority : authorities.at("authorities")) {
    fields(authority, {"id", "workerId", "allocationId", "generation", "key"});
    identifier(authority, "workerId");
    identifier(authority, "allocationId");
    integer(authority.at("generation"));
    text(authority, "key", 1024);
  }
}
}  // namespace

void validate_submission(const json& request) {
  fields(request, {"schema", "workspaceId", "recipeId", "parameters", "idempotencyKey", "parent",
                   "snapshot"});
  if (request.at("schema") != "hi/fleet/build-submit/v1" ||
      request.at("parameters") != json::object())
    throw FleetError(400, "unsupported build submission or parameters");
  for (const auto* field : {"workspaceId", "recipeId", "idempotencyKey"})
    identifier(request, field);
  const auto& parent = request.at("parent");
  fields(parent, {"targetKind", "targetId", "sessionId", "executionId", "generation"});
  if (parent.at("targetKind") != "sessions" && parent.at("targetKind") != "executions")
    throw FleetError(400, "build parent must be a session or execution");
  for (const auto* field : {"targetId", "sessionId", "executionId"}) identifier(parent, field);
  integer(parent.at("generation"));
  const auto& snapshot = request.at("snapshot");
  fields(snapshot,
         {"reference", "manifestDigest", "baseCommit", "members", "bytes", "policyDigest"});
  identifier(snapshot, "reference");
  hash(snapshot, "manifestDigest");
  hash(snapshot, "baseCommit", 40);
  hash(snapshot, "policyDigest");
  integer(snapshot.at("members"));
  integer(snapshot.at("bytes"));
}

namespace {
void validate_catalog(const json& catalog) {
  fields(catalog, {"schema", "workspaces", "recipes", "allocations"});
  if (catalog.at("schema") != "hi/fleet/build-catalog/v1")
    throw FleetError(400, "unsupported build policy schema");
  for (const auto* name : {"workspaces", "recipes", "allocations"}) unique(catalog.at(name));
  for (const auto& value : catalog.at("workspaces")) {
    fields(value, {"id", "repository", "parentWorkspace", "snapshotPolicyDigest"});
    text(value, "repository");
    text(value, "parentWorkspace", 1024);
    hash(value, "snapshotPolicyDigest");
  }
  for (const auto& value : catalog.at("recipes")) {
    fields(value, {"id", "repository", "argv", "parameters", "recipeDigest", "lockDigest",
                   "platform", "imageDigest", "toolchainDigest", "resources"});
    if (value.at("id") != "hephaestus-test-unit-v1" ||
        value.at("argv") != json::array({"just", "test-unit"}) ||
        value.at("parameters") != json::object())
      throw FleetError(400, "unsupported registered build recipe");
    text(value, "repository");
    hash(value, "recipeDigest");
    hash(value, "lockDigest");
    identities(value);
    const auto& resources = value.at("resources");
    fields(resources, {"cpus", "gpus", "memoryBytes", "diskBytes", "wallSeconds", "outputBytes",
                       "artifactBytes", "snapshotBytes", "snapshotMembers"});
    for (const auto& [key, quantity] : resources.items()) integer(quantity, key == "gpus");
    if (resources.at("gpus") != 0) throw FleetError(400, "first build profile requires zero GPUs");
  }
  for (const auto& allocation : catalog.at("allocations")) {
    fields(allocation,
           {"id", "workerId", "generation", "authorityId", "platform", "imageDigest",
            "toolchainDigest", "qualificationReceiptDigest", "resources", "supervision"});
    identifier(allocation, "workerId");
    identifier(allocation, "authorityId");
    integer(allocation.at("generation"));
    hash(allocation, "qualificationReceiptDigest");
    identities(allocation);
    const auto& resources = allocation.at("resources");
    const auto& reserve = allocation.at("supervision");
    fields(resources, {"cpus", "gpus", "memoryBytes", "diskBytes"});
    fields(reserve, {"cpus", "memoryBytes"});
    for (const auto& [key, quantity] : resources.items()) integer(quantity, key == "gpus");
    for (const auto& [key, quantity] : reserve.items()) integer(quantity);
  }
}

json eligible_policies(const json& catalog, const json& request) {
  validate_catalog(catalog);
  json workspace;
  for (const auto& value : catalog.at("workspaces")) {
    if (value.at("id") == request.at("workspaceId")) workspace = value;
  }
  json recipe;
  for (const auto& value : catalog.at("recipes")) {
    if (value.at("id") == request.at("recipeId")) recipe = value;
  }
  if (workspace.is_null() || recipe.is_null())
    throw FleetError(400, "unregistered build workspace or recipe");
  const auto& snapshot = request.at("snapshot");
  if (workspace.at("repository") != recipe.at("repository") ||
      workspace.at("snapshotPolicyDigest") != snapshot.at("policyDigest") ||
      integer(snapshot.at("members")) > integer(recipe.at("resources").at("snapshotMembers")) ||
      integer(snapshot.at("bytes")) > integer(recipe.at("resources").at("snapshotBytes")))
    throw FleetError(400, "snapshot does not match the registered build policy");
  json result = json::array();
  for (const auto& allocation : catalog.at("allocations")) {
    const auto& resources = allocation.at("resources");
    const auto& reserve = allocation.at("supervision");
    bool fits = true;
    for (const auto* key : {"cpus", "memoryBytes"}) {
      const auto available = integer(resources.at(key));
      const auto supervision = integer(reserve.at(key));
      fits =
          fits && supervision < available &&
          integer(recipe.at("resources").at(key)) <= available - std::min(available, supervision);
    }
    fits = fits &&
           integer(recipe.at("resources").at("diskBytes")) <= integer(resources.at("diskBytes")) &&
           resources.at("gpus") == 0;
    for (const auto* key : {"platform", "imageDigest", "toolchainDigest"})
      fits = fits && allocation.at(key) == recipe.at(key);
    if (fits)
      result.push_back({{"workspace", workspace}, {"recipe", recipe}, {"allocation", allocation}});
  }
  return result;
}

}  // namespace

void validate_configuration(const json& catalog, const json& authorities) {
  if (catalog.is_object() && catalog.empty()) {
    if (authorities.is_object() && authorities.empty()) return;
    validate_authorities(authorities);
    return;
  }
  validate_catalog(catalog);
  validate_authorities(authorities);
  for (const auto& allocation : catalog.at("allocations")) {
    const auto& values = authorities.at("authorities");
    const bool bound = std::any_of(values.begin(), values.end(), [&](const auto& authority) {
      return authority.at("id") == allocation.at("authorityId") &&
             authority.at("workerId") == allocation.at("workerId") &&
             authority.at("allocationId") == allocation.at("id") &&
             authority.at("generation") == allocation.at("generation");
    });
    if (!bound) throw FleetError(400, "build allocation has no matching configured authority");
  }
}

json policies(const json& catalog, const json& authorities, const json& request) {
  validate_authorities(authorities);
  const auto candidates = eligible_policies(catalog, request);
  json result = json::array();
  for (const auto& candidate : candidates) {
    const auto& allocation = candidate.at("allocation");
    for (const auto& authority : authorities.at("authorities"))
      if (authority.at("id") == allocation.at("authorityId") &&
          authority.at("workerId") == allocation.at("workerId") &&
          authority.at("allocationId") == allocation.at("id") &&
          authority.at("generation") == allocation.at("generation"))
        result.push_back(candidate);
  }
  return result;
}

std::string digest(const json& value) {
  const auto bytes = value.dump();
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned int length = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), output.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != 32)
    throw FleetError(503, "build identity digest unavailable");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (unsigned int i = 0; i < length; ++i) {
    result += hex[output[i] >> 4];
    result += hex[output[i] & 15];
  }
  return result;
}

bool typed(const json& record) {
  return record.value("kind", "") == "build-jobs" && record.contains("build");
}

json start_command(const json& record) {
  const auto& build = record.at("build");
  const auto& allocation = build.at("allocation");
  const auto id = record.at("id").get<std::string>() + "-start";
  return {{"schema", "hi/fleet/v1"},
          {"targetKind", "build-jobs"},
          {"targetId", record.at("id")},
          {"workerId", allocation.at("workerId")},
          {"generation", allocation.at("generation")},
          {"operation", "start"},
          {"commandId", id},
          {"idempotencyKey", id},
          {"payload",
           {{"schema", "hi/fleet/build-command/v1"},
            {"attempt", build.at("attempt")},
            {"parent", record.at("parent")},
            {"policy", build.at("policy")},
            {"policyDigest", build.at("policyDigest")},
            {"snapshot", build.at("request").at("snapshot")},
            {"parametersDigest", build.at("parametersDigest")},
            {"snapshotWorkspace", build.at("snapshotWorkspace")},
            {"requiresRunGrant", true}}}};
}

namespace {
void same(const json& actual, const json& expected) {
  // JSON numeric equality permits 1.0 == 1. The retained wire identity does not.
  if (actual.dump() != expected.dump())
    throw FleetError(400, "stored build identity does not match its canonical intent");
}

void timestamp(const json& value, const char* name) {
  static const std::regex allowed("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z");
  if (!std::regex_match(text(value, name), allowed))
    throw FleetError(400, "invalid stored build timestamp");
}

void validate_retained_parent(const json& parent, const json& request, const json& workspace) {
  fields(parent, {"targetKind", "targetId", "sessionId", "executionId", "generation", "taskId",
                  "agentId", "claim"});
  auto execution = parent;
  for (const auto* field : {"taskId", "agentId", "claim"}) execution.erase(field);
  same(execution, request);
  identifier(parent, "taskId");
  identifier(parent, "agentId");
  const auto& claim = parent.at("claim");
  fields(claim,
         {"schema", "targetKind", "targetId", "workerId", "agentId", "generation", "workspace"});
  identifier(claim, "workerId");
  same(claim, {{"schema", "hi/fleet/claim/v1"},
               {"targetKind", parent.at("targetKind")},
               {"targetId", parent.at("targetId")},
               {"workerId", claim.at("workerId")},
               {"agentId", parent.at("agentId")},
               {"generation", parent.at("generation")},
               {"workspace", workspace.at("parentWorkspace")}});
}

void validate_retained_policy(const json& build) {
  const auto& policy = build.at("policy");
  fields(policy, {"workspace", "recipe", "allocation"});
  // Reuse the admission shape and capacity rules without manufacturing a current
  // authority. Recovery authenticates separately against retained operator keys.
  const json catalog = {{"schema", "hi/fleet/build-catalog/v1"},
                        {"workspaces", json::array({policy.at("workspace")})},
                        {"recipes", json::array({policy.at("recipe")})},
                        {"allocations", json::array({policy.at("allocation")})}};
  same(eligible_policies(catalog, build.at("request")), json::array({policy}));
  same(build.at("policyDigest"), digest(policy));
  same(build.at("parametersDigest"), digest(build.at("request").at("parameters")));
  same(build.at("allocation"), policy.at("allocation"));
}

void validate_retained_header(const json& record) {
  auto header = record;
  for (const auto* field : {"activity", "observationState"}) {
    if (header.contains(field)) {
      text(header, field);
      header.erase(field);
    }
  }
  fields(header, {"schema", "kind", "id", "status", "claimStatus", "generation", "createdAt",
                  "updatedAt", "parent", "build", "collectionVerified", "commandId"});
  same(record.at("schema"), "hi/fleet/v1");
  same(record.at("kind"), "build-jobs");
  same(record.at("claimStatus"), "unclaimed");
  same(record.at("collectionVerified"), false);
  identifier(record, "id");
  identifier(record, "commandId");
  integer(record.at("generation"));
  timestamp(record, "createdAt");
  timestamp(record, "updatedAt");
  auto base = record.at("build");
  for (const auto* field : {"grant", "cancellation", "terminal", "evidenceState"})
    base.erase(field);
  fields(base, {"schema", "request", "policy", "policyDigest", "parametersDigest", "allocation",
                "attempt", "reservation", "snapshotWorkspace"});
  same(base.at("schema"), "hi/fleet/build/v1");
  validate_submission(base.at("request"));
  validate_retained_policy(base);
  same(record.at("generation"), base.at("allocation").at("generation"));
  same(base.at("attempt"), 1);
  const auto id = "build-" + digest({{"workspaceId", base.at("request").at("workspaceId")},
                                     {"idempotencyKey", base.at("request").at("idempotencyKey")}});
  same(record.at("id"), id);
  same(base.at("snapshotWorkspace"), id + "-attempt-1");
  validate_retained_parent(record.at("parent"), base.at("request").at("parent"),
                           base.at("policy").at("workspace"));
}

void validate_retained_history(const json& document, const json& transitions) {
  const auto& record = document.at("record");
  const auto& events = document.at("events");
  if (!events.is_array() || events.size() != transitions.size())
    throw FleetError(400, "stored build transition history is incomplete");
  std::uint64_t previous = 0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& event = events.at(i);
    fields(event, {"seq", "kind", "targetId", "event", "at", "buildTransition"});
    const auto seq = integer(event.at("seq"));
    if (seq <= previous) throw FleetError(400, "stored build transition order is invalid");
    previous = seq;
    same(event.at("kind"), "build-jobs");
    same(event.at("targetId"), record.at("id"));
    same(event.at("event"), transitions.at(i).at("event"));
    same(event.at("buildTransition"), transitions.at(i).at("state"));
    timestamp(event, "at");
  }
  same(events.back().at("at"), record.at("updatedAt"));
}
}  // namespace

void validate_document(const json& document) {
  fields(document, {"schema", "kind", "record", "commands", "events"});
  same(document.at("schema"), "hi/fleet/v1");
  same(document.at("kind"), "build-jobs");
  const auto& record = document.at("record");
  validate_retained_header(record);
  const auto& build = record.at("build");
  const auto start = start_command(record);
  json commands =
      json::array({{{"command", start}, {"status", "pending"}, {"facts", json::array()}}});
  json transitions = json::array();
  std::string status = "admitted";
  std::string reservation = "reserved";
  const auto transition = [&](const char* event) {
    transitions.push_back(
        {{"event", event}, {"state", {{"status", status}, {"reservation", reservation}}}});
  };
  transition("build.admitted");
  if (build.contains("grant")) {
    const auto& grant = build.at("grant");
    fields(grant, {"schema", "claim", "grantId", "authorizedAt"});
    same(grant.at("schema"), "hi/fleet/build-grant/v1");
    validate_claim(grant.at("claim"), record, start);
    same(grant.at("grantId"), digest({{"buildId", record.at("id")}, {"claim", grant.at("claim")}}));
    timestamp(grant, "authorizedAt");
    status = "authorized";
    transition("build.run_authorized");
  }
  if (build.contains("cancellation")) {
    const auto& request = build.at("cancellation");
    validate_cancel(request, record);
    auto stop = start;
    stop["operation"] = "cancel";
    stop["commandId"] = request.at("commandId");
    stop["idempotencyKey"] = request.at("idempotencyKey");
    stop["payload"]["stopStartCommandId"] = start.at("commandId");
    if (stop.at("commandId") == start.at("commandId"))
      throw FleetError(400, "stored build stop reused its start identity");
    commands.at(0)["status"] = "superseded";
    commands.push_back({{"command", stop}, {"status", "pending"}, {"facts", json::array()}});
    status = "cancelling";
    transition("build.cancel_requested");
  }
  same(record.at("commandId"), commands.back().at("command").at("commandId"));
  if (build.contains("terminal")) {
    const auto& fact = build.at("terminal");
    validate_terminal(fact, record);
    if (status == "cancelling") {
      if (fact.at("outcome") != "cancelled" || !fact.at("startFenced").get<bool>())
        throw FleetError(400, "stored cancellation lacks a start fence");
    } else if (status != "authorized" || fact.at("outcome") == "cancelled") {
      throw FleetError(400, "stored terminal result lacks run authorization");
    }
    same(build.at("evidenceState"),
         fact.at("receipt").is_null() || fact.at("logs").is_null() || fact.at("artifacts").is_null()
             ? "incomplete"
             : "unverified");
    status = fact.at("outcome").get<std::string>();
    reservation = "released";
    commands.back()["status"] = "completed";
    transition("build.terminal");
  } else if (build.contains("evidenceState")) {
    throw FleetError(400, "stored evidence state lacks its terminal fact");
  }
  same(record.at("status"), status);
  same(build.at("reservation"), reservation);
  same(document.at("commands"), commands);
  validate_retained_history(document, transitions);
}

void authorize(const json& record, const json& authorities, const std::string& key) {
  if (!typed(record)) throw FleetError(409, "target is not a subordinate build");
  try {
    validate_authorities(authorities);
  } catch (const FleetError&) {
    throw FleetError(403, "build supervisor authority unavailable");
  } catch (const json::exception&) {
    throw FleetError(403, "build supervisor authority unavailable");
  }
  const auto& allocation = record.at("build").at("allocation");
  for (const auto& authority : authorities.at("authorities")) {
    if (authority.at("id") != allocation.at("authorityId") ||
        authority.at("workerId") != allocation.at("workerId") ||
        authority.at("allocationId") != allocation.at("id") ||
        authority.at("generation") != allocation.at("generation"))
      continue;
    const auto expected = text(authority, "key", 1024);
    if (key.size() == expected.size() &&
        CRYPTO_memcmp(key.data(), expected.data(), key.size()) == 0)
      return;
  }
  throw FleetError(403, "build supervisor authority does not match");
}

void validate_claim(const json& claim, const json& record, const json& command) {
  fields(claim,
         {"schema", "workerId", "allocationId", "generation", "attempt", "commandId", "claimId"});
  if (claim.at("schema") != "hi/fleet/build-claim/v1")
    throw FleetError(400, "unsupported build claim schema");
  for (const auto* name : {"workerId", "allocationId", "commandId", "claimId"})
    identifier(claim, name);
  integer(claim.at("generation"));
  integer(claim.at("attempt"));
  const auto& build = record.at("build");
  const auto& allocation = build.at("allocation");
  if (claim.at("workerId") != allocation.at("workerId") ||
      claim.at("allocationId") != allocation.at("id") ||
      claim.at("generation") != allocation.at("generation") ||
      claim.at("attempt") != build.at("attempt") ||
      claim.at("commandId") != command.at("commandId"))
    throw FleetError(409, "build claim does not match the retained attempt");
}

void validate_cancel(const json& request, const json& record) {
  fields(request, {"schema", "commandId", "idempotencyKey", "generation", "attempt"});
  if (request.at("schema") != "hi/fleet/build-cancel/v1")
    throw FleetError(400, "unsupported build cancellation schema");
  identifier(request, "commandId");
  identifier(request, "idempotencyKey");
  integer(request.at("generation"));
  integer(request.at("attempt"));
  if (request.at("generation") != record.at("generation") ||
      request.at("attempt") != record.at("build").at("attempt"))
    throw FleetError(409, "cancellation does not match retained build attempt");
}

void validate_delivery(const json& request, const json& record, const json& command) {
  fields(request, {"schema", "commandId", "generation", "attempt"});
  if (request.at("schema") != "hi/fleet/build-delivery/v1")
    throw FleetError(400, "unsupported build delivery schema");
  identifier(request, "commandId");
  integer(request.at("generation"));
  integer(request.at("attempt"));
  if (request.at("generation") != record.at("generation") ||
      request.at("attempt") != record.at("build").at("attempt") ||
      request.at("commandId") != command.at("commandId"))
    throw FleetError(409, "delivery must retain the exact admitted command");
}

void validate_terminal(const json& fact, const json& record) {
  fields(fact, {"schema",           "eventId",        "workerId",  "allocationId",
                "generation",       "attempt",        "commandId", "policyDigest",
                "parametersDigest", "snapshotDigest", "platform",  "imageDigest",
                "toolchainDigest",  "outcome",        "exitCode",  "cleanup",
                "startFenced",      "receipt",        "logs",      "artifacts"});
  if (fact.at("schema") != "hi/fleet/build-fact/v1")
    throw FleetError(400, "unsupported build fact schema");
  for (const auto* name : {"eventId", "workerId", "allocationId", "commandId"})
    identifier(fact, name);
  integer(fact.at("generation"));
  integer(fact.at("attempt"));
  for (const auto* name : {"policyDigest", "parametersDigest", "snapshotDigest"}) hash(fact, name);
  identities(fact);
  if (!fact.at("startFenced").is_boolean())
    throw FleetError(400, "start fence must be an exact boolean");
  for (const auto* name : {"receipt", "logs", "artifacts"}) {
    const auto& reference = fact.at(name);
    if (!reference.is_null()) {
      fields(reference, {"reference", "digest"});
      identifier(reference, "reference");
      hash(reference, "digest");
    }
  }
  const auto& build = record.at("build");
  const auto& allocation = build.at("allocation");
  bool matches =
      fact.at("workerId") == allocation.at("workerId") &&
      fact.at("allocationId") == allocation.at("id") &&
      fact.at("generation") == allocation.at("generation") &&
      fact.at("attempt") == build.at("attempt") && fact.at("commandId") == record.at("commandId") &&
      fact.at("policyDigest") == build.at("policyDigest") &&
      fact.at("parametersDigest") == build.at("parametersDigest") &&
      fact.at("snapshotDigest") == build.at("request").at("snapshot").at("manifestDigest");
  for (const auto* name : {"platform", "imageDigest", "toolchainDigest"})
    matches = matches && fact.at(name) == allocation.at(name);
  if (!matches || fact.at("cleanup") != "confirmed_empty")
    throw FleetError(409, "terminal fact does not confirm this attempt and its cleanup");
  const auto outcome = text(fact, "outcome");
  const auto& exit = fact.at("exitCode");
  if (outcome == "completed" || outcome == "failed") {
    if (!exit.is_number_integer() || exit < -255 || exit > 255 ||
        (outcome == "completed" ? exit != 0 : exit == 0))
      throw FleetError(400, "exit code does not match terminal outcome");
  } else if ((outcome != "cancelled" && outcome != "timed_out") || !exit.is_null()) {
    throw FleetError(400, "unsupported terminal outcome");
  }
}
}  // namespace agamemnon::fleet_build
