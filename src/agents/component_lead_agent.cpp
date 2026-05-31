#include "agents/component_lead_agent.hpp"

#include <algorithm>
#include <chrono>
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

ComponentLeadAgent::ComponentLeadAgent(const std::string& agent_id)
    : LeadAgentBase<State>(agent_id, State::IDLE, State::PLANNING, State::WAITING_FOR_MODULES,
                           State::AGGREGATING, State::ERROR) {
  // Base class constructor initializes coordination_ with IDLE state
}

// processMessage() is now implemented in LeadAgentBase (template method
// pattern)

void ComponentLeadAgent::setAvailableModuleLeads(const std::vector<std::string>& module_lead_ids) {
  available_module_leads_ = module_lead_ids;
}

// === Hook Method Implementations (override LeadAgentBase pure virtuals) ===

bool ComponentLeadAgent::isSubordinateResult(const core::KeystoneMessage& msg) {
  // Check if this is a module result
  _Pragma("GCC diagnostic push")
      _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"") bool result =
          msg.command == "module_result";
  _Pragma("GCC diagnostic pop") return result;
}

std::vector<std::string> ComponentLeadAgent::decomposeGoal(const std::string& goal) {
  std::vector<std::string> module_goals;

  // Parse component goal like "Implement Core component: Messaging(10+20+30)
  // and Concurrency(40+50+60)" Extract module sections using regex

  // Pattern to match "ModuleName(numbers)"
  std::regex module_regex(R"((\w+)\(([0-9+]+)\))");
  auto modules_begin = std::sregex_iterator(goal.begin(), goal.end(), module_regex);
  auto modules_end = std::sregex_iterator();

  for (std::sregex_iterator i = modules_begin; i != modules_end; ++i) {
    std::smatch match = *i;
    std::string module_name = match[1].str();  // e.g., "Messaging"
    std::string numbers = match[2].str();      // e.g., "10+20+30"

    // Create module goal: "Calculate MODULE: numbers"
    std::string module_goal = "Calculate " + module_name + ": " + numbers;
    module_goals.push_back(module_goal);
  }

  return module_goals;
}

void ComponentLeadAgent::delegateSubtasks(const std::vector<std::string>& subtasks) {
  // Assign module goals to available ModuleLeadAgents
  for (size_t i = 0; i < subtasks.size(); ++i) {
    if (available_module_leads_.empty()) {
      throw std::runtime_error("No ModuleLeadAgents available for delegation");
    }

    if (i >= available_module_leads_.size()) {
      throw std::runtime_error("Not enough ModuleLeadAgents for all modules");
    }

    // Assign one module per ModuleLead
    const std::string& module_lead_id = available_module_leads_[i];

    // Create and send module goal message
    auto module_msg = core::KeystoneMessage::create(agent_id_, module_lead_id, subtasks[i]);

    // Track pending module
    coordination_.trackPendingSubordinate(module_msg.msg_id, module_lead_id);

    // Send via MessageBus
    sendMessage(module_msg);
  }
}

void ComponentLeadAgent::processSubordinateResult(const core::KeystoneMessage& result_msg) {
  if (!result_msg.payload) {
    // No payload, skip
    return;
  }

  // Store the module result and check if complete
  bool all_complete = coordination_.recordResult(*result_msg.payload);

  // Check if we've received all module results
  if (all_complete) {
    coordination_.transitionTo(State::AGGREGATING, stateToString(State::AGGREGATING));
  }
}

std::string ComponentLeadAgent::synthesizeComponentResult() {
  State current_state = coordination_.getCurrentState();
  if (current_state == State::ERROR) {
    auto failures = coordination_.getFailureMessages();
    std::string msg = "ERROR: " + std::to_string(coordination_.getFailureCount()) +
                      " subordinate module(s) failed";
    if (!failures.empty()) {
      msg += ": " + failures.front();
    }
    return msg;
  }
  if (current_state != State::AGGREGATING && current_state != State::WAITING_FOR_MODULES) {
    return "ERROR: Cannot synthesize in current state";
  }

  // Aggregate all module results
  std::stringstream ss;
  ss << "Component result: ";

  const auto& results = coordination_.getResults();
  for (size_t i = 0; i < results.size(); ++i) {
    if (i > 0) {
      ss << ", ";
    }
    ss << results[i];
  }

  coordination_.transitionTo(State::IDLE, stateToString(State::IDLE));

  return ss.str();
}

std::string ComponentLeadAgent::stateToString(State state) const {
  switch (state) {
    case State::IDLE:
      return "IDLE";
    case State::PLANNING:
      return "PLANNING";
    case State::WAITING_FOR_MODULES:
      return "WAITING_FOR_MODULES";
    case State::AGGREGATING:
      return "AGGREGATING";
    case State::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

#ifdef ENABLE_GRPC
void ComponentLeadAgent::initializeGrpc(const std::string& coordinator_address,
                                        const std::string& registry_address,
                                        const std::string& agent_type, uint8_t level) {
  // Delegate gRPC initialization to coordination template
  std::vector<std::string> capabilities = {"module_coordination", "component_synthesis"};
  coordination_.initializeGrpc(agent_id_, coordinator_address, registry_address, agent_type, level,
                               capabilities, 10);
}

void ComponentLeadAgent::processYamlComponent(const std::string& yaml_spec) {
  auto spec_opt = network::YamlParser::parseTaskSpec(yaml_spec);
  if (!spec_opt) {
    throw std::runtime_error("Failed to parse YAML component specification");
  }

  auto spec = *spec_opt;
  coordination_.setCurrentTaskId(spec.metadata.task_id);

  coordination_.transitionTo(State::PLANNING, stateToString(State::PLANNING));

  // Decompose component into modules using the hook method
  auto module_goals = decomposeGoal(spec.hierarchy.level1_directive.value_or(""));

  if (module_goals.empty()) {
    submitFailureResult(spec, "Failed to decompose component goal");
    return;
  }

  // Query for available ModuleLeadAgents
  available_module_leads_ = coordination_.queryAvailableChildren("ModuleLeadAgent");

  if (available_module_leads_.empty()) {
    submitFailureResult(spec, "No ModuleLeadAgents available");
    return;
  }

  // Create result aggregator
  auto timeout = network::YamlParser::parseDuration(spec.aggregation.timeout);
  result_aggregator_ = std::make_unique<network::ResultAggregator>(
      network::stringToStrategy(spec.aggregation.strategy),
      std::chrono::milliseconds(timeout.value_or(core::Config::DEFAULT_TASK_TIMEOUT_MS)),
      module_goals.size());

  // Generate child module YAMLs and submit to ModuleLeadAgents
  coordination_.transitionTo(State::WAITING_FOR_MODULES, stateToString(State::WAITING_FOR_MODULES));
  coordination_.initializeCoordination(static_cast<int>(module_goals.size()));

  for (size_t i = 0; i < module_goals.size(); ++i) {
    // Create child module spec
    network::HierarchicalTaskSpec child_spec;
    child_spec.api_version = "v1";
    child_spec.kind = "HierarchicalTask";
    child_spec.metadata.name = "module-" + std::to_string(i);
    child_spec.metadata.task_id = spec.metadata.task_id + "-module-" + std::to_string(i);
    child_spec.metadata.parent_task_id = spec.metadata.task_id;
    child_spec.metadata.session_id = spec.metadata.session_id;

    // Set routing
    size_t agent_index = i % available_module_leads_.size();
    child_spec.routing.target_agent_id = available_module_leads_[agent_index];
    child_spec.routing.target_level = 2;
    child_spec.routing.target_agent_type = "ModuleLeadAgent";

    // Set hierarchy
    child_spec.hierarchy.level0_goal = spec.hierarchy.level0_goal;
    child_spec.hierarchy.level1_directive = spec.hierarchy.level1_directive;
    child_spec.hierarchy.level2_module = module_goals[i];

    // Set action
    child_spec.action.type = "DECOMPOSE";

    // Copy aggregation config
    child_spec.aggregation = spec.aggregation;

    // Generate YAML
    std::string child_yaml = network::YamlParser::generateTaskSpec(child_spec);

    // Submit module via gRPC
    try {
      auto deadline_ms = network::YamlParser::parseDuration(spec.metadata.deadline.value_or("25m"))
                             .value_or(core::Config::DEFAULT_TASK_TIMEOUT_MS);
      auto coordinator_client = coordination_.getCoordinatorClient();
      auto response = coordinator_client->submitTask(
          child_yaml, spec.metadata.session_id.value_or(""), deadline_ms,
          hmas::TASK_PRIORITY_NORMAL, coordination_.getCurrentTaskId());

      coordination_.trackPendingSubordinate(child_spec.metadata.task_id,
                                            available_module_leads_[agent_index]);

      // Poll for task result in background (gRPC async result handling for
      // Issue #186) If the task fails, record it to prevent DAG deadlock
      std::thread([this, coordinator_client, task_id = response.task_id(), deadline_ms]() {
        try {
          auto result = coordinator_client->getTaskResult(task_id, deadline_ms);
          if (!result.success()) {
            coordination_.recordFailure(result.error());
            // Transition to ERROR if all results are now in
            State current_state = coordination_.getCurrentState();
            if (coordination_.isComplete() && current_state == State::WAITING_FOR_MODULES) {
              coordination_.transitionTo(State::ERROR, stateToString(State::ERROR));
            }
          }
        } catch (const std::exception& e) {
          concurrency::Logger::error("Failed to get result for module task {}: {}", task_id,
                                     e.what());
        }
      }).detach();
    } catch (const std::exception& e) {
      concurrency::Logger::error("Failed to submit module: {}", e.what());
    }
  }
}

void ComponentLeadAgent::startHeartbeat() {
  // Delegate to coordination template
  coordination_.startHeartbeat(agent_id_);
}

void ComponentLeadAgent::stopHeartbeat() {
  // Delegate to coordination template
  coordination_.stopHeartbeat();
}

void ComponentLeadAgent::shutdown() {
  // Shutdown coordination (handles heartbeat, client cleanup)
  coordination_.shutdownGrpc();

  // Clean up component-specific resources
  result_aggregator_.reset();
}
#endif  // ENABLE_GRPC

}  // namespace agents
}  // namespace keystone
