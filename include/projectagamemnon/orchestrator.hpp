#pragma once

#include "projectagamemnon/hmas_types.hpp"
#include "projectagamemnon/planning_breakdown.hpp"
#include "projectagamemnon/state_machine.hpp"

#include <string>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

class Store;
class NatsClient;

using json = nlohmann::json;

/// HMAS Orchestrator — owns the full task lifecycle from brief submission through
/// L0→L3 delegation, escalation, and completion.
class Orchestrator {
 public:
  Orchestrator(Store& store, NatsClient& nats);

  /// Accept a TaskBrief, decompose it, persist all tasks, and enqueue the L0 root.
  /// Returns the brief ID (caller can use it to retrieve the full plan).
  std::string submit(TaskBrief brief);

  /// Transition a task to Delegated and publish it to the appropriate myrmidon subject.
  bool delegate(const std::string& task_id);

  /// Record an escalation on task_id and re-queue to parent layer's subject.
  bool escalate(const std::string& task_id, const std::string& reason);

  /// Called by the NATS subscription callback when a myrmidon publishes completion.
  void on_myrmidon_completion(const std::string& subject, const std::string& payload);

  /// Serialize the full task tree for a brief as JSON.
  json get_plan(const std::string& brief_id) const;

 private:
  Store& store_;
  NatsClient& nats_;
  PlanningBreakdown breakdown_;
  TaskStateMachine state_machine_;

  /// NATS subject for a given layer.
  static std::string myrmidon_subject(HmasLayer layer, const std::string& task_id);

  /// Delegate all child tasks of parent_id that are no longer blocked.
  void delegate_unblocked_children(const std::string& parent_id);
};

}  // namespace projectagamemnon
