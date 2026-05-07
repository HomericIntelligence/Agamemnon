#pragma once

#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

using json = nlohmann::json;

/// Generate a UUID-like string using <random>.
std::string generate_uuid();

/// Get current ISO 8601 timestamp (UTC).
std::string now_iso8601();

/// Thread-safe in-memory store for agents, teams, tasks, and faults.
class Store {
 public:
  // ── Agents ─────────────────────────────────────────────────────────────
  json create_agent(const json& body);
  json get_agent(const std::string& id);
  json get_agent_by_name(const std::string& name);
  json list_agents();
  json update_agent(const std::string& id, const json& fields);
  bool delete_agent(const std::string& id);
  json start_agent(const std::string& id);
  json stop_agent(const std::string& id);

  // ── Teams ──────────────────────────────────────────────────────────────
  json create_team(const json& body);
  json get_team(const std::string& id);
  json list_teams();
  json update_team(const std::string& id, const json& body);
  bool delete_team(const std::string& id);

  // ── Tasks ──────────────────────────────────────────────────────────────
  json create_task(const std::string& team_id, const json& body);
  json get_task(const std::string& team_id, const std::string& task_id);
  json update_task(const std::string& team_id, const std::string& task_id, const json& body);
  json list_tasks_for_team(const std::string& team_id);
  json list_all_tasks();
  void mark_task_completed(const std::string& task_id);

  // ── Chaos faults ───────────────────────────────────────────────────────
  json list_faults();
  json create_fault(const std::string& type);
  bool remove_fault(const std::string& id);

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, json> agents_;
  std::unordered_map<std::string, json> teams_;
  std::unordered_map<std::string, json> tasks_;
  std::unordered_map<std::string, json> faults_;
};

}  // namespace projectagamemnon
