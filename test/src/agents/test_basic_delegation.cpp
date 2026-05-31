/**
 * @file phase1_basic_delegation.cpp
 * @brief E2E tests for Phase 1: Basic Delegation (L0 ↔ L3)
 *
 * Tests the simplest 2-agent hierarchy:
 * - ChiefArchitectAgent (Level 0) sends commands
 * - TaskAgent (Level 3) executes commands and returns results
 */

#include <random>
#include <sstream>

#include "agents/chief_architect_agent.hpp"
#include "agents/task_agent.hpp"
#include "core/in_process_router.hpp"
#include <gtest/gtest.h>

using namespace keystone::agents;
using namespace keystone::core;

/**
 * @brief E2E Test: ChiefArchitect sends bash command to add two random numbers
 *
 * Flow:
 *   1. ChiefArchitect generates two random numbers
 *   2. ChiefArchitect sends bash command: "echo $((num1 + num2))"
 *   3. TaskAgent receives command
 *   4. TaskAgent executes bash command
 *   5. TaskAgent sends result back to ChiefArchitect
 *   6. Verify result is correct
 */
TEST(E2E_Phase1, ChiefArchitectDelegatesToTaskAgent) {
  // ARRANGE: Create InProcessRouter and agents
  auto bus = std::make_unique<InProcessRouter>();
  auto chief = std::make_shared<ChiefArchitectAgent>("chief_1");
  auto task_agent = std::make_shared<TaskAgent>("task_1");

  // Register agents with bus (using shared_ptr for safe lifetime management)
  bus->registerAgent(chief->getAgentId(), chief);
  bus->registerAgent(task_agent->getAgentId(), task_agent);

  // Set message bus for each agent
  chief->setMessageBus(bus.get());
  task_agent->setMessageBus(bus.get());

  // Generate two random numbers
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(1, 100);

  int32_t num1 = dis(gen);
  int32_t num2 = dis(gen);
  int32_t expected_result = num1 + num2;

  std::cout << "\nTest: Adding " << num1 << " + " << num2 << " = " << expected_result << std::endl;

  // Create bash command to add the numbers
  std::stringstream cmd;
  cmd << "echo $((" << num1 << " + " << num2 << "))";
  std::string bash_command = cmd.str();

  // ACT: Send message via InProcessRouter
  auto msg = KeystoneMessage::create(chief->getAgentId(), task_agent->getAgentId(), bash_command);

  // Chief sends message (InProcessRouter routes it)
  chief->sendMessage(msg);

  // TaskAgent processes the message
  auto task_msg = task_agent->getMessage();
  ASSERT_TRUE(task_msg.has_value()) << "TaskAgent should receive message";

  auto response = task_agent->processMessage(*task_msg).get();

  // Response is automatically routed back via InProcessRouter
  // ChiefArchitect receives it
  auto chief_response_msg = chief->getMessage();
  ASSERT_TRUE(chief_response_msg.has_value()) << "ChiefArchitect should receive response";

  auto final_response = chief->processMessage(*chief_response_msg).get();

  // ASSERT: Verify the result
  EXPECT_EQ(final_response.status, Response::Status::Success) << "Command should succeed";
  EXPECT_EQ(std::stoi(final_response.result), expected_result)
      << "Result should be " << expected_result << ", got " << final_response.result;

  std::cout << "✓ TaskAgent correctly computed: " << num1 << " + " << num2 << " = "
            << final_response.result << std::endl;

  // Verify command was logged
  auto history = task_agent->getCommandHistory();
  ASSERT_EQ(history.size(), 1) << "TaskAgent should have executed exactly one command";
  EXPECT_EQ(history[0].first, bash_command) << "Command in history should match";
  EXPECT_EQ(std::stoi(history[0].second), expected_result) << "Result in history should match";
}

/**
 * @brief E2E Test: Multiple sequential commands
 */
TEST(E2E_Phase1, ChiefArchitectSendsMultipleCommands) {
  // ARRANGE: Create InProcessRouter and agents
  auto bus = std::make_unique<InProcessRouter>();
  auto chief = std::make_shared<ChiefArchitectAgent>("chief_1");
  auto task_agent = std::make_shared<TaskAgent>("task_1");

  // Register agents with bus (using shared_ptr for safe lifetime management)
  bus->registerAgent(chief->getAgentId(), chief);
  bus->registerAgent(task_agent->getAgentId(), task_agent);

  // Set message bus for each agent
  chief->setMessageBus(bus.get());
  task_agent->setMessageBus(bus.get());

  // Test cases: (num1, num2, expected_result)
  std::vector<std::tuple<int32_t, int32_t, int32_t>> test_cases = {
      {5, 3, 8}, {15, 27, 42}, {100, 200, 300}};

  // ACT & ASSERT: Send each command
  for (const auto& [num1, num2, expected] : test_cases) {
    std::stringstream cmd;
    cmd << "echo $((" << num1 << " + " << num2 << "))";
    std::string bash_command = cmd.str();

    // Send message via InProcessRouter
    auto msg = KeystoneMessage::create(chief->getAgentId(), task_agent->getAgentId(), bash_command);

    chief->sendMessage(msg);

    // Process
    auto task_msg = task_agent->getMessage();
    ASSERT_TRUE(task_msg.has_value());
    auto response = task_agent->processMessage(*task_msg).get();

    // Response automatically routed back via InProcessRouter
    // Verify
    auto chief_response_msg = chief->getMessage();
    ASSERT_TRUE(chief_response_msg.has_value());
    auto final_response = chief->processMessage(*chief_response_msg).get();

    EXPECT_EQ(final_response.status, Response::Status::Success);
    EXPECT_EQ(std::stoi(final_response.result), expected)
        << "Expected " << expected << ", got " << final_response.result;

    std::cout << "✓ " << num1 << " + " << num2 << " = " << final_response.result << std::endl;
  }

  // Verify all commands in history
  auto history = task_agent->getCommandHistory();
  EXPECT_EQ(history.size(), 3u) << "Should have executed 3 commands";
}

/**
 * @brief E2E Test: TaskAgent handles invalid commands gracefully
 */
TEST(E2E_Phase1, TaskAgentHandlesCommandErrors) {
  // ARRANGE: Create InProcessRouter and agents
  auto bus = std::make_unique<InProcessRouter>();
  auto chief = std::make_shared<ChiefArchitectAgent>("chief_1");
  auto task_agent = std::make_shared<TaskAgent>("task_1");

  // Register agents with bus (using shared_ptr for safe lifetime management)
  bus->registerAgent(chief->getAgentId(), chief);
  bus->registerAgent(task_agent->getAgentId(), task_agent);

  // Set message bus for each agent
  chief->setMessageBus(bus.get());
  task_agent->setMessageBus(bus.get());

  // Invalid command
  std::string invalid_command = "this_command_does_not_exist_12345";

  // ACT: Send invalid command via InProcessRouter
  auto msg =
      KeystoneMessage::create(chief->getAgentId(), task_agent->getAgentId(), invalid_command);

  chief->sendMessage(msg);

  auto task_msg = task_agent->getMessage();
  ASSERT_TRUE(task_msg.has_value());
  auto response = task_agent->processMessage(*task_msg).get();

  // ASSERT: Should get error response
  EXPECT_EQ(response.status, Response::Status::Error)
      << "Invalid command should return error status";
  EXPECT_FALSE(response.result.empty()) << "Error response should contain error message";

  std::cout << "✓ TaskAgent correctly handled invalid command with error: " << response.result
            << std::endl;
}

// main() provided by GTest::gtest_main (ADR-015 port adaptation).
