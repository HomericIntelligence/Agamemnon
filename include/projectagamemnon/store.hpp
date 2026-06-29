#pragma once

#include "projectagamemnon/github_client.hpp"
#include "projectagamemnon/hmas_types.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

using json = nlohmann::json;

class MetricsRegistry;

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

  /// Attach a MetricsRegistry for instrumentation (nullable; pass nullptr to disable).
  void set_metrics(MetricsRegistry* metrics) noexcept { metrics_ = metrics; }

  // ── Agents ─────────────────────────────────────────────────────────────
  json create_agent(const json& body);
  json get_agent(const std::string& id);
  json get_agent_by_name(const std::string& name);
  // limit defaults to "all items"; offset is the number of items to skip.
  json list_agents(std::size_t limit = std::numeric_limits<std::size_t>::max(),
                   std::size_t offset = 0);
  json update_agent(const std::string& id, const json& fields);
  bool delete_agent(const std::string& id);
  json start_agent(const std::string& id);
  json stop_agent(const std::string& id);

  // ── Teams ──────────────────────────────────────────────────────────────
  json create_team(const json& body);
  json get_team(const std::string& id);
  json list_teams(std::size_t limit = std::numeric_limits<std::size_t>::max(),
                  std::size_t offset = 0);
  json update_team(const std::string& id, const json& body);
  bool delete_team(const std::string& id);

  // ── Tasks ──────────────────────────────────────────────────────────────
  json create_task(const std::string& team_id, const json& body);
  json get_task(const std::string& team_id, const std::string& task_id);
  json update_task(const std::string& team_id, const std::string& task_id, const json& body);
  json list_tasks_for_team(const std::string& team_id,
                           std::size_t limit = std::numeric_limits<std::size_t>::max(),
                           std::size_t offset = 0);
  json list_all_tasks(std::size_t limit = std::numeric_limits<std::size_t>::max(),
                      std::size_t offset = 0);
  void mark_task_completed(const std::string& task_id);

  // ── Chaos faults ───────────────────────────────────────────────────────
  json list_faults(std::size_t limit = std::numeric_limits<std::size_t>::max(),
                   std::size_t offset = 0);
  json create_fault(const std::string& type);
  bool remove_fault(const std::string& id);

  // ── HMAS typed tasks ───────────────────────────────────────────────────
  void create_hmas_task(const HmasTask& task);
  /// Returns a value copy of the task; safe to use outside the mutex.
  std::optional<HmasTask> get_hmas_task(const std::string& id);
  bool update_hmas_task_state(const std::string& id, TaskState state);
  /// Atomically update the task state and append an escalation record.
  bool update_hmas_task_state_and_record_escalation(const std::string& id, TaskState new_state,
                                                    const EscalationRecord& escalation);
  bool update_hmas_task(const HmasTask& task);
  std::vector<HmasTask> list_hmas_tasks_by_layer(HmasLayer layer);
  std::vector<HmasTask> list_hmas_tasks_by_parent(const std::string& parent_id);
  std::vector<HmasTask> list_hmas_tasks_by_brief(const std::string& brief_id);

  // ── TaskBriefs (HMAS root submissions) ─────────────────────────────────
  void create_task_brief(const TaskBrief& brief);
  std::optional<TaskBrief> get_task_brief(const std::string& id);
  std::vector<TaskBrief> list_task_briefs();

  // ── Inbound sync (#165) ───────────────────────────────────────────────
  bool apply_github_event(std::string_view entity_label, std::string_view action,
                          const json& issue_shape, std::string_view updated_at);
  std::size_t reconcile_from_github();

 private:
  std::shared_ptr<IGitHubClient> gh_;
  MetricsRegistry* metrics_ = nullptr;
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, json> agents_;
  std::unordered_map<std::string, json> teams_;
  std::unordered_map<std::string, json> tasks_;
  std::unordered_map<std::string, json> faults_;
  std::unordered_map<std::string, HmasTask> hmas_tasks_;
  // Secondary index: brief_id -> task ids. Maintained under the same write
  // lock as hmas_tasks_; read under shared_lock by list_hmas_tasks_by_brief.
  // #156: avoids O(n) full-map scan on every myrmidon completion.
  std::unordered_map<std::string, std::vector<std::string>> hmas_tasks_by_brief_;
  std::unordered_map<std::string, TaskBrief> task_briefs_;
  std::unordered_map<std::string, std::string> hmas_task_issue_numbers_;
  std::unordered_map<std::string, std::string> brief_issue_numbers_;

  // Atomic flags: checked outside the lock; once_flags guard the single fetch.
  std::atomic<bool> agents_loaded_{false};
  std::atomic<bool> teams_loaded_{false};
  std::atomic<bool> tasks_loaded_{false};
  std::atomic<bool> faults_loaded_{false};
  std::atomic<bool> hmas_tasks_loaded_{false};
  std::atomic<bool> briefs_loaded_{false};
  mutable std::once_flag agents_once_;
  mutable std::once_flag teams_once_;
  mutable std::once_flag tasks_once_;
  mutable std::once_flag faults_once_;
  mutable std::once_flag hmas_tasks_once_;
  mutable std::once_flag briefs_once_;

  // Called while holding mutex_; loads entity type from GitHub on first access.
  void ensure_agents_loaded_();
  void ensure_teams_loaded_();
  void ensure_tasks_loaded_();
  void ensure_faults_loaded_();
  void ensure_hmas_tasks_loaded_();
  void ensure_briefs_loaded_();

  // Parses the JSON payload embedded in an issue body; returns nullptr on failure.
  static json parse_issue_entity_(const json& issue);

  // Builds a GitHub issue body containing a labelled JSON block.
  static std::string make_issue_body_(std::string_view entity_type, const json& entity);

  // Returns the map matching the agamemnon-* label, or nullptr on unknown label.
  std::unordered_map<std::string, json>* pick_map_(std::string_view entity_label);
};

}  // namespace projectagamemnon
