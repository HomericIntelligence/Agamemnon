#pragma once

#include "agamemnon/hmas_types.hpp"
#include "agamemnon/planning_breakdown.hpp"
#include "agamemnon/state_machine.hpp"

#include <string>

#include "nlohmann/json.hpp"

namespace agamemnon {

class Store;
class NatsPublisher;

using json = nlohmann::json;

/// HMAS Orchestrator — owns the full task lifecycle from brief submission through
/// L0→L3 delegation, escalation, and completion.
class Orchestrator {
 public:
  Orchestrator(Store& store, NatsPublisher& nats);

  /// Accept a TaskBrief, decompose it, persist all tasks, and enqueue the L0 root.
  /// Returns the brief ID (caller can use it to retrieve the full plan).
  std::string submit(TaskBrief brief);

  /// Transition a task to Delegated and publish it to the appropriate myrmidon subject.
  bool delegate(const std::string& task_id);

  /// Record an escalation on task_id and re-queue to parent layer's subject.
  bool escalate(const std::string& task_id, const std::string& reason);

  /// Called by the NATS subscription callback when a myrmidon publishes completion.
  void on_myrmidon_completion(const std::string& subject, const std::string& payload);

  /// Called when a worker publishes hi.tasks.{team}.{task}.started (ADR-013 §2).
  /// Drives the task to InProgress and records the assignment — the claim IS
  /// the assignment: agent_id/exec_host from the payload land on the task.
  void on_myrmidon_started(const std::string& subject, const std::string& payload);

  /// Called when a worker publishes hi.tasks.{team}.{task}.failed (ADR-013 §2).
  /// Drives the task to Failed (via InProgress when needed).
  void on_myrmidon_failed(const std::string& subject, const std::string& payload);

  /// Called when Telemachy publishes hi.pipeline.epic.{key}.registered
  /// (ADR-013 §6). Creates a placeholder brief + L0 root (Pending →
  /// Decomposing) and dispatches the decompose burst to the
  /// pipeline.chief-architect role queue. Returns the new brief id ("" on
  /// invalid payloads).
  std::string on_epic_registered(const std::string& subject, const std::string& payload);

  /// Worker overrun re-adjustment (ADR-013 §4): register remainder subtasks
  /// under task_id. Each subtask {title, description?, blocked_by?[],
  /// base_branch?} becomes a Pending sibling blocked by the original (plus
  /// any listed blockers), so completing the original — the first slice of
  /// the split — dispatches the remainder. Returns
  /// {"task_id", "created": [ids]} or {"error": ...}.
  json split_task(const std::string& task_id, const json& subtasks);

  /// Serialize the full task tree for a brief as JSON.
  json get_plan(const std::string& brief_id) const;

 private:
  Store& store_;
  NatsPublisher& nats_;
  PlanningBreakdown breakdown_;
  TaskStateMachine state_machine_;

  /// Publish task state to hi.tasks.<state> NATS subject per ADR-006.
  void publish_task_state(const HmasTask& task);

  /// Legacy NATS subject for a given layer (dual-published for one release —
  /// ADR-013 migration; the role-addressed form is mesh_dispatch_subject()).
  static std::string myrmidon_subject(HmasLayer layer, const std::string& task_id);

  /// Publish a task to BOTH the legacy layer subject and the role-addressed
  /// ADR-013 queue (hi.myrmidon.pipeline.{role}.task.{id}).
  void dispatch_task(const HmasTask& task, json payload);

  /// Delegate all child tasks of parent_id that are no longer blocked.
  void delegate_unblocked_children(const std::string& parent_id);
};

}  // namespace agamemnon
