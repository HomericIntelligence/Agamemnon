#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"
#include "projectagamemnon/github_client.hpp"

namespace projectagamemnon {

using json = nlohmann::json;

/// Generate a UUID-like string using <random>.
std::string generate_uuid();

/// Get current ISO 8601 timestamp (UTC).
std::string now_iso8601();

/// Thread-safe store for agents, teams, tasks, and faults.
/// In-memory maps act as a write-through cache backed by GitHub Issues.
/// Pass nullptr (or use the default constructor) for pure in-memory mode.
class Store {
 public:
  explicit Store(std::shared_ptr<IGitHubClient> gh = nullptr) : gh_(std::move(gh)) {}

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
  std::shared_ptr<IGitHubClient> gh_;
  std::mutex mutex_;

  std::unordered_map<std::string, json> agents_;
  std::unordered_map<std::string, json> teams_;
  std::unordered_map<std::string, json> tasks_;
  std::unordered_map<std::string, json> faults_;

  bool agents_loaded_{false};
  bool teams_loaded_{false};
  bool tasks_loaded_{false};
  bool faults_loaded_{false};

  // Called while holding mutex_; loads entity type from GitHub on first access.
  void ensure_agents_loaded_();
  void ensure_teams_loaded_();
  void ensure_tasks_loaded_();
  void ensure_faults_loaded_();

  // Parses the JSON payload embedded in an issue body; returns nullptr on failure.
  static json parse_issue_entity_(const json& issue);

  // Builds a GitHub issue body containing a labelled JSON block.
  static std::string make_issue_body_(std::string_view entity_type, const json& entity);
};

}  // namespace projectagamemnon
