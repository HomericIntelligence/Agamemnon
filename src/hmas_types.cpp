#include "projectagamemnon/hmas_types.hpp"

#include <stdexcept>

namespace projectagamemnon {

std::string hmas_layer_to_string(HmasLayer layer) {
  switch (layer) {
    case HmasLayer::L0_ChiefArchitect: return "L0_ChiefArchitect";
    case HmasLayer::L1_ComponentLead:  return "L1_ComponentLead";
    case HmasLayer::L2_ModuleLead:     return "L2_ModuleLead";
    case HmasLayer::L3_TaskAgent:      return "L3_TaskAgent";
  }
  return "unknown";
}

std::string task_state_to_string(TaskState state) {
  switch (state) {
    case TaskState::Pending:      return "Pending";
    case TaskState::Decomposing:  return "Decomposing";
    case TaskState::Delegated:    return "Delegated";
    case TaskState::InProgress:   return "InProgress";
    case TaskState::Escalated:    return "Escalated";
    case TaskState::Completed:    return "Completed";
    case TaskState::Failed:       return "Failed";
  }
  return "unknown";
}

HmasLayer hmas_layer_from_string(const std::string& s) {
  if (s == "L0_ChiefArchitect") return HmasLayer::L0_ChiefArchitect;
  if (s == "L1_ComponentLead")  return HmasLayer::L1_ComponentLead;
  if (s == "L2_ModuleLead")     return HmasLayer::L2_ModuleLead;
  if (s == "L3_TaskAgent")      return HmasLayer::L3_TaskAgent;
  throw std::invalid_argument("Unknown HmasLayer: " + s);
}

TaskState task_state_from_string(const std::string& s) {
  if (s == "Pending")     return TaskState::Pending;
  if (s == "Decomposing") return TaskState::Decomposing;
  if (s == "Delegated")   return TaskState::Delegated;
  if (s == "InProgress")  return TaskState::InProgress;
  if (s == "Escalated")   return TaskState::Escalated;
  if (s == "Completed")   return TaskState::Completed;
  if (s == "Failed")      return TaskState::Failed;
  throw std::invalid_argument("Unknown TaskState: " + s);
}

json escalation_record_to_json(const EscalationRecord& rec) {
  return {{"task_id", rec.task_id},
          {"reason", rec.reason},
          {"escalated_at", rec.escalated_at},
          {"from_layer", hmas_layer_to_string(rec.from_layer)}};
}

json hmas_task_to_json(const HmasTask& task) {
  json j;
  j["id"]               = task.id;
  j["brief_id"]         = task.brief_id;
  j["parent_task_id"]   = task.parent_task_id;
  j["layer"]            = hmas_layer_to_string(task.layer);
  j["state"]            = task_state_to_string(task.state);
  j["subject"]          = task.subject;
  j["description"]      = task.description;
  j["repo"]             = task.repo;
  j["module"]           = task.module;
  j["assigned_lead_id"] = task.assigned_lead_id;
  j["blocked_by"]       = task.blocked_by;
  j["child_task_ids"]   = task.child_task_ids;
  j["created_at"]       = task.created_at;
  j["completed_at"]     = task.completed_at;

  json escalations_arr = json::array();
  for (const auto& e : task.escalations) escalations_arr.push_back(escalation_record_to_json(e));
  j["escalations"] = escalations_arr;

  return j;
}

json task_brief_to_json(const TaskBrief& brief) {
  json j;
  j["id"]          = brief.id;
  j["title"]       = brief.title;
  j["description"] = brief.description;
  j["repos"]       = brief.repos;

  json modules_obj = json::object();
  for (const auto& [repo, mods] : brief.modules) modules_obj[repo] = mods;
  j["modules"] = modules_obj;

  json impls_obj = json::object();
  for (const auto& [repo, mod_map] : brief.impls) {
    json repo_obj = json::object();
    for (const auto& [mod, tasks] : mod_map) repo_obj[mod] = tasks;
    impls_obj[repo] = repo_obj;
  }
  j["impls"] = impls_obj;

  return j;
}

TaskBrief task_brief_from_json(const json& j) {
  TaskBrief brief;
  brief.id          = j.value("id", "");
  brief.title       = j.value("title", "");
  brief.description = j.value("description", "");

  if (j.contains("repos") && j["repos"].is_array()) {
    for (const auto& r : j["repos"]) brief.repos.push_back(r.get<std::string>());
  }

  if (j.contains("modules") && j["modules"].is_object()) {
    for (auto& [repo, mods] : j["modules"].items()) {
      if (mods.is_array()) {
        for (const auto& m : mods) brief.modules[repo].push_back(m.get<std::string>());
      }
    }
  }

  if (j.contains("impls") && j["impls"].is_object()) {
    for (auto& [repo, mod_map] : j["impls"].items()) {
      if (mod_map.is_object()) {
        for (auto& [mod, tasks] : mod_map.items()) {
          if (tasks.is_array()) {
            for (const auto& t : tasks) brief.impls[repo][mod].push_back(t.get<std::string>());
          }
        }
      }
    }
  }

  return brief;
}

}  // namespace projectagamemnon
