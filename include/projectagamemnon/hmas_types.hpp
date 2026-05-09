#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

using json = nlohmann::json;

// ── Enums ─────────────────────────────────────────────────────────────────────

enum class HmasLayer : int {
  L0_ChiefArchitect = 0,  // inter-repo decomposition
  L1_ComponentLead = 1,   // per-repo coordination
  L2_ModuleLead = 2,      // per-module delegation
  L3_TaskAgent = 3,       // leaf impl tasks for myrmidons
};

enum class TaskState : int {
  Pending = 0,
  Decomposing = 1,
  Delegated = 2,
  InProgress = 3,
  Escalated = 4,
  Completed = 5,
  Failed = 6,
};

// ── Structs ───────────────────────────────────────────────────────────────────

struct EscalationRecord {
  std::string task_id;
  std::string reason;
  std::string escalated_at;
  HmasLayer from_layer;
};

struct HmasTask {
  std::string id;
  std::string brief_id;        // root brief this task belongs to
  std::string parent_task_id;  // empty for L0 root
  HmasLayer layer;
  TaskState state;
  std::string subject;
  std::string description;
  std::string repo;                     // relevant repository (L1+)
  std::string module;                   // relevant module (L2+)
  std::string assigned_lead_id;         // agent assigned at this layer
  std::vector<std::string> blocked_by;  // task IDs this task depends on
  std::vector<std::string> child_task_ids;
  std::vector<EscalationRecord> escalations;
  std::string created_at;
  std::string completed_at;  // empty until completed
};

struct TaskBrief {
  std::string id;  // generated on submit
  std::string title;
  std::string description;
  std::vector<std::string> repos;
  // repo -> list of modules
  std::unordered_map<std::string, std::vector<std::string>> modules;
  // repo -> module -> list of impl tasks (descriptions)
  std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> impls;
};

// ── Serialization helpers ─────────────────────────────────────────────────────

std::string hmas_layer_to_string(HmasLayer layer);
std::string task_state_to_string(TaskState state);
HmasLayer hmas_layer_from_string(const std::string& s);
TaskState task_state_from_string(const std::string& s);

json hmas_task_to_json(const HmasTask& task);
json escalation_record_to_json(const EscalationRecord& rec);
json task_brief_to_json(const TaskBrief& brief);
TaskBrief task_brief_from_json(const json& j);

}  // namespace projectagamemnon
