#include "agamemnon/fleet_issue.hpp"

#include "agamemnon/auth.hpp"
#include "agamemnon/store.hpp"
#include "agamemnon/version.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <limits>
#include <openssl/evp.h>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "httplib.h"

namespace agamemnon {
namespace {
bool text(const json& value, std::size_t maximum) {
  if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
      value.get_ref<const std::string&>().size() > maximum)
    return false;
  try {
    (void)value.dump();
  } catch (const json::exception&) {
    return false;
  }
  return true;
}
bool match(const json& value, const char* pattern) {
  return value.is_string() && std::regex_match(value.get<std::string>(), std::regex(pattern));
}
std::string digest(const std::string& bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> value{};
  unsigned int size = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), value.data(), &size, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("issue_identity_unavailable");
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (unsigned int n = 0; n < size; ++n) result << std::setw(2) << static_cast<int>(value[n]);
  return result.str();
}
std::string lower(std::string value) {
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}
bool repository_name(const json& name) {
  return text(name, 255) && match(name, "[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+") &&
         name.get<std::string>().find("..") == std::string::npos &&
         !name.get<std::string>().starts_with("./") && !name.get<std::string>().ends_with("/.");
}
const json& entry(const IssueImportConfiguration& config, const std::string& key) {
  for (const auto& candidate : config.repositories)
    if (candidate.at("key").get<std::string>() == key) return candidate;
  throw std::invalid_argument("unknown_repository_selection");
}
json reference(const CanonicalWorkIssue& work) {
  return {{"repository", work.repository}, {"number", work.number}, {"url", work.url}};
}
json routing() {
  return {{"domain", "pipeline"}, {"hmasRole", "task-agent"}, {"stage", "implementation"}};
}
json resolve_plan(IGitHubClient& github, const CanonicalWorkIssue& work,
                  const std::optional<std::string>& node, ImportContext& context) {
  std::string body = work.body;
  if (node) {
    const auto data = github.import_plan_comment(*node, context);
    const auto comment = data.value("node", json());
    if (!comment.is_object() || comment.value("__typename", json()) != "IssueComment" ||
        comment.value("id", json()) != *node || !comment.value("issue", json()).is_object() ||
        comment["issue"].value("id", json()) != work.issue_id ||
        !comment.value("body", json()).is_string())
      throw std::runtime_error("invalid_plan_comment");
    body = comment["body"].get<std::string>();
  }
  context.checkpoint();
  if (body.empty() || body.size() > 128 * 1024) throw std::runtime_error("invalid_plan_snapshot");
  json result{{"kind", node ? "issue_comment" : "issue_body"}, {"digest", digest(body)}};
  if (node) result["nodeId"] = *node;
  return result;
}
json parse_request(const std::string& body) {
  std::vector<std::set<std::string>> keys;
  const auto request = json::parse(body, [&keys](int depth, json::parse_event_t event,
                                                 json& value) {
    if (depth > 16) throw std::invalid_argument("invalid_issue_import");
    if (event == json::parse_event_t::object_start) keys.emplace_back();
    if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
      throw std::invalid_argument("invalid_issue_import");
    if (event == json::parse_event_t::object_end) keys.pop_back();
    return true;
  });
  if (!request.is_object() || request.size() != 6 ||
      request.value("schema", json()) != "hi/agamemnon/issue-import/v1" ||
      !match(request.value("repositoryKey", json()), "[a-z0-9][a-z0-9_-]{0,63}") ||
      !text(request.value("repositoryId", json()), 128) ||
      !text(request.value("issueId", json()), 128) ||
      !request.value("issueNumber", json()).is_number_integer() || request["issueNumber"] <= 0 ||
      request["issueNumber"] > std::numeric_limits<int>::max())
    throw std::invalid_argument("invalid_issue_import");
  const auto plan = request.value("plan", json());
  if (!plan.is_object() || !match(plan.value("digest", json()), "[a-f0-9]{64}") ||
      !((plan.size() == 2 && plan.value("kind", json()) == "issue_body") ||
        (plan.size() == 3 && plan.value("kind", json()) == "issue_comment" &&
         text(plan.value("nodeId", json()), 128))))
    throw std::invalid_argument("invalid_issue_import");
  return request;
}
void reply(httplib::Response& response, const IssueImportResponse& result) {
  response.status = result.status;
  response.set_header("X-API-Version", std::string(kVersion));
  response.set_content(result.body.dump(), "application/json");
}
IssueImportResponse unavailable() { return {503, {{"error", "issue_import_unavailable"}}}; }
}  // namespace

void validate_issue_import_configuration(const IssueImportConfiguration& config) {
  if (config.state_branch.empty() || config.state_branch.size() > 128 ||
      !std::regex_match(config.state_branch, std::regex("[A-Za-z0-9_-][A-Za-z0-9_./-]*")) ||
      config.state_branch.find("..") != std::string::npos || config.state_branch.ends_with('/') ||
      config.state_branch.find("//") != std::string::npos || !config.repositories.is_array() ||
      config.repositories.empty() || config.repositories.size() > 64)
    throw std::invalid_argument("invalid_issue_import_configuration");
  std::set<std::string> keys, ids, names;
  for (const auto& item : config.repositories) {
    if (!item.is_object() || item.size() != 3 ||
        !match(item.value("key", json()), "[a-z0-9][a-z0-9_-]{0,63}") ||
        !repository_name(item.value("repository", json())) ||
        !text(item.value("repositoryId", json()), 128) ||
        !keys.insert(item["key"].get<std::string>()).second ||
        !ids.insert(item["repositoryId"].get<std::string>()).second ||
        !names.insert(lower(item["repository"].get<std::string>())).second)
      throw std::invalid_argument("invalid_issue_import_configuration");
  }
}

CanonicalWorkIssue resolve_work_issue(const IssueImportConfiguration& config, IGitHubClient& github,
                                      const std::string& repository, int number,
                                      ImportContext& context) {
  validate_issue_import_configuration(config);
  if (number <= 0) throw std::invalid_argument("invalid_work_issue");
  const json* registered = nullptr;
  for (const auto& item : config.repositories)
    if (lower(item["repository"].get<std::string>()) == lower(repository)) registered = &item;
  if (!registered) throw std::runtime_error("unregistered_work_repository");
  const auto canonical = registered->at("repository").get<std::string>();
  const auto slash = canonical.find('/');
  const auto data = github.import_work_issue(canonical.substr(0, slash),
                                             canonical.substr(slash + 1), number, context);
  context.checkpoint();
  const auto repo = data.value("repository", json());
  if (!repo.is_object() || repo.value("nameWithOwner", json()) != canonical ||
      repo.value("id", json()) != registered->at("repositoryId"))
    throw std::runtime_error("work_repository_identity_mismatch");
  const auto issue = repo.value("issue", json());
  if (issue.is_null()) throw std::out_of_range("issue_not_found");
  if (!issue.is_object() || issue.value("__typename", json()) != "Issue" ||
      !text(issue.value("id", json()), 128) || !issue.value("number", json()).is_number_integer() ||
      issue["number"] != number ||
      issue.value("url", json()) !=
          "https://github.com/" + canonical + "/issues/" + std::to_string(number) ||
      (issue.value("state", json()) != "OPEN" && issue.value("state", json()) != "CLOSED") ||
      !text(issue.value("title", json()), 1024) || !issue.value("body", json()).is_string())
    throw std::runtime_error("work_issue_identity_mismatch");
  return {canonical,
          registered->at("repositoryId").get<std::string>(),
          issue["id"].get<std::string>(),
          number,
          issue["url"].get<std::string>(),
          issue["state"] == "OPEN",
          issue["title"].get<std::string>(),
          issue["body"].get<std::string>()};
}

std::string import_work_key(const CanonicalWorkIssue& issue) {
  return digest(json{{"schema", "hi/agamemnon/issue-task-key/v1"},
                     {"forge", "github"},
                     {"repositoryId", issue.repository_id},
                     {"issueId", issue.issue_id}}
                    .dump());
}

void validate_issue_intake_provenance(const json& provenance, const std::string& task_id,
                                      const std::string& repository, int number) {
  const auto issue = provenance.value("issue", json());
  const auto plan = provenance.value("plan", json());
  if (!provenance.is_object() || provenance.size() != 8 ||
      provenance.value("schema", json()) != "hi/agamemnon/issue-intake/v1" ||
      provenance.value("forge", json()) != "github" ||
      !text(provenance.value("repositoryId", json()), 128) ||
      !text(provenance.value("issueId", json()), 128) ||
      !match(provenance.value("observedAt", json()),
             "[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])T([01][0-9]|2[0-3]):[0-5][0-9]:[0-5]"
             "[0-9]Z") ||
      provenance.value("routing", json()).dump() != routing().dump() || !issue.is_object() ||
      issue.size() != 3 || !repository_name(repository) || number <= 0 ||
      issue.value("repository", json()) != repository ||
      !issue.value("number", json()).is_number_integer() || issue["number"] != number ||
      issue.value("url", json()) !=
          "https://github.com/" + repository + "/issues/" + std::to_string(number) ||
      !plan.is_object() || !match(plan.value("digest", json()), "[a-f0-9]{64}") ||
      !((plan.size() == 2 && plan.value("kind", json()) == "issue_body") ||
        (plan.size() == 3 && plan.value("kind", json()) == "issue_comment" &&
         text(plan.value("nodeId", json()), 128))))
    throw std::invalid_argument("Invalid retained issue import provenance");
  CanonicalWorkIssue work{};
  work.repository_id = provenance["repositoryId"].get<std::string>();
  work.issue_id = provenance["issueId"].get<std::string>();
  if (task_id != "issue-" + import_work_key(work))
    throw std::invalid_argument("Invalid retained issue task identity");
}

FleetIssueService::FleetIssueService(Store& store,
                                     std::shared_ptr<const IssueImportConfiguration> configuration,
                                     const AuthMiddleware& auth)
    : store_(store), configuration_(std::move(configuration)) {
  httplib::Request request;
  request.path = "/v1/fleet/issue-intakes";
  if (!store_.github_client() || !configuration_ || auth.validate(request) ||
      !store_.import_configuration() ||
      store_.import_configuration()->repositories.dump() != configuration_->repositories.dump() ||
      store_.import_configuration()->state_branch != configuration_->state_branch)
    throw std::invalid_argument("issue_import_requires_persistence_and_authority");
  validate_issue_import_configuration(*configuration_);
}
IssueImportResponse FleetIssueService::repositories() const {
  return {200,
          {{"schema", "hi/agamemnon/issue-repositories/v1"},
           {"repositories", configuration_->repositories}}};
}
IssueImportResponse FleetIssueService::inspect(const std::string& key, int number,
                                               const std::optional<std::string>& comment) {
  try {
    const auto& selected = entry(*configuration_, key);
    ImportContext context;
    const auto work =
        resolve_work_issue(*configuration_, *store_.github_client(),
                           selected["repository"].get<std::string>(), number, context);
    const auto plan = resolve_plan(*store_.github_client(), work, comment, context);
    return {200,
            {{"schema", "hi/agamemnon/issue-inspection/v1"},
             {"repositoryKey", key},
             {"repositoryId", work.repository_id},
             {"issueId", work.issue_id},
             {"issue", reference(work)},
             {"title", work.title},
             {"state", work.open ? "open" : "closed"},
             {"plan", plan},
             {"observedAt", now_iso8601()}}};
  } catch (const std::invalid_argument&) {
    return {400, {{"error", "invalid_issue_selection"}}};
  } catch (const std::out_of_range&) {
    return {404, {{"error", "issue_not_found"}}};
  } catch (const std::exception&) {
    return {503, {{"error", "issue_lookup_unavailable"}}};
  }
}
IssueImportResponse FleetIssueService::import_request(const json& request) {
  try {
    if (request.dump().size() > 4096) return {413, {{"error", "invalid_issue_import"}}};
    (void)parse_request(request.dump());
    const auto& selected = entry(*configuration_, request["repositoryKey"].get<std::string>());
    if (request["repositoryId"] != selected["repositoryId"])
      return {400, {{"error", "invalid_issue_selection"}}};
  } catch (const std::exception&) {
    return {400, {{"error", "invalid_issue_import"}}};
  }
  try {
    const auto& selected = entry(*configuration_, request["repositoryKey"].get<std::string>());
    ImportContext context;
    const auto work = resolve_work_issue(*configuration_, *store_.github_client(),
                                         selected["repository"].get<std::string>(),
                                         request["issueNumber"].get<int>(), context);
    std::optional<std::string> comment;
    if (request["plan"]["kind"] == "issue_comment")
      comment = request["plan"]["nodeId"].get<std::string>();
    const auto plan = resolve_plan(*store_.github_client(), work, comment, context);
    if (work.issue_id != request["issueId"].get<std::string>() ||
        plan.dump() != request["plan"].dump())
      return {409, {{"error", "issue_reference_changed"}}};
    HmasTask proposed{};
    proposed.id = "issue-" + import_work_key(work);
    proposed.layer = HmasLayer::L3_TaskAgent;
    proposed.state = TaskState::Pending;
    proposed.subject = "Planned issue intake";
    proposed.description = "Execute the referenced canonical planned issue.";
    proposed.repo = work.repository;
    proposed.issue = work.number;
    proposed.created_at = now_iso8601();
    proposed.delivery["issueIntake"] = {{"schema", "hi/agamemnon/issue-intake/v1"},
                                        {"forge", "github"},
                                        {"repositoryId", work.repository_id},
                                        {"issueId", work.issue_id},
                                        {"issue", reference(work)},
                                        {"plan", plan},
                                        {"routing", routing()},
                                        {"observedAt", proposed.created_at}};
    const auto [task, created] = store_.import_issue_task(proposed, work, context);
    const auto provenance = task.delivery.at("issueIntake");
    return {created ? 201 : 200,
            {{"schema", "hi/agamemnon/issue-import-receipt/v1"},
             {"taskId", task.id},
             {"state", task_state_to_string(task.state)},
             {"provenance", provenance},
             {"issue", provenance["issue"]},
             {"routing", provenance["routing"]}}};
  } catch (const std::out_of_range&) {
    return {404, {{"error", "issue_not_found"}}};
  } catch (const std::invalid_argument& error) {
    return {409,
            {{"error", std::string(error.what()) == "work_issue_already_imported"
                           ? "work_issue_already_imported"
                           : "issue_import_conflict"}}};
  } catch (const std::exception&) {
    return {503, {{"error", "issue_persistence_uncertain"}}};
  }
}

void register_fleet_issue_routes(httplib::Server& server,
                                 std::shared_ptr<FleetIssueService> service) {
  server.Get("/v1/fleet/issue-intakes/repositories",
             [service](const auto& request, auto& response) {
               if (!request.params.empty()) {
                 reply(response, {400, {{"error", "invalid_issue_selection"}}});
                 return;
               }
               reply(response, service ? service->repositories() : unavailable());
             });
  server.Get(
      R"(/v1/fleet/issue-intakes/([^/]+)/([^/]+))", [service](const auto& request, auto& response) {
        int number = 0;
        const std::string raw = request.matches[2];
        const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), number);
        const std::string key = request.matches[1];
        if (error != std::errc{} || end != raw.data() + raw.size() || number <= 0 ||
            std::to_string(number) != raw ||
            !std::regex_match(key, std::regex("[a-z0-9][a-z0-9_-]{0,63}")) ||
            request.params.size() > 1 ||
            (!request.params.empty() && request.params.begin()->first != "planCommentId")) {
          reply(response, {400, {{"error", "invalid_issue_selection"}}});
          return;
        }
        std::optional<std::string> comment;
        if (!request.params.empty()) {
          comment = request.params.begin()->second;
          if (!text(*comment, 128)) {
            reply(response, {400, {{"error", "invalid_issue_selection"}}});
            return;
          }
        }
        reply(response, service ? service->inspect(key, number, comment) : unavailable());
      });
  server.Post("/v1/fleet/issue-intakes", [service](const auto& request, auto& response) {
    if (!request.params.empty()) {
      reply(response, {400, {{"error", "invalid_issue_import"}}});
      return;
    }
    if (request.body.size() > 4096) {
      reply(response, {413, {{"error", "invalid_issue_import"}}});
      return;
    }
    json parsed;
    try {
      parsed = parse_request(request.body);
    } catch (const std::exception&) {
      reply(response, {400, {{"error", "invalid_issue_import"}}});
      return;
    }
    reply(response, service ? service->import_request(parsed) : unavailable());
  });
}
}  // namespace agamemnon
