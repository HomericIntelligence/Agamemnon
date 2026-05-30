#include "agents/chief_architect_agent.hpp"

#include <chrono>
#include <thread>

#include "concurrency/logger.hpp"

#ifdef ENABLE_GRPC
#include "hmas_coordinator.pb.h"
#endif

namespace keystone {
namespace agents {

ChiefArchitectAgent::ChiefArchitectAgent(const std::string& agent_id) : AsyncAgent(agent_id) {}

concurrency::Task<core::Response> ChiefArchitectAgent::processMessage(
    const core::KeystoneMessage& msg) {
  // FIX C3: Changed to async (returns Task<Response>)

  // Phase 1.2 (Issue #52): Handle CANCEL_TASK action type
  if (msg.action_type == core::ActionType::CANCEL_TASK) {
    auto response = handleCancellation(msg);

    // Send acknowledgement back to sender via MessageBus
    auto response_msg =
        core::KeystoneMessage::create(agent_id_,
                                      msg.sender_id,  // Route back to original sender
                                      "response", response.result);
    response_msg.msg_id = msg.msg_id;  // Keep same msg_id for tracking

    sendMessage(response_msg);

    co_return response;
  }

  // For Phase 1, ChiefArchitect receives responses from TaskAgent
  // Defensive: check payload exists before dereferencing
  if (!msg.payload) {
    co_return core::Response::createError(msg, agent_id_, "Missing payload in message");
  }

  core::Response resp = core::Response::createSuccess(msg, agent_id_, *msg.payload);
  co_return resp;
}

concurrency::Task<core::Response> ChiefArchitectAgent::sendCommand(
    const std::string& command, const std::string& task_agent_id) {
  // FIX C3: Changed to async (returns Task<Response>)
  // Create message
  auto msg = core::KeystoneMessage::create(agent_id_, task_agent_id, command);

  // Send to task agent via MessageBus
  sendMessage(msg);

  // For Phase 1 synchronous testing, task agent processes immediately
  // and sends response back via MessageBus
  // Get the response
  auto response_msg = getMessage();
  if (response_msg) {
    co_return co_await processMessage(*response_msg);
  }

  co_return core::Response::createError(msg, agent_id_, "No response received");
}

#ifdef ENABLE_GRPC
void ChiefArchitectAgent::initializeGrpc(const std::string& coordinator_address,
                                         const std::string& registry_address,
                                         const std::string& agent_type, uint8_t level) {
  agent_type_ = agent_type;
  agent_level_ = level;

  // Create gRPC clients
  network::GrpcClientConfig coordinator_config;
  coordinator_config.server_address = coordinator_address;
  coordinator_client_ = std::make_unique<network::HMASCoordinatorClient>(coordinator_config);

  network::GrpcClientConfig registry_config;
  registry_config.server_address = registry_address;
  registry_client_ = std::make_unique<network::ServiceRegistryClient>(registry_config);

  // Register with ServiceRegistry
  hmas::AgentRegistration registration;
  registration.set_agent_id(agent_id_);
  registration.set_agent_type(agent_type_);
  registration.set_level(level);
  registration.add_capabilities("strategic_orchestration");
  registration.set_max_concurrent_tasks(1);

  try {
    auto response = registry_client_->registerAgent(registration);
    if (response.success()) {
      startHeartbeat();
    } else {
      throw std::runtime_error("Failed to register with ServiceRegistry: " + response.message());
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("gRPC registration failed: " + std::string(e.what()));
  }
}

std::string ChiefArchitectAgent::submitUserGoal(const std::string& user_goal,
                                                const std::string& session_id) {
  // Query for available ComponentLeadAgent
  std::string component_agent_id = queryComponentLeadAgent();

  if (component_agent_id.empty()) {
    throw std::runtime_error("No ComponentLeadAgent available");
  }

  // Generate YAML task specification
  network::HierarchicalTaskSpec spec;
  spec.api_version = "v1";
  spec.kind = "HierarchicalTask";
  spec.metadata.name = "user-goal";
  spec.metadata.task_id = "chief-" + session_id;
  spec.metadata.session_id = session_id;

  // Set timestamps
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  char time_buf[100];
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t_now));
  spec.metadata.created_at = std::string(time_buf);
  spec.metadata.deadline = "25m";

  // Set routing to ComponentLeadAgent
  spec.routing.origin_node = agent_id_;
  spec.routing.target_level = 1;
  spec.routing.target_agent_type = "ComponentLeadAgent";
  spec.routing.target_agent_id = component_agent_id;

  // Set hierarchy
  spec.hierarchy.level0_goal = user_goal;
  spec.hierarchy.level1_directive = user_goal;  // Pass user goal as directive

  // Set action
  spec.action.type = "DECOMPOSE";
  spec.action.priority = "NORMAL";

  // Set aggregation strategy
  spec.aggregation.strategy = "WAIT_ALL";
  spec.aggregation.timeout = "25m";

  // Generate YAML
  std::string yaml_spec = network::YamlParser::generateTaskSpec(spec);

  // Submit task via gRPC
  try {
    auto response = coordinator_client_->submitTask(yaml_spec, session_id, 25 * 60 * 1000,
                                                    hmas::TASK_PRIORITY_NORMAL, "");

    // Wait for result
    auto result = coordinator_client_->getTaskResult(response.task_id(), 25 * 60 * 1000);

    if (result.success()) {
      // Parse result YAML
      auto result_spec_opt = network::YamlParser::parseTaskSpec(result.result_yaml());
      if (result_spec_opt && result_spec_opt->status.result) {
        return *result_spec_opt->status.result;
      }
      return result.result_yaml();
    } else {
      return "ERROR: " + result.error_message();
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to submit/receive task: " + std::string(e.what()));
  }
}

void ChiefArchitectAgent::startHeartbeat() {
  if (heartbeat_running_) {
    return;
  }

  heartbeat_running_ = true;
  heartbeat_thread_ = std::thread(&ChiefArchitectAgent::heartbeatLoop, this);
}

void ChiefArchitectAgent::stopHeartbeat() {
  if (!heartbeat_running_) {
    return;
  }

  heartbeat_running_ = false;
  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }
}

void ChiefArchitectAgent::heartbeatLoop() {
  while (heartbeat_running_) {
    try {
      if (registry_client_) {
        registry_client_->heartbeat(agent_id_, 0.0f, 0.0f, 0);
      }
    } catch (const std::exception& e) {
      concurrency::Logger::error("Heartbeat failed: {}", e.what());
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void ChiefArchitectAgent::shutdown() {
  stopHeartbeat();

  if (registry_client_) {
    try {
      registry_client_->unregisterAgent(agent_id_, "Shutdown requested");
    } catch (const std::exception& e) {
      concurrency::Logger::error("Failed to unregister agent: {}", e.what());
    }
  }

  coordinator_client_.reset();
  registry_client_.reset();
}

std::string ChiefArchitectAgent::queryComponentLeadAgent() {
  if (!registry_client_) {
    return "";
  }

  try {
    hmas::AgentQuery query;
    query.set_agent_type("ComponentLeadAgent");
    query.set_level(1);
    query.set_only_alive(true);

    auto agent_list = registry_client_->queryAgents(query);

    if (agent_list.agents_size() > 0) {
      return agent_list.agents(0).agent_id();
    }
  } catch (const std::exception& e) {
    concurrency::Logger::error("Failed to query ComponentLeadAgent: {}", e.what());
  }

  return "";
}
#endif

}  // namespace agents
}  // namespace keystone
