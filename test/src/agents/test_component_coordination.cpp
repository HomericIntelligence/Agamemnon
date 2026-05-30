/**
 * @file phase3_component_coordination.cpp
 * @brief E2E tests for Phase 3: Component Lead Coordination (L0 ↔ L1 ↔ L2 ↔ L3)
 *
 * Tests the complete 4-layer hierarchy:
 * - ChiefArchitectAgent (Level 0) delegates component goals
 * - ComponentLeadAgent (Level 1) coordinates multiple modules
 * - ModuleLeadAgent (Level 2) decomposes and coordinates tasks
 * - TaskAgent (Level 3) executes individual tasks
 *
 * Phase 3 introduces:
 * - Component-level coordination
 * - Multi-module management
 * - Module dependency resolution
 * - Full 4-layer message flow
 */

// KeystoneMessage::command is [[deprecated]]; test files intentionally access
// it to verify backward-compat behaviour.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <memory>
#include <sstream>
#include <vector>

#include "agents/chief_architect_agent.hpp"
#include "agents/component_lead_agent.hpp"
#include "agents/module_lead_agent.hpp"
#include "agents/task_agent.hpp"
#include "core/in_process_router.hpp"
#include <gtest/gtest.h>

using namespace keystone::agents;
using namespace keystone::core;

/**
 * @brief E2E Test 3.1: ComponentLead coordinates 2 modules with 6 total
 * TaskAgents
 *
 * Flow:
 *   1. ChiefArchitect sends component goal to ComponentLead
 *   2. ComponentLead decomposes into 2 module goals
 *   3. ComponentLead delegates to 2 ModuleLeadAgents
 *   4. Each ModuleLead decomposes into tasks and delegates to TaskAgents
 *   5. TaskAgents execute (6 total: 3 per module)
 *   6. ModuleLeads synthesize their module results
 *   7. ComponentLead aggregates module results
 *   8. ComponentLead sends final result to ChiefArchitect
 */
TEST(E2E_Phase3, ComponentLeadCoordinatesMultipleModules) {
  // ARRANGE: Create InProcessRouter and full 4-layer hierarchy
  auto bus = std::make_unique<InProcessRouter>();

  auto chief = std::make_shared<ChiefArchitectAgent>("chief_1");
  auto component_lead = std::make_shared<ComponentLeadAgent>("component_core");

  // Create 2 ModuleLeadAgents
  auto module_lead_1 = std::make_shared<ModuleLeadAgent>("module_messaging");
  auto module_lead_2 = std::make_shared<ModuleLeadAgent>("module_concurrency");

  // Create 6 TaskAgents (3 per module)
  std::vector<std::shared_ptr<TaskAgent>> task_agents;
  for (int32_t i = 1; i <= 6; ++i) {
    auto agent = std::make_shared<TaskAgent>("task_" + std::to_string(i));
    task_agents.push_back(agent);
  }

  // Register all agents with InProcessRouter
  bus->registerAgent(chief->getAgentId(), chief);
  bus->registerAgent(component_lead->getAgentId(), component_lead);
  bus->registerAgent(module_lead_1->getAgentId(), module_lead_1);
  bus->registerAgent(module_lead_2->getAgentId(), module_lead_2);
  for (const auto& agent : task_agents) {
    bus->registerAgent(agent->getAgentId(), agent);
  }

  // Set InProcessRouter for all agents
  chief->setMessageBus(bus.get());
  component_lead->setMessageBus(bus.get());
  module_lead_1->setMessageBus(bus.get());
  module_lead_2->setMessageBus(bus.get());
  for (auto& agent : task_agents) {
    agent->setMessageBus(bus.get());
  }

  // Configure ComponentLead with available ModuleLeads
  component_lead->setAvailableModuleLeads(
      {module_lead_1->getAgentId(), module_lead_2->getAgentId()});

  // Configure ModuleLeads with available TaskAgents
  // module_lead_1 gets task_1, task_2, task_3
  module_lead_1->setAvailableTaskAgents(
      {task_agents[0]->getAgentId(), task_agents[1]->getAgentId(), task_agents[2]->getAgentId()});

  // module_lead_2 gets task_4, task_5, task_6
  module_lead_2->setAvailableTaskAgents(
      {task_agents[3]->getAgentId(), task_agents[4]->getAgentId(), task_agents[5]->getAgentId()});

  std::cout << "\n=== Phase 3 E2E Test: Full 4-Layer Hierarchy ===" << std::endl;

  // ACT: ChiefArchitect sends component-level goal
  std::string component_goal =
      "Implement Core component: Messaging(10+20+30) and Concurrency(40+50+60)";

  auto msg_to_component =
      KeystoneMessage::create(chief->getAgentId(), component_lead->getAgentId(), component_goal);

  std::cout << "1. ChiefArchitect → ComponentLead: " << component_goal << std::endl;
  chief->sendMessage(msg_to_component);

  // ComponentLead receives and processes
  auto component_msg = component_lead->getMessage();
  ASSERT_TRUE(component_msg.has_value()) << "ComponentLead should receive goal";

  std::cout << "2. ComponentLead decomposes into 2 modules..." << std::endl;
  auto component_response = component_lead->processMessage(*component_msg).get();

  // ComponentLead should delegate to 2 ModuleLeads
  std::cout << "3. ComponentLead → ModuleLeads (2)..." << std::endl;

  // ModuleLead 1 processes (Messaging module: 10+20+30)
  auto module_1_msg = module_lead_1->getMessage();
  ASSERT_TRUE(module_1_msg.has_value()) << "ModuleLead 1 should receive goal";
  std::cout << "   - ModuleLead 1 (Messaging) receives: " << module_1_msg->command << std::endl;
  module_lead_1->processMessage(*module_1_msg).get();

  // ModuleLead 2 processes (Concurrency module: 40+50+60)
  auto module_2_msg = module_lead_2->getMessage();
  ASSERT_TRUE(module_2_msg.has_value()) << "ModuleLead 2 should receive goal";
  std::cout << "   - ModuleLead 2 (Concurrency) receives: " << module_2_msg->command << std::endl;
  module_lead_2->processMessage(*module_2_msg).get();

  std::cout << "4. ModuleLeads → TaskAgents (6 total)..." << std::endl;

  // All 6 TaskAgents process their tasks
  int32_t tasks_processed = 0;
  for (auto& agent : task_agents) {
    auto task_msg = agent->getMessage();
    if (task_msg.has_value()) {
      std::cout << "   - " << agent->getAgentId() << " processes: " << task_msg->command
                << std::endl;
      agent->processMessage(*task_msg).get();
      tasks_processed++;
    }
  }

  EXPECT_EQ(tasks_processed, 6) << "All 6 TaskAgents should process tasks";

  std::cout << "5. TaskAgents → ModuleLeads (results)..." << std::endl;

  // ModuleLead 1 collects results from its 3 tasks
  for (int32_t i = 0; i < 3; ++i) {
    auto result_msg = module_lead_1->getMessage();
    if (result_msg.has_value()) {
      module_lead_1->processMessage(*result_msg).get();
    }
  }

  // ModuleLead 2 collects results from its 3 tasks
  for (int32_t i = 0; i < 3; ++i) {
    auto result_msg = module_lead_2->getMessage();
    if (result_msg.has_value()) {
      module_lead_2->processMessage(*result_msg).get();
    }
  }

  std::cout << "6. ModuleLeads synthesize results..." << std::endl;

  auto module_1_result = module_lead_1->synthesizeResults();
  std::cout << "   - Messaging module: " << module_1_result << std::endl;

  auto module_2_result = module_lead_2->synthesizeResults();
  std::cout << "   - Concurrency module: " << module_2_result << std::endl;

  // Send module results back to ComponentLead
  auto msg_from_mod1 = KeystoneMessage::create(
      module_lead_1->getAgentId(), component_lead->getAgentId(), "module_result", module_1_result);
  module_lead_1->sendMessage(msg_from_mod1);

  auto msg_from_mod2 = KeystoneMessage::create(
      module_lead_2->getAgentId(), component_lead->getAgentId(), "module_result", module_2_result);
  module_lead_2->sendMessage(msg_from_mod2);

  std::cout << "7. ComponentLead aggregates module results..." << std::endl;

  // ComponentLead receives module results
  auto comp_result_1 = component_lead->getMessage();
  ASSERT_TRUE(comp_result_1.has_value());
  component_lead->processMessage(*comp_result_1).get();

  auto comp_result_2 = component_lead->getMessage();
  ASSERT_TRUE(comp_result_2.has_value());
  component_lead->processMessage(*comp_result_2).get();

  // ComponentLead synthesizes final component result
  auto component_result = component_lead->synthesizeComponentResult();
  std::cout << "   - Component result: " << component_result << std::endl;

  // Send to ChiefArchitect
  auto final_msg = KeystoneMessage::create(component_lead->getAgentId(), chief->getAgentId(),
                                           "component_result", component_result);
  component_lead->sendMessage(final_msg);

  std::cout << "8. ChiefArchitect receives final result..." << std::endl;

  auto chief_final_msg = chief->getMessage();
  ASSERT_TRUE(chief_final_msg.has_value());
  auto final_response = chief->processMessage(*chief_final_msg).get();

  // ASSERT: Verify full flow
  EXPECT_EQ(final_response.status, Response::Status::Success);

  // Expected: Messaging(60) + Concurrency(150) = 210
  EXPECT_NE(component_result.find("60"), std::string::npos)
      << "Should contain Messaging module sum (60)";
  EXPECT_NE(component_result.find("150"), std::string::npos)
      << "Should contain Concurrency module sum (150)";

  std::cout << "✓ Full 4-layer hierarchy working!" << std::endl;

  // Verify execution traces
  auto comp_trace = component_lead->getExecutionTrace();
  EXPECT_GE(comp_trace.size(), 3) << "ComponentLead should track states";

  auto mod1_trace = module_lead_1->getExecutionTrace();
  auto mod2_trace = module_lead_2->getExecutionTrace();
  EXPECT_GE(mod1_trace.size(), 3) << "ModuleLead 1 should track states";
  EXPECT_GE(mod2_trace.size(), 3) << "ModuleLead 2 should track states";

  // Verify all TaskAgents participated
  for (const auto& agent : task_agents) {
    auto history = agent->getCommandHistory();
    EXPECT_EQ(history.size(), 1u) << agent->getAgentId() << " should execute 1 task";
  }

  std::cout << "\n=== Phase 3 Complete: 4 Layers Working ===\n" << std::endl;
}

// main() provided by GTest::gtest_main (ADR-015 port adaptation).

#pragma GCC diagnostic pop
