/**
 * @file phase2_module_coordination.cpp
 * @brief E2E tests for Phase 2: Module Lead Coordination (L0 ↔ L2 ↔ L3)
 *
 * Tests the 3-layer hierarchy:
 * - ChiefArchitectAgent (Level 0) delegates module goals
 * - ModuleLeadAgent (Level 2) decomposes and coordinates tasks
 * - TaskAgent (Level 3) executes individual tasks
 *
 * Phase 2 introduces:
 * - Task decomposition
 * - Result synthesis
 * - Multi-agent coordination
 */

// KeystoneMessage::command is [[deprecated]]; test files intentionally access
// it to verify backward-compat behaviour.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <memory>
#include <sstream>
#include <vector>

#include "agents/chief_architect_agent.hpp"
#include "agents/module_lead_agent.hpp"
#include "agents/task_agent.hpp"
#include "core/in_process_router.hpp"
#include <gtest/gtest.h>

using namespace keystone::agents;
using namespace keystone::core;

/**
 * @brief E2E Test 2.1: ModuleLead synthesizes results from 3 TaskAgents
 *
 * Flow:
 *   1. ChiefArchitect sends module goal to ModuleLead
 *   2. ModuleLead decomposes goal into 3 tasks
 *   3. ModuleLead delegates tasks to 3 TaskAgents
 *   4. TaskAgents execute and return results
 *   5. ModuleLead synthesizes results into module output
 *   6. ModuleLead sends synthesized result to ChiefArchitect
 *   7. Verify complete flow and result synthesis
 */
TEST(E2E_Phase2, ModuleLeadSynthesizesTaskResults) {
  // ARRANGE: Create InProcessRouter and 3-layer agent hierarchy
  auto bus = std::make_unique<InProcessRouter>();

  auto chief = std::make_shared<ChiefArchitectAgent>("chief_1");
  auto module_lead = std::make_shared<ModuleLeadAgent>("module_messaging");

  // Create 3 TaskAgents for parallel execution
  std::vector<std::shared_ptr<TaskAgent>> task_agents;
  for (int32_t i = 1; i <= 3; ++i) {
    auto agent = std::make_shared<TaskAgent>("task_" + std::to_string(i));
    task_agents.push_back(agent);
  }

  // Register all agents with InProcessRouter (using shared_ptr for safe lifetime
  // management)
  bus->registerAgent(chief->getAgentId(), chief);
  bus->registerAgent(module_lead->getAgentId(), module_lead);
  for (const auto& agent : task_agents) {
    bus->registerAgent(agent->getAgentId(), agent);
  }

  // Set InProcessRouter for all agents
  chief->setMessageBus(bus.get());
  module_lead->setMessageBus(bus.get());
  for (auto& agent : task_agents) {
    agent->setMessageBus(bus.get());
  }

  // Configure ModuleLead with available TaskAgent IDs
  std::vector<std::string> task_agent_ids;
  for (const auto& agent : task_agents) {
    task_agent_ids.push_back(agent->getAgentId());
  }
  module_lead->setAvailableTaskAgents(task_agent_ids);

  std::cout << "\n=== Phase 2 E2E Test: ModuleLead Synthesis ===" << std::endl;

  // ACT: ChiefArchitect sends module-level goal to ModuleLead
  std::string module_goal = "Calculate sum of: 10 + 20 + 30";

  auto msg_to_module =
      KeystoneMessage::create(chief->getAgentId(), module_lead->getAgentId(), module_goal);

  std::cout << "1. ChiefArchitect sends goal to ModuleLead: " << module_goal << std::endl;
  chief->sendMessage(msg_to_module);

  // ModuleLead receives and processes the goal
  auto module_msg = module_lead->getMessage();
  ASSERT_TRUE(module_msg.has_value()) << "ModuleLead should receive goal from ChiefArchitect";

  std::cout << "2. ModuleLead receives goal and decomposes into tasks..." << std::endl;
  auto module_response = module_lead->processMessage(*module_msg).get();

  // ModuleLead should decompose into 3 tasks and send to TaskAgents
  // Expected tasks:
  //   task_1: "echo 10"
  //   task_2: "echo 20"
  //   task_3: "echo 30"

  std::cout << "3. ModuleLead delegates to 3 TaskAgents..." << std::endl;

  // Each TaskAgent processes their assigned task
  int32_t tasks_processed = 0;
  for (auto& agent : task_agents) {
    auto task_msg = agent->getMessage();
    if (task_msg.has_value()) {
      std::cout << "   - " << agent->getAgentId() << " processes task: " << task_msg->command
                << std::endl;
      auto task_response = agent->processMessage(*task_msg).get();
      tasks_processed++;
    }
  }

  EXPECT_EQ(tasks_processed, 3) << "All 3 TaskAgents should process tasks";

  // ModuleLead receives results from all TaskAgents
  std::cout << "4. ModuleLead receives results from TaskAgents..." << std::endl;

  int32_t results_received = 0;
  for (int32_t i = 0; i < 3; ++i) {
    auto result_msg = module_lead->getMessage();
    if (result_msg.has_value()) {
      std::cout << "   - Received result: " << result_msg->payload.value_or("") << std::endl;
      module_lead->processMessage(*result_msg).get();
      results_received++;
    }
  }

  EXPECT_EQ(results_received, 3) << "ModuleLead should receive all 3 results";

  // ModuleLead synthesizes results and sends to ChiefArchitect
  std::cout << "5. ModuleLead synthesizes results..." << std::endl;
  auto synthesized_result = module_lead->synthesizeResults();

  auto final_msg = KeystoneMessage::create(module_lead->getAgentId(), chief->getAgentId(),
                                           "module_result", synthesized_result);
  module_lead->sendMessage(final_msg);

  // ChiefArchitect receives synthesized result
  std::cout << "6. ChiefArchitect receives synthesized result..." << std::endl;
  auto chief_result_msg = chief->getMessage();
  ASSERT_TRUE(chief_result_msg.has_value()) << "ChiefArchitect should receive synthesized result";

  auto final_response = chief->processMessage(*chief_result_msg).get();

  // ASSERT: Verify synthesis
  EXPECT_EQ(final_response.status, Response::Status::Success) << "Module synthesis should succeed";

  // Expected synthesis: "10 + 20 + 30 = 60"
  std::string expected_synthesis = "60";  // Sum of 10, 20, 30
  EXPECT_NE(final_response.result.find(expected_synthesis), std::string::npos)
      << "Synthesized result should contain sum: " << expected_synthesis;

  std::cout << "✓ ModuleLead successfully synthesized: " << final_response.result << std::endl;

  // Verify all TaskAgents participated
  for (const auto& agent : task_agents) {
    auto history = agent->getCommandHistory();
    EXPECT_EQ(history.size(), 1u) << agent->getAgentId() << " should execute exactly 1 task";
  }

  // Verify ModuleLead state transitions
  auto execution_trace = module_lead->getExecutionTrace();
  EXPECT_GE(execution_trace.size(), 3) << "ModuleLead should track state transitions";

  std::cout << "\nExecution trace:";
  for (const auto& state : execution_trace) {
    std::cout << " → " << state;
  }
  std::cout << std::endl;

  std::cout << "\n=== Phase 2 Test Complete ===\n" << std::endl;
}

/**
 * @brief E2E Test 2.2: ModuleLead handles varying number of tasks
 *
 * Tests flexibility in task decomposition (2 tasks instead of 3)
 */
TEST(E2E_Phase2, ModuleLeadHandlesVariableTaskCount) {
  // ARRANGE: Same setup but with 2-task goal
  auto bus = std::make_unique<InProcessRouter>();

  auto chief = std::make_shared<ChiefArchitectAgent>("chief_1");
  auto module_lead = std::make_shared<ModuleLeadAgent>("module_math");

  std::vector<std::shared_ptr<TaskAgent>> task_agents;
  for (int32_t i = 1; i <= 3; ++i) {
    auto agent = std::make_shared<TaskAgent>("task_" + std::to_string(i));
    task_agents.push_back(agent);
  }

  bus->registerAgent(chief->getAgentId(), chief);
  bus->registerAgent(module_lead->getAgentId(), module_lead);
  for (const auto& agent : task_agents) {
    bus->registerAgent(agent->getAgentId(), agent);
  }

  chief->setMessageBus(bus.get());
  module_lead->setMessageBus(bus.get());
  for (auto& agent : task_agents) {
    agent->setMessageBus(bus.get());
  }

  std::vector<std::string> task_agent_ids;
  for (const auto& agent : task_agents) {
    task_agent_ids.push_back(agent->getAgentId());
  }
  module_lead->setAvailableTaskAgents(task_agent_ids);

  // ACT: Goal requiring only 2 tasks
  std::string module_goal = "Calculate: 100 + 200";

  auto msg = KeystoneMessage::create(chief->getAgentId(), module_lead->getAgentId(), module_goal);

  chief->sendMessage(msg);

  auto module_msg = module_lead->getMessage();
  ASSERT_TRUE(module_msg.has_value());
  module_lead->processMessage(*module_msg).get();

  // Process tasks (should be 2)
  int32_t tasks_processed = 0;
  for (auto& agent : task_agents) {
    auto task_msg = agent->getMessage();
    if (task_msg.has_value()) {
      agent->processMessage(*task_msg).get();
      tasks_processed++;
    }
  }

  EXPECT_EQ(tasks_processed, 2) << "Should decompose into 2 tasks";

  // Collect results
  for (int32_t i = 0; i < tasks_processed; ++i) {
    auto result_msg = module_lead->getMessage();
    if (result_msg.has_value()) {
      module_lead->processMessage(*result_msg).get();
    }
  }

  auto synthesized = module_lead->synthesizeResults();
  EXPECT_NE(synthesized.find("300"), std::string::npos) << "Should synthesize to 300";

  std::cout << "✓ ModuleLead handled 2 tasks correctly: " << synthesized << std::endl;
}

// main() provided by GTest::gtest_main (ADR-015 port adaptation).

#pragma GCC diagnostic pop
