#include "agents/module_lead_agent.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <regex>
#include <sstream>
#include <thread>

#include "concurrency/logger.hpp"
#include "core/config.hpp"

#ifdef ENABLE_GRPC
#include "hmas_coordinator.pb.h"
#endif

namespace keystone {
namespace agents {

ModuleLeadAgent::ModuleLeadAgent(const std::string& agent_id)
    : LeadAgentBase<State>(agent_id, State::IDLE, State::PLANNING, State::WAITING_FOR_TASKS,
                           State::SYNTHESIZING, State::ERROR) {
  // Base class constructor initializes coordination_ with IDLE state
}

// processMessage() is now implemented in LeadAgentBase (template method
// pattern)

void ModuleLeadAgent::setAvailableTaskAgents(const std::vector<std::string>& task_agent_ids) {
  available_task_agents_ = task_agent_ids;
}

// === Hook Method Implementations (override LeadAgentBase pure virtuals) ===

bool ModuleLeadAgent::isSubordinateResult(const core::KeystoneMessage& msg) {
  // Exclude TASK_FAILED so processSubordinateFailure() handles it instead
  _Pragma("GCC diagnostic push")
      _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"") bool result =
          msg.command == "response" && msg.action_type != core::ActionType::TASK_FAILED;
  _Pragma("GCC diagnostic pop") return result;
}

std::vector<std::string> ModuleLeadAgent::decomposeGoal(const std::string& goal) {
  std::vector<std::string> tasks;

  // Parse module goal like "Calculate sum of: 10 + 20 + 30"
  // or "Calculate: 100 + 200"

  // Extract numbers using regex
  std::regex number_regex(R"(\d+)");
  auto numbers_begin = std::sregex_iterator(goal.begin(), goal.end(), number_regex);
  auto numbers_end = std::sregex_iterator();

  // Create a task for each number (echo the number)
  for (std::sregex_iterator i = numbers_begin; i != numbers_end; ++i) {
    std::string number = i->str();
    std::string task = "echo " + number;
    tasks.push_back(task);
  }

  return tasks;
}

void ModuleLeadAgent::delegateSubtasks(const std::vector<std::string>& subtasks) {
  // Assign tasks to available TaskAgents in round-robin fashion
  for (size_t i = 0; i < subtasks.size(); ++i) {
    if (available_task_agents_.empty()) {
      throw std::runtime_error("No TaskAgents available for delegation");
    }

    // Round-robin assignment
    size_t agent_index = i % available_task_agents_.size();
    const std::string& task_agent_id = available_task_agents_[agent_index];

    // Create and send task message
    auto task_msg = core::KeystoneMessage::create(agent_id_, task_agent_id, subtasks[i]);

    // Track pending task
    coordination_.trackPendingSubordinate(task_msg.msg_id, task_agent_id);

    // Send via MessageBus
    sendMessage(task_msg);
  }
}

void ModuleLeadAgent::processSubordinateResult(const core::KeystoneMessage& result_msg) {
  if (!result_msg.payload) {
    // No payload, skip
    return;
  }

  // Store the result and check if complete
  bool all_complete = coordination_.recordResult(*result_msg.payload);

  // Check if we've received all results
  if (all_complete) {
    if (coordination_.hasFailures()) {
      coordination_.transitionTo(State::ERROR, stateToString(State::ERROR));
    } else {
      coordination_.transitionTo(State::SYNTHESIZING, stateToString(State::SYNTHESIZING));
    }
  }
}

std::string ModuleLeadAgent::synthesizeResults() {
  State current_state = coordination_.getCurrentState();
  if (current_state == State::ERROR) {
    auto failures = coordination_.getFailureMessages();
    std::string msg =
        "ERROR: " + std::to_string(coordination_.getFailureCount()) + " subordinate task(s) failed";
    if (!failures.empty()) {
      msg += ": " + failures.front();
    }
    return msg;
  }
  if (current_state != State::SYNTHESIZING && current_state != State::WAITING_FOR_TASKS) {
    return "ERROR: Cannot synthesize in current state";
  }

  // Parse each result as a number and sum them
  int32_t total = 0;
  const auto& results = coordination_.getResults();
  for (const auto& result : results) {
    try {
      int32_t value = std::stoi(result);
      total += value;
    } catch (const std::exception&) {
      // Skip non-numeric results
    }
  }

  coordination_.transitionTo(State::IDLE, stateToString(State::IDLE));

  // Return synthesis
  std::stringstream ss;
  ss << "Module result: Sum = " << total;
  return ss.str();
}

std::string ModuleLeadAgent::stateToString(State state) const {
  switch (state) {
    case State::IDLE:
      return "IDLE";
    case State::PLANNING:
      return "PLANNING";
    case State::WAITING_FOR_TASKS:
      return "WAITING_FOR_TASKS";
    case State::SYNTHESIZING:
      return "SYNTHESIZING";
    case State::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

#ifdef ENABLE_GRPC
void ModuleLeadAgent::initializeGrpc(const std::string& coordinator_address,
                                     const std::string& registry_address,
                                     const std::string& agent_type, uint8_t level) {
  // Delegate gRPC initialization to coordination template
  std::vector<std::string> capabilities = {"task_coordination", "result_synthesis"};
  coordination_.initializeGrpc(agent_id_, coordinator_address, registry_address, agent_type, level,
                               capabilities, 5);
}

void ModuleLeadAgent::processYamlModule(const std::string& yaml_spec) {
  // Parse YAML module specification
  auto spec_opt = network::YamlParser::parseTaskSpec(yaml_spec);
  if (!spec_opt) {
    throw std::runtime_error("Failed to parse YAML module specification");
  }

  auto spec = *spec_opt;
  coordination_.setCurrentTaskId(spec.metadata.task_id);

  coordination_.transitionTo(State::PLANNING, stateToString(State::PLANNING));

  // Decompose module into tasks using the hook method
  auto tasks = decomposeGoal(spec.hierarchy.level2_module.value_or(""));

  if (tasks.empty()) {
    submitFailureResult(spec, "Failed to decompose module goal");
    return;
  }

  // Query for available TaskAgents
  available_task_agents_ = coordination_.queryAvailableChildren("TaskAgent");

  if (available_task_agents_.empty()) {
    submitFailureResult(spec, "No TaskAgents available");
    return;
  }

  // Create result aggregator
  auto timeout = network::YamlParser::parseDuration(spec.aggregation.timeout);
  result_aggregator_ = std::make_unique<network::ResultAggregator>(
      network::stringToStrategy(spec.aggregation.strategy),
      std::chrono::milliseconds(timeout.value_or(core::Config::DEFAULT_TASK_TIMEOUT_MS)),
      tasks.size());

  // Generate child task YAMLs and submit to TaskAgents
  coordination_.transitionTo(State::WAITING_FOR_TASKS, stateToString(State::WAITING_FOR_TASKS));
  coordination_.initializeCoordination(static_cast<int32_t>(tasks.size()));

  for (size_t i = 0; i < tasks.size(); ++i) {
    // Create child task spec
    network::HierarchicalTaskSpec child_spec;
    child_spec.api_version = "v1";
    child_spec.kind = "HierarchicalTask";
    child_spec.metadata.name = "task-" + std::to_string(i);
    child_spec.metadata.task_id = spec.metadata.task_id + "-subtask-" + std::to_string(i);
    child_spec.metadata.parent_task_id = spec.metadata.task_id;
    child_spec.metadata.session_id = spec.metadata.session_id;

    // Set routing
    size_t agent_index = i % available_task_agents_.size();
    child_spec.routing.target_agent_id = available_task_agents_[agent_index];
    child_spec.routing.target_level = 3;
    child_spec.routing.target_agent_type = "TaskAgent";

    // Set payload
    child_spec.payload.command = tasks[i];

    // Set action
    child_spec.action.type = "EXECUTE";

    // Set hierarchy
    child_spec.hierarchy.level0_goal = spec.hierarchy.level0_goal;
    child_spec.hierarchy.level2_module = spec.hierarchy.level2_module;
    child_spec.hierarchy.level3_task = tasks[i];

    // Generate YAML
    std::string child_yaml = network::YamlParser::generateTaskSpec(child_spec);

    // Submit task via gRPC
    try {
      auto deadline_ms = network::YamlParser::parseDuration(spec.metadata.deadline.value_or("25m"))
                             .value_or(core::Config::DEFAULT_TASK_TIMEOUT_MS);
      auto coordinator_client = coordination_.getCoordinatorClient();
      auto response = coordinator_client->submitTask(
          child_yaml, spec.metadata.session_id.value_or(""), deadline_ms,
          hmas::TASK_PRIORITY_NORMAL, coordination_.getCurrentTaskId());

      coordination_.trackPendingSubordinate(child_spec.metadata.task_id,
                                            available_task_agents_[agent_index]);

      // Poll for task result in background (gRPC async result handling for
      // Issue #186) If the task fails, record it to prevent DAG deadlock
      std::thread([this, coordinator_client, task_id = response.task_id(), deadline_ms]() {
        try {
          auto result = coordinator_client->getTaskResult(task_id, deadline_ms);
          if (!result.success()) {
            coordination_.recordFailure(result.error());
            // Transition to ERROR if all results are now in
            State current_state = coordination_.getCurrentState();
            if (coordination_.isComplete() && current_state == State::WAITING_FOR_TASKS) {
              coordination_.transitionTo(State::ERROR, stateToString(State::ERROR));
            }
          }
        } catch (const std::exception& e) {
          concurrency::Logger::error("Failed to get result for task {}: {}", task_id, e.what());
        }
      }).detach();
    } catch (const std::exception& e) {
      concurrency::Logger::error("Failed to submit task: {}", e.what());
    }
  }
}

void ModuleLeadAgent::startHeartbeat() {
  // Delegate to coordination template
  coordination_.startHeartbeat(agent_id_);
}

void ModuleLeadAgent::stopHeartbeat() {
  // Delegate to coordination template
  coordination_.stopHeartbeat();
}

void ModuleLeadAgent::shutdown() {
  // Shutdown coordination (handles heartbeat, client cleanup)
  coordination_.shutdownGrpc();

  // Clean up module-specific resources
  result_aggregator_.reset();
}
#endif  // ENABLE_GRPC

}  // namespace agents
}  // namespace keystone
