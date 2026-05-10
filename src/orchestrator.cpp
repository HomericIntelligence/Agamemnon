#include "projectagamemnon/orchestrator.hpp"

#include "projectagamemnon/nats_publisher.hpp"
#include "projectagamemnon/store.hpp"

#include <iostream>

namespace projectagamemnon {

Orchestrator::Orchestrator(Store& store, NatsPublisher& nats) : store_(store), nats_(nats) {}

// ── Public API ────────────────────────────────────────────────────────────────

std::string Orchestrator::submit(TaskBrief brief) {
  if (brief.id.empty()) brief.id = generate_uuid();

  // Decompose into full task tree.
  auto tasks = breakdown_.decompose(brief);

  // Persist all tasks.
  for (const auto& t : tasks) store_.create_hmas_task(t);

  // Transition L0 root to Decomposing then immediately Delegated, then enqueue.
  if (!tasks.empty()) {
    HmasTask* root = store_.get_hmas_task(tasks[0].id);
    if (root) {
      state_machine_.try_transition(*root, TaskEvent::Submit);    // Pending -> Decomposing
      state_machine_.try_transition(*root, TaskEvent::Delegate);  // Decomposing -> Delegated
      store_.update_hmas_task(*root);

      std::string subj = myrmidon_subject(HmasLayer::L0_ChiefArchitect, root->id);
      json payload = hmas_task_to_json(*root);
      payload["brief_id"] = brief.id;
      nats_.publish(subj, payload.dump());
      nats_.publish_log(
          "hi.logs.agamemnon.brief_submitted", "info", "Brief submitted: " + brief.id,
          {{"brief_id", brief.id}, {"root_task_id", root->id}, {"task_count", tasks.size()}});
    }
  }

  return brief.id;
}

bool Orchestrator::delegate(const std::string& task_id) {
  HmasTask* task = store_.get_hmas_task(task_id);
  if (!task) return false;

  bool ok = state_machine_.try_transition(*task, TaskEvent::Delegate);
  if (!ok) {
    // L3 leaf tasks transition Pending → Delegated; try again in case state is still Pending.
    ok = state_machine_.try_transition(*task, TaskEvent::Delegate);
  }
  if (!ok) return false;

  store_.update_hmas_task(*task);
  std::string subj = myrmidon_subject(task->layer, task->id);
  nats_.publish(subj, hmas_task_to_json(*task).dump());
  return true;
}

bool Orchestrator::escalate(const std::string& task_id, const std::string& reason) {
  HmasTask* task = store_.get_hmas_task(task_id);
  if (!task) return false;

  // Only InProgress tasks can be escalated.
  if (!state_machine_.try_transition(*task, TaskEvent::Escalate)) return false;

  EscalationRecord rec;
  rec.task_id = task_id;
  rec.reason = reason;
  rec.escalated_at = now_iso8601();
  rec.from_layer = task->layer;
  task->escalations.push_back(rec);
  store_.update_hmas_task(*task);

  // Re-enqueue to parent layer's subject.
  HmasLayer parent_layer = (task->layer == HmasLayer::L0_ChiefArchitect)
                               ? HmasLayer::L0_ChiefArchitect
                               : static_cast<HmasLayer>(static_cast<int>(task->layer) - 1);

  json payload = hmas_task_to_json(*task);
  payload["escalation_reason"] = reason;
  nats_.publish(myrmidon_subject(parent_layer, task->id), payload.dump());
  nats_.publish_log("hi.logs.agamemnon.task_escalated", "warn", "Task escalated: " + task_id,
                    {{"task_id", task_id},
                     {"reason", reason},
                     {"from_layer", hmas_layer_to_string(task->layer)}});
  return true;
}

void Orchestrator::on_myrmidon_completion(const std::string& subject, const std::string& payload) {
  try {
    auto msg = json::parse(payload);
    std::string task_id;
    if (msg.contains("data") && msg["data"].contains("task_id"))
      task_id = msg["data"]["task_id"].get<std::string>();
    else if (msg.contains("task_id"))
      task_id = msg["task_id"].get<std::string>();

    if (task_id.empty()) return;

    HmasTask* task = store_.get_hmas_task(task_id);
    if (!task) {
      // Fall back to plain task completion for non-HMAS tasks.
      store_.mark_task_completed(task_id);
      return;
    }

    // L3 tasks go InProgress → Completed.
    // Higher-layer tasks also resolve when myrmidon reports done.
    if (task->state == TaskState::Delegated) state_machine_.try_transition(*task, TaskEvent::Start);
    state_machine_.try_transition(*task, TaskEvent::Complete);
    store_.update_hmas_task(*task);

    std::cout << "[orchestrator] task completed via " << subject << ": " << task_id << "\n";

    // Unlock and delegate any children that are no longer blocked.
    delegate_unblocked_children(task_id);

    // If this was the L0 root, log pipeline completion.
    if (task->layer == HmasLayer::L0_ChiefArchitect) {
      nats_.publish_log("hi.logs.agamemnon.brief_completed", "info",
                        "Brief completed: " + task->brief_id,
                        {{"brief_id", task->brief_id}, {"root_task_id", task_id}});
    }
  } catch (...) {
    // Ignore malformed payloads.
  }
}

json Orchestrator::get_plan(const std::string& brief_id) const {
  auto tasks = store_.list_hmas_tasks_by_brief(brief_id);

  json root_json = nullptr;
  json tasks_arr = json::array();
  for (const auto& t : tasks) {
    json tj = hmas_task_to_json(t);
    tasks_arr.push_back(tj);
    if (t.parent_task_id.empty()) root_json = tj;
  }

  return {{"brief_id", brief_id}, {"root", root_json}, {"tasks", tasks_arr}};
}

// ── Private helpers ───────────────────────────────────────────────────────────

std::string Orchestrator::myrmidon_subject(HmasLayer layer, const std::string& task_id) {
  std::string layer_str;
  switch (layer) {
    case HmasLayer::L0_ChiefArchitect:
      layer_str = "chief_architect";
      break;
    case HmasLayer::L1_ComponentLead:
      layer_str = "component_lead";
      break;
    case HmasLayer::L2_ModuleLead:
      layer_str = "module_lead";
      break;
    case HmasLayer::L3_TaskAgent:
      layer_str = "task_agent";
      break;
  }
  return "hi.myrmidon." + layer_str + "." + task_id;
}

void Orchestrator::delegate_unblocked_children(const std::string& completed_task_id) {
  // Find the completed task to know its brief.
  HmasTask* completed = store_.get_hmas_task(completed_task_id);
  if (!completed) return;

  // Get all tasks in this brief.
  auto siblings = store_.list_hmas_tasks_by_brief(completed->brief_id);

  for (const auto& candidate : siblings) {
    if (candidate.state != TaskState::Pending) continue;

    // Check if all blockers are now Completed.
    bool all_clear = true;
    for (const auto& blocker_id : candidate.blocked_by) {
      HmasTask* blocker = store_.get_hmas_task(blocker_id);
      if (!blocker || blocker->state != TaskState::Completed) {
        all_clear = false;
        break;
      }
    }

    if (all_clear) delegate(candidate.id);
  }
}

}  // namespace projectagamemnon
