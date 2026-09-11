#include "agamemnon/projects.hpp"

#include "agamemnon/hmas_types.hpp"
#include "agamemnon/store.hpp"

#include <map>
#include <set>

namespace agamemnon {
namespace {
constexpr const char* kFields = R"(query FleetProjectFields($projectId:ID!,$after:String){
  node(id:$projectId){... on ProjectV2{id fields(first:100,after:$after){
    nodes{__typename ... on ProjectV2SingleSelectField{id options{id}}
      ... on ProjectV2Field{id dataType}}
    pageInfo{hasNextPage endCursor}}}}})";
constexpr const char* kItems = R"(query FleetProjectItems($projectId:ID!,$after:String){
  node(id:$projectId){... on ProjectV2{items(first:100,after:$after){
    nodes{id isArchived content{... on Issue{id}}
      fieldValues(first:100){nodes{
        ... on ProjectV2ItemFieldSingleSelectValue{optionId field{... on ProjectV2FieldCommon{id}}}
        ... on ProjectV2ItemFieldTextValue{text field{... on ProjectV2FieldCommon{id}}}}
        pageInfo{hasNextPage endCursor}}}
    pageInfo{hasNextPage endCursor}}}}})";
constexpr const char* kWork = R"(query FleetWorkIssue($owner:String!,$repo:String!,$number:Int!){
  repository(owner:$owner,name:$repo){issue(number:$number){url
    labels(first:100){nodes{name} pageInfo{hasNextPage endCursor}}
    closedByPullRequestsReferences(first:100,includeClosedPrs:true){
      nodes{url} pageInfo{hasNextPage endCursor}}}}})";
constexpr const char* kAdd = R"(mutation FleetProjectAdd($projectId:ID!,$contentId:ID!){
  addProjectV2ItemById(input:{projectId:$projectId,contentId:$contentId}){item{id}}})";
constexpr const char* kUpdate = R"(mutation FleetProjectUpdate(
  $projectId:ID!,$itemId:ID!,$fieldId:ID!,$value:ProjectV2FieldValue!){
  updateProjectV2ItemFieldValue(input:{projectId:$projectId,itemId:$itemId,
    fieldId:$fieldId,value:$value}){projectV2Item{id}}})";
constexpr const char* kClear =
    R"(mutation FleetProjectClear($projectId:ID!,$itemId:ID!,$fieldId:ID!){
  clearProjectV2ItemFieldValue(input:{projectId:$projectId,itemId:$itemId,fieldId:$fieldId}){
    projectV2Item{id}}})";

bool identifier(const json& value) {
  if (!value.is_string()) return false;
  const auto text = value.get<std::string>();
  return !text.empty() && text.size() <= 256 && text.find_first_of("\r\n\t ") == std::string::npos;
}
void validate_config(const json& config) {
  if (!config.is_object() || config.value("schema", "") != "hi/projects-projection/v1" ||
      !config.contains("projectId") || !identifier(config["projectId"]) ||
      !config.contains("stateFieldId") || !identifier(config["stateFieldId"]) ||
      !config.contains("stateOptions") || !config["stateOptions"].is_object() ||
      config["stateOptions"].size() != 7)
    throw std::invalid_argument("invalid_project_mapping");
  for (const auto* state :
       {"Pending", "Decomposing", "Delegated", "InProgress", "Escalated", "Completed", "Failed"})
    if (!config["stateOptions"].contains(state) || !identifier(config["stateOptions"][state]))
      throw std::invalid_argument("invalid_project_mapping");
  std::set<std::string> fields{config["stateFieldId"].get<std::string>()};
  for (const auto* key : {"stageFieldId", "workLinksFieldId"}) {
    if (config.contains(key) &&
        (!identifier(config[key]) || !fields.insert(config[key].get<std::string>()).second))
      throw std::invalid_argument("invalid_project_mapping");
  }
  if (config.contains("stageFieldId") != config.contains("stageOptions"))
    throw std::invalid_argument("invalid_project_mapping");
  if (config.contains("stageOptions")) {
    if (!config["stageOptions"].is_object() || config["stageOptions"].empty())
      throw std::invalid_argument("invalid_project_mapping");
    for (const auto& [label, option] : config["stageOptions"].items())
      if (!label.starts_with("state:") || !identifier(option))
        throw std::invalid_argument("invalid_project_mapping");
  }
}
json nodes(const json& connection) {
  if (!connection.at("nodes").is_array()) throw std::runtime_error("invalid_connection");
  return connection["nodes"];
}
bool more(const json& connection) {
  return connection.at("pageInfo").at("hasNextPage").get<bool>();
}
json enumerate(IGitHubClient& github, const char* query, json variables, const char* key) {
  json result = json::array();
  variables["after"] = nullptr;
  std::set<std::string> cursors;
  for (int page = 0; page < 100; ++page) {
    const auto connection = github.graphql(query, variables).at("node").at(key);
    for (const auto& node : nodes(connection)) result.push_back(node);
    if (!more(connection)) return result;
    const auto cursor = connection.at("pageInfo").at("endCursor").get<std::string>();
    if (cursor.empty() || !cursors.insert(cursor).second)
      throw std::runtime_error("invalid_cursor");
    variables["after"] = cursor;
  }
  throw std::runtime_error("project_pagination_limit");
}
void validate_fields(const json& config, const json& fields) {
  std::map<std::string, json> by_id;
  for (const auto& field : fields)
    if (field.is_object() && field.contains("id")) by_id[field.at("id").get<std::string>()] = field;
  for (const auto* prefix : {"state", "stage"}) {
    const auto field_key = std::string(prefix) + "FieldId";
    if (!config.contains(field_key)) continue;
    const auto found = by_id.find(config[field_key].get<std::string>());
    if (found == by_id.end() ||
        found->second.value("__typename", "") != "ProjectV2SingleSelectField")
      throw std::invalid_argument("configured_field_unavailable");
    std::set<std::string> available;
    for (const auto& option : found->second.at("options"))
      available.insert(option.at("id").get<std::string>());
    for (const auto& [label, id] : config[std::string(prefix) + "Options"].items())
      if (!available.contains(id.get<std::string>()))
        throw std::invalid_argument("configured_option_unavailable");
  }
  if (config.contains("workLinksFieldId")) {
    const auto found = by_id.find(config["workLinksFieldId"].get<std::string>());
    if (found == by_id.end() || found->second.value("dataType", "") != "TEXT")
      throw std::invalid_argument("configured_links_field_unavailable");
  }
}
std::vector<std::pair<json, HmasTask>> sources(IGitHubClient& github) {
  std::vector<std::pair<json, HmasTask>> result;
  std::set<std::string> identities;
  for (const auto& issue : github.list_issues_including_closed("agamemnon-hmas-task")) {
    const auto body = issue.at("body").get<std::string>();
    auto begin = body.find("```json\n");
    if (begin == std::string::npos) throw std::runtime_error("invalid_canonical_record");
    begin += 8;
    const auto end = body.find("\n```", begin);
    if (end == std::string::npos) throw std::runtime_error("invalid_canonical_record");
    const auto document = json::parse(body.substr(begin, end - begin));
    auto task = hmas_task_from_json(document);
    if (task.id.empty() || !document.contains("state") || !identifier(issue.at("node_id")) ||
        !identities.insert(task.id).second)
      throw std::runtime_error("invalid_canonical_record");
    result.emplace_back(issue, std::move(task));
  }
  return result;
}
void work_links(IGitHubClient& github, const HmasTask& task, const json& config, json& row,
                std::map<std::string, json>& desired) {
  row["stageProjection"] = "unavailable";
  row["stageReason"] =
      config.contains("stageFieldId") ? "work_issue_unavailable" : "not_configured";
  row["pullRequestUrls"] = json::array();
  const auto slash = task.repo.find('/');
  if (slash == std::string::npos || task.issue <= 0) return;
  try {
    const auto work = github
                          .graphql(kWork, {{"owner", task.repo.substr(0, slash)},
                                           {"repo", task.repo.substr(slash + 1)},
                                           {"number", task.issue}})
                          .at("repository")
                          .at("issue");
    row["workIssueUrl"] = work.at("url");
    if (more(work.at("closedByPullRequestsReferences")))
      throw std::runtime_error("incomplete_work_links");
    std::set<std::string> links{work.at("url").get<std::string>()};
    for (const auto& pr : nodes(work.at("closedByPullRequestsReferences"))) {
      const auto url = pr.at("url").get<std::string>();
      if (links.insert(url).second) row["pullRequestUrls"].push_back(url);
    }
    if (config.contains("workLinksFieldId")) {
      std::string text;
      for (const auto& link : links) text += (text.empty() ? "" : "\n") + link;
      desired[config["workLinksFieldId"].get<std::string>()] = {{"text", text}};
    }
    row["workLinksProjection"] = "available";
    if (!config.contains("stageFieldId")) return;
    if (more(work.at("labels"))) throw std::runtime_error("incomplete_stage_labels");
    desired[config["stageFieldId"].get<std::string>()] = nullptr;
    std::set<std::string> stages;
    for (const auto& label : nodes(work.at("labels"))) {
      auto name = label.at("name").get<std::string>();
      if (name.starts_with("state:")) stages.insert(name);
    }
    if (stages.size() != 1) {
      row["stageReason"] = "missing_or_ambiguous_stage_label";
      return;
    }
    const auto& stage = *stages.begin();
    row["stageLabel"] = stage;
    if (!config["stageOptions"].contains(stage)) {
      row["stageReason"] = "unmapped_stage_label";
      return;
    }
    desired[config["stageFieldId"].get<std::string>()] = {
        {"singleSelectOptionId", config["stageOptions"][stage]}};
    row["stageProjection"] = "available";
    row.erase("stageReason");
  } catch (const std::exception&) {
    row["workLinksProjection"] = "unavailable";
    row["stageReason"] = "work_issue_read_failed_or_incomplete";
    row["retryable"] = true;
  }
}
}  // namespace

ProjectProjection::ProjectProjection(std::shared_ptr<IGitHubClient> github, json config)
    : github_(std::move(github)),
      config_(std::move(config)),
      status_{{"schema", "hi/projects-projection/v1"}, {"state", "disabled"},
              {"authority", "github-issues"},          {"direction", "issues-to-project"},
              {"stageProjection", "unavailable"},      {"items", json::array()}} {
  if (config_.is_null()) return;
  try {
    validate_config(config_);
    status_["projectId"] = config_["projectId"];
    status_["state"] = github_ ? "pending" : "unavailable";
    if (!github_) status_["error"] = "github_persistence_required";
  } catch (const std::exception&) {
    status_["state"] = "misconfigured";
    status_["error"] = "invalid_project_mapping";
  }
}
json ProjectProjection::health() const {
  std::lock_guard lock(status_mutex_);
  return status_;
}
json ProjectProjection::reconcile() {
  std::unique_lock run_lock(run_mutex_, std::try_to_lock);
  if (!run_lock.owns_lock()) return health();
  auto result = health();
  if (config_.is_null() || !github_ || result.value("error", "") == "invalid_project_mapping")
    return result;
  result["state"] = "running";
  result["lastAttemptAt"] = now_iso8601();
  result["projected"] = 0;
  result["unchanged"] = 0;
  result["failed"] = 0;
  result["unavailable"] = 0;
  result["items"] = json::array();
  result["stageProjection"] = "unavailable";
  result.erase("error");
  {
    std::lock_guard lock(status_mutex_);
    status_ = result;
  }
  try {
    const json variables{{"projectId", config_["projectId"]}};
    validate_fields(config_, enumerate(*github_, kFields, variables, "fields"));
    const auto records = sources(*github_);  // Complete source validation before any mutation.
    std::map<std::string, json> by_content;
    for (const auto& item : enumerate(*github_, kItems, variables, "items")) {
      if (!item.at("content").is_object() || !item["content"].contains("id")) continue;
      const auto content = item["content"]["id"].get<std::string>();
      if (!by_content.emplace(content, item).second)
        throw std::runtime_error("duplicate_project_items");
    }
    for (const auto& [issue, task] : records) {
      json row{{"taskId", task.id},
               {"orchestrationState", task_state_to_string(task.state)},
               {"orchestrationIssueUrl", issue.value("html_url", "")},
               {"repo", task.repo},
               {"issue", task.issue}};
      try {
        std::map<std::string, json> desired;
        desired[config_["stateFieldId"].get<std::string>()] = {
            {"singleSelectOptionId", config_["stateOptions"][task_state_to_string(task.state)]}};
        work_links(*github_, task, config_, row, desired);
        const auto content = issue.at("node_id").get<std::string>();
        auto found = by_content.find(content);
        json item;
        bool changed = false;
        if (found == by_content.end()) {
          const auto added =
              github_->graphql(kAdd, {{"projectId", config_["projectId"]}, {"contentId", content}});
          const auto item_id = added.at("addProjectV2ItemById").at("item").at("id");
          if (!identifier(item_id)) throw std::runtime_error("invalid_add_acknowledgment");
          item = {
              {"id", item_id},
              {"isArchived", false},
              {"fieldValues", {{"nodes", json::array()}, {"pageInfo", {{"hasNextPage", false}}}}}};
          changed = true;
        } else
          item = found->second;
        row["projectItemId"] = item.at("id");
        if (item.at("isArchived").get<bool>()) throw std::runtime_error("project_item_archived");
        if (more(item.at("fieldValues"))) throw std::runtime_error("incomplete_project_fields");
        std::map<std::string, json> existing;
        for (const auto& field : nodes(item.at("fieldValues"))) {
          if (!field.contains("field")) continue;
          const auto field_id = field.at("field").at("id").get<std::string>();
          if (field.contains("optionId"))
            existing[field_id] = {{"singleSelectOptionId", field["optionId"]}};
          if (field.contains("text")) existing[field_id] = {{"text", field["text"]}};
        }
        for (const auto& [field_id, value] : desired) {
          if (existing.contains(field_id) && existing[field_id] == value) continue;
          if (value.is_null() && !existing.contains(field_id)) continue;
          json arguments{
              {"projectId", config_["projectId"]}, {"itemId", item["id"]}, {"fieldId", field_id}};
          if (!value.is_null()) arguments["value"] = value;
          const auto updated = github_->graphql(value.is_null() ? kClear : kUpdate, arguments);
          const auto mutation =
              value.is_null() ? "clearProjectV2ItemFieldValue" : "updateProjectV2ItemFieldValue";
          if (updated.at(mutation).at("projectV2Item").at("id") != item["id"])
            throw std::runtime_error("invalid_update_acknowledgment");
          changed = true;
        }
        row["state"] = changed ? "projected" : "unchanged";
        const auto counter = changed ? "projected" : "unchanged";
        result[counter] = result[counter].get<int>() + 1;
      } catch (const std::exception&) {
        row["state"] = "failed";
        row["error"] = "projection_not_acknowledged";
        row["retryable"] = true;
        result["failed"] = result["failed"].get<int>() + 1;
      }
      if (row.value("retryable", false) ||
          (config_.contains("stageFieldId") &&
           row.value("stageProjection", "unavailable") != "available"))
        result["unavailable"] = result["unavailable"].get<int>() + 1;
      result["items"].push_back(row);
    }
    std::size_t available_stages = 0;
    for (const auto& row : result["items"])
      if (row.value("stageProjection", "unavailable") == "available") ++available_stages;
    result["stageProjection"] = available_stages == 0                        ? "unavailable"
                                : available_stages == result["items"].size() ? "available"
                                                                             : "partial";
    const bool healthy = result["failed"] == 0 && result["unavailable"] == 0;
    result["state"] = healthy ? "healthy" : "degraded";
    result["retryable"] = !healthy;
    if (healthy) result["lastSuccessAt"] = now_iso8601();
  } catch (const std::invalid_argument&) {
    result["state"] = "misconfigured";
    result["error"] = "configured_fields_or_options_unavailable";
    result["retryable"] = true;
  } catch (const std::exception&) {
    result["state"] = "degraded";
    result["error"] = "projection_source_read_failed";
    result["retryable"] = true;
  }
  {
    std::lock_guard lock(status_mutex_);
    status_ = result;
  }
  return result;
}
}  // namespace agamemnon
