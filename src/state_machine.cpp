#include "agamemnon/state_machine.hpp"

#include "agamemnon/store.hpp"  // for now_iso8601

namespace agamemnon {

std::string task_event_to_string(TaskEvent event) {
  switch (event) {
    case TaskEvent::Submit:
      return "Submit";
    case TaskEvent::Delegate:
      return "Delegate";
    case TaskEvent::Start:
      return "Start";
    case TaskEvent::Escalate:
      return "Escalate";
    case TaskEvent::Retry:
      return "Retry";
    case TaskEvent::Complete:
      return "Complete";
    case TaskEvent::Fail:
      return "Fail";
  }
  return "unknown";
}

TaskStateMachine::TaskStateMachine() {
  // Helper to register a transition with optional guard and action.
  auto add = [&](TaskState from, TaskEvent ev, TaskState to,
                 std::function<bool(const HmasTask&)> guard = nullptr,
                 std::function<void(HmasTask&)> action = nullptr) {
    table_[{from, ev}].push_back({to, std::move(guard), std::move(action)});
  };

  // Pending → Decomposing on Submit (L0 root; no guard required)
  add(TaskState::Pending, TaskEvent::Submit, TaskState::Decomposing);

  // Decomposing → Delegated on Delegate (breakdown complete, children created)
  add(TaskState::Decomposing, TaskEvent::Delegate, TaskState::Delegated);

  // Pending → Delegated on Delegate (L3 leaf tasks skip Decomposing)
  add(TaskState::Pending, TaskEvent::Delegate, TaskState::Delegated,
      [](const HmasTask& t) { return t.layer == HmasLayer::L3_TaskAgent; });

  // Delegated → InProgress on Start (myrmidon acknowledges pick-up)
  add(TaskState::Delegated, TaskEvent::Start, TaskState::InProgress);

  // InProgress → Escalated on Escalate
  add(TaskState::InProgress, TaskEvent::Escalate, TaskState::Escalated);

  // Escalated → Delegated on Retry (re-assigned to parent layer)
  add(TaskState::Escalated, TaskEvent::Retry, TaskState::Delegated);

  // InProgress → Completed on Complete (sets completedAt)
  add(TaskState::InProgress, TaskEvent::Complete, TaskState::Completed, nullptr,
      [](HmasTask& t) { t.completed_at = now_iso8601(); });

  // InProgress | Escalated → Failed on Fail
  add(TaskState::InProgress, TaskEvent::Fail, TaskState::Failed, nullptr,
      [](HmasTask& t) { t.completed_at = now_iso8601(); });
  add(TaskState::Escalated, TaskEvent::Fail, TaskState::Failed, nullptr,
      [](HmasTask& t) { t.completed_at = now_iso8601(); });
}

bool TaskStateMachine::try_transition(HmasTask& task, TaskEvent event) const {
  auto it = table_.find({task.state, event});
  if (it == table_.end()) return false;

  for (const auto& tr : it->second) {
    if (!tr.guard || tr.guard(task)) {
      task.state = tr.target;
      if (tr.action) tr.action(task);
      return true;
    }
  }
  return false;
}

std::vector<TaskState> TaskStateMachine::valid_targets(TaskState from, TaskEvent event) const {
  auto it = table_.find({from, event});
  if (it == table_.end()) return {};
  std::vector<TaskState> out;
  out.reserve(it->second.size());
  for (const auto& tr : it->second) out.push_back(tr.target);
  return out;
}

}  // namespace agamemnon
