#pragma once

#include "agamemnon/github_client.hpp"
#include "agamemnon/hmas_types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace agamemnon {

using json = nlohmann::json;

class MetricsRegistry;
struct IssueImportConfiguration;
struct CanonicalWorkIssue;

/// Generate a UUID-like string using <random>.
std::string generate_uuid();

/// Get current ISO 8601 timestamp (UTC).
std::string now_iso8601();

/// Thread-safe store for agents, teams, tasks, and faults.
/// In-memory maps act as a write-through cache backed by GitHub Issues.
/// Pass nullptr (or use the default constructor) for pure in-memory mode.
class Store {
 public:
  explicit Store(std::shared_ptr<IGitHubClient> gh = nullptr,
                 std::shared_ptr<const IssueImportConfiguration> import_configuration = nullptr)
      : gh_(std::move(gh)), import_configuration_(std::move(import_configuration)) {}

  /// Fleet shares this backing store; it never permits memory-only operation.
  std::shared_ptr<IGitHubClient> github_client() const { return gh_; }
  std::shared_ptr<const IssueImportConfiguration> import_configuration() const {
    return import_configuration_;
  }

  /// Attach a MetricsRegistry for instrumentation (nullable; pass nullptr to disable).
  void set_metrics(MetricsRegistry* metrics) noexcept { metrics_ = metrics; }

  /// Identifies a single store collection and its dedicated mutex (#184).
  enum class Collection { kAgents, kTeams, kTasks, kFaults };

  /// Testing seam ONLY (#184): exclusively locks one collection's mutex for the
  /// duration of fn(), letting concurrency tests pin one collection and assert
  /// independent progress on the others. Not for production use.
  template <typename F>
  auto with_collection_locked(Collection c, F&& fn) -> decltype(fn()) {
    std::unique_lock<std::shared_mutex> lk(collection_mutex_(c));
    return fn();
  }

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
  /// Reconcile every backing issue before creating or replaying a research leaf.
  /// The boolean is true only after a new durable creation is acknowledged.
  std::pair<HmasTask, bool> import_research_task(const HmasTask& proposed);
  std::pair<HmasTask, bool> import_issue_task(const HmasTask& proposed,
                                              const CanonicalWorkIssue& work,
                                              ImportContext& context);
  /// Returns a value copy of the task; safe to use outside the mutex.
  std::optional<HmasTask> get_hmas_task(const std::string& id);
  bool update_hmas_task_state(const std::string& id, TaskState state);
  /// Atomically update the task state and append an escalation record.
  bool update_hmas_task_state_and_record_escalation(const std::string& id, TaskState new_state,
                                                    const EscalationRecord& escalation);
  bool update_hmas_task(const HmasTask& task);
  /// Serialize legacy planning against Fleet admission. Persist parent links
  /// before child creation so a partial write cannot leave untracked children.
  bool append_hmas_children(const HmasTask& expected, const std::vector<HmasTask>& children);
  /// Metadata-only compare-and-write, including Fleet-owned tasks; no state/claim mutation.
  bool update_hmas_delivery(const std::string& id, const json& expected, const json& delivery);
  /// Publish only while both snapshots remain current and the parent is parked
  /// in a durable epic. Holds the HMAS read lock through the bounded callback;
  /// the callback must perform transport only and must not call Store or GitHub.
  bool publish_hmas_parent_wakeup(const HmasTask& child, const HmasTask& parent,
                                  const std::function<void()>& publish);
  /// Durable exclusive reservation; false means ineligible or another owner.
  bool reserve_hmas_fleet_claim(const std::string& id, const json& claim);
  /// A matching worker observation may start work; it cannot complete a task.
  bool observe_hmas_fleet_start(const std::string& id, const json& claim);
  std::optional<HmasTask> resolve_hmas_fleet_task(const std::string& id, const json& claim,
                                                  const json& decision);
  std::vector<HmasTask> list_hmas_tasks_by_layer(HmasLayer layer);
  std::vector<HmasTask> list_hmas_tasks_by_parent(const std::string& parent_id);
  std::vector<HmasTask> list_hmas_tasks_by_brief(const std::string& brief_id);

  // ── TaskBriefs (HMAS root submissions) ─────────────────────────────────
  void create_task_brief(const TaskBrief& brief);
  /// Strict all-state reconciliation for deterministic durable registrations.
  void ensure_durable_task_brief(const TaskBrief& brief);
  std::optional<TaskBrief> get_task_brief(const std::string& id);
  std::vector<TaskBrief> list_task_briefs();

  // ── Inbound sync (#165) ───────────────────────────────────────────────
  bool apply_github_event(std::string_view entity_label, std::string_view action,
                          const json& issue_shape, std::string_view updated_at);
  std::size_t reconcile_from_github();

 private:
  std::shared_ptr<IGitHubClient> gh_;
  std::shared_ptr<const IssueImportConfiguration> import_configuration_;
  MetricsRegistry* metrics_ = nullptr;

  // One mutex per collection (#184): agents_/teams_/tasks_/faults_ each get
  // their own shared_mutex so operations on different collections proceed in
  // parallel. hmas_mutex_ covers hmas_tasks_, hmas_tasks_by_brief_, and
  // hmas_task_issue_numbers_; briefs_mutex_ covers task_briefs_ and
  // brief_issue_numbers_. No function ever holds two collection mutexes at once.
  mutable std::shared_mutex agents_mutex_;
  mutable std::shared_mutex teams_mutex_;
  mutable std::shared_mutex tasks_mutex_;
  mutable std::shared_mutex faults_mutex_;
  mutable std::shared_timed_mutex hmas_mutex_;
  mutable std::shared_mutex briefs_mutex_;

  std::unordered_map<std::string, json> agents_;
  std::unordered_map<std::string, json> teams_;
  std::unordered_map<std::string, json> tasks_;
  std::unordered_map<std::string, json> faults_;
  std::unordered_map<std::string, HmasTask> hmas_tasks_;
  // Secondary index: brief_id -> task ids. Maintained under the same
  // hmas_mutex_ write lock as hmas_tasks_; read under shared_lock by
  // list_hmas_tasks_by_brief.
  // #156: avoids O(n) full-map scan on every myrmidon completion.
  std::unordered_map<std::string, std::vector<std::string>> hmas_tasks_by_brief_;
  std::unordered_map<std::string, TaskBrief> task_briefs_;
  std::unordered_map<std::string, std::string> hmas_task_issue_numbers_;
  std::unordered_map<std::string, std::string> brief_issue_numbers_;

  // Atomic flags: checked outside the lock. Legacy once_flags guard a single
  // fetch; retryable HMAS hydration and invalidation use hmas_mutex_.
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
  mutable std::once_flag briefs_once_;

  // Called without holding a collection mutex; each loader locks internally.
  void ensure_agents_loaded_();
  void ensure_teams_loaded_();
  void ensure_tasks_loaded_();
  void ensure_faults_loaded_();
  void ensure_hmas_tasks_loaded_();
  // Called under hmas_mutex_; an uncertain response invalidates HMAS state.
  void persist_hmas_task_(const HmasTask& task);
  // Under hmas_mutex_: refuse imported/uncertain work and return its verified key for batch checks.
  std::optional<std::string> guard_work_acquisition_(const HmasTask& task,
                                                     bool existing_identity = false);
  std::pair<HmasTask, bool> import_task_(const HmasTask& proposed, const CanonicalWorkIssue& work,
                                         ImportContext& context);
  void ensure_briefs_loaded_();

  // Returns the mutex guarding the given collection.
  std::shared_mutex& collection_mutex_(Collection c) noexcept;

  // Map + its mutex, returned together so apply_github_event can lock the
  // right collection. map is nullptr for unknown labels (previous pick_map_
  // semantics).
  struct CollectionRef {
    std::unordered_map<std::string, json>* map;
    std::shared_mutex* mtx;
  };

  // Returns the collection matching the agamemnon-* label; map == nullptr on
  // unknown label.
  CollectionRef pick_collection_(std::string_view entity_label);

  // Parses the JSON payload embedded in an issue body; returns nullptr on failure.
  static json parse_issue_entity_(const json& issue, bool reject_duplicate_keys = false);

  // Builds a GitHub issue body containing a labelled JSON block.
  static std::string make_issue_body_(std::string_view entity_type, const json& entity);
};

}  // namespace agamemnon
