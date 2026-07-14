#pragma once

#include "agamemnon/hmas_types.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agamemnon {

// Events that drive TaskState transitions.
enum class TaskEvent : int {
  Submit,    // Pending -> Decomposing
  Delegate,  // Decomposing -> Delegated
  Start,     // Delegated -> InProgress
  Escalate,  // InProgress -> Escalated
  Retry,     // Escalated -> Delegated (re-assign after escalation)
  Complete,  // InProgress -> Completed
  Fail,      // InProgress | Escalated -> Failed
};

std::string task_event_to_string(TaskEvent event);

struct Transition {
  TaskState target;
  std::function<bool(const HmasTask&)> guard;  // nullptr = always allowed
  std::function<void(HmasTask&)> action;       // nullptr = no side effect
};

/// Transition table for HMAS task state machine.
/// try_transition applies the first matching transition for the (state, event) pair.
class TaskStateMachine {
 public:
  TaskStateMachine();

  /// Attempt a transition. Returns true and mutates task on success, false if rejected.
  bool try_transition(HmasTask& task, TaskEvent event) const;

  /// Return valid target states from a given state (for introspection/tests).
  std::vector<TaskState> valid_targets(TaskState from, TaskEvent event) const;

 private:
  // (from_state, event) -> list of candidate Transitions (first matching guard wins)
  struct Key {
    TaskState state;
    TaskEvent event;
    bool operator==(const Key& o) const noexcept { return state == o.state && event == o.event; }
  };

  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
      return std::hash<int>{}(static_cast<int>(k.state)) ^
             (std::hash<int>{}(static_cast<int>(k.event)) << 4);
    }
  };

  std::unordered_map<Key, std::vector<Transition>, KeyHash> table_;
};

}  // namespace agamemnon
