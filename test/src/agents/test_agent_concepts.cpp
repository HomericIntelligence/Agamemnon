/**
 * @file test_agent_concepts.cpp
 * @brief Unit tests for C++20 agent concepts (Issue #24)
 *
 * Tests compile-time interface verification using C++20 concepts.
 * Verifies that:
 * - Valid agent types satisfy the Agent concept
 * - Concept-based InProcessRouter::registerAgent works correctly
 * - Compile-time errors for incomplete interfaces
 */

// KeystoneMessage::command is [[deprecated]]; test files intentionally access
// it to verify backward-compat behaviour.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include "agents/chief_architect_agent.hpp"
#include "agents/component_lead_agent.hpp"
#include "agents/concepts.hpp"
#include "agents/module_lead_agent.hpp"
#include "agents/task_agent.hpp"
#include "core/in_process_router.hpp"
#include <gtest/gtest.h>

using namespace keystone::core;
using namespace keystone::agents;

// =============================================================================
// Compile-Time Concept Verification (static_assert)
// =============================================================================

/**
 * @brief Test: All concrete agent types satisfy the Agent concept
 *
 * This test verifies at compile time that all agent implementations
 * satisfy the required interface.
 */
TEST(AgentConcepts, ConcreteAgentsSatisfyAgentConcept) {
  // These will fail at compile time if the concept is not satisfied
  static_assert(Agent<TaskAgent>, "TaskAgent should satisfy Agent concept");
  static_assert(Agent<ChiefArchitectAgent>, "ChiefArchitectAgent should satisfy Agent concept");
  static_assert(Agent<ModuleLeadAgent>, "ModuleLeadAgent should satisfy Agent concept");
  static_assert(Agent<ComponentLeadAgent>, "ComponentLeadAgent should satisfy Agent concept");

  // Runtime assertion to satisfy gtest
  EXPECT_TRUE(true);
}

/**
 * @brief Test: Agent types satisfy individual sub-concepts
 */
TEST(AgentConcepts, AgentsSatisfySubConcepts) {
  // Identifiable
  static_assert(Identifiable<TaskAgent>, "TaskAgent should be Identifiable");
  static_assert(Identifiable<ChiefArchitectAgent>, "ChiefArchitectAgent should be Identifiable");

  // MessageSender
  static_assert(MessageSender<TaskAgent>, "TaskAgent should be MessageSender");
  static_assert(MessageSender<ChiefArchitectAgent>, "ChiefArchitectAgent should be MessageSender");

  // MessageReceiver
  static_assert(MessageReceiver<TaskAgent>, "TaskAgent should be MessageReceiver");
  static_assert(MessageReceiver<ChiefArchitectAgent>,
                "ChiefArchitectAgent should be MessageReceiver");

  // AsyncMessageHandler
  static_assert(AsyncMessageHandler<TaskAgent>, "TaskAgent should be AsyncMessageHandler");
  static_assert(AsyncMessageHandler<ChiefArchitectAgent>,
                "ChiefArchitectAgent should be AsyncMessageHandler");

  // Runtime assertion to satisfy gtest
  EXPECT_TRUE(true);
}

/**
 * @brief Test: Agent types satisfy integration concepts
 */
TEST(AgentConcepts, AgentsSatisfyIntegrationConcepts) {
  // SchedulerAware
  static_assert(SchedulerAware<TaskAgent>, "TaskAgent should be SchedulerAware");
  static_assert(SchedulerAware<ChiefArchitectAgent>,
                "ChiefArchitectAgent should be SchedulerAware");

  // MessageBusAware
  static_assert(MessageBusAware<TaskAgent>, "TaskAgent should be MessageBusAware");
  static_assert(MessageBusAware<ChiefArchitectAgent>,
                "ChiefArchitectAgent should be MessageBusAware");

  // IntegratedAgent
  static_assert(IntegratedAgent<TaskAgent>, "TaskAgent should be IntegratedAgent");
  static_assert(IntegratedAgent<ChiefArchitectAgent>,
                "ChiefArchitectAgent should be IntegratedAgent");

  // Runtime assertion to satisfy gtest
  EXPECT_TRUE(true);
}

// =============================================================================
// Runtime Tests for Concept-Based InProcessRouter::registerAgent
// =============================================================================

/**
 * @brief Test: Concept-based registerAgent works with TaskAgent
 */
TEST(AgentConcepts, ConceptBasedRegisterTaskAgent) {
  InProcessRouter bus;
  auto agent = std::make_shared<TaskAgent>("test_task");

  // Use the new concept-based registerAgent (no explicit agent_id parameter)
  EXPECT_NO_THROW(bus.registerAgent(agent));
  EXPECT_TRUE(bus.hasAgent("test_task"));
}

/**
 * @brief Test: Concept-based registerAgent works with ChiefArchitectAgent
 */
TEST(AgentConcepts, ConceptBasedRegisterChiefArchitect) {
  InProcessRouter bus;
  auto agent = std::make_shared<ChiefArchitectAgent>("test_chief");

  EXPECT_NO_THROW(bus.registerAgent(agent));
  EXPECT_TRUE(bus.hasAgent("test_chief"));
}

/**
 * @brief Test: Concept-based registerAgent works with ModuleLeadAgent
 */
TEST(AgentConcepts, ConceptBasedRegisterModuleLead) {
  InProcessRouter bus;
  auto agent = std::make_shared<ModuleLeadAgent>("test_module");

  EXPECT_NO_THROW(bus.registerAgent(agent));
  EXPECT_TRUE(bus.hasAgent("test_module"));
}

/**
 * @brief Test: Concept-based registerAgent works with ComponentLeadAgent
 */
TEST(AgentConcepts, ConceptBasedRegisterComponentLead) {
  InProcessRouter bus;
  auto agent = std::make_shared<ComponentLeadAgent>("test_component");

  EXPECT_NO_THROW(bus.registerAgent(agent));
  EXPECT_TRUE(bus.hasAgent("test_component"));
}

/**
 * @brief Test: Concept-based registerAgent throws on null pointer
 */
TEST(AgentConcepts, ConceptBasedRegisterNullThrows) {
  InProcessRouter bus;
  std::shared_ptr<TaskAgent> null_agent = nullptr;

  EXPECT_THROW(bus.registerAgent(null_agent), std::runtime_error);
}

/**
 * @brief Test: Concept-based registerAgent throws on duplicate ID
 */
TEST(AgentConcepts, ConceptBasedRegisterDuplicateThrows) {
  InProcessRouter bus;
  auto agent1 = std::make_shared<TaskAgent>("duplicate_id");
  auto agent2 = std::make_shared<TaskAgent>("duplicate_id");

  bus.registerAgent(agent1);
  EXPECT_THROW(bus.registerAgent(agent2), std::runtime_error);
}

/**
 * @brief Test: Concept-based registration works with multiple agent types
 */
TEST(AgentConcepts, ConceptBasedRegisterMultipleTypes) {
  InProcessRouter bus;

  auto chief = std::make_shared<ChiefArchitectAgent>("chief");
  auto component = std::make_shared<ComponentLeadAgent>("component");
  auto module = std::make_shared<ModuleLeadAgent>("module");
  auto task = std::make_shared<TaskAgent>("task");

  // Register all types using concept-based method
  EXPECT_NO_THROW(bus.registerAgent(chief));
  EXPECT_NO_THROW(bus.registerAgent(component));
  EXPECT_NO_THROW(bus.registerAgent(module));
  EXPECT_NO_THROW(bus.registerAgent(task));

  // Verify all registered
  auto agents = bus.listAgents();
  EXPECT_EQ(agents.size(), 4u);
  EXPECT_TRUE(bus.hasAgent("chief"));
  EXPECT_TRUE(bus.hasAgent("component"));
  EXPECT_TRUE(bus.hasAgent("module"));
  EXPECT_TRUE(bus.hasAgent("task"));
}

/**
 * @brief Test: Concept-based registration followed by message routing
 */
TEST(AgentConcepts, ConceptBasedRegisterAndRoute) {
  InProcessRouter bus;

  auto sender = std::make_shared<ChiefArchitectAgent>("sender");
  auto receiver = std::make_shared<TaskAgent>("receiver");

  // Register using concept-based method
  bus.registerAgent(sender);
  bus.registerAgent(receiver);

  // Configure message bus
  sender->setMessageBus(&bus);
  receiver->setMessageBus(&bus);

  // Send message
  auto msg = KeystoneMessage::create("sender", "receiver", "test command");
  EXPECT_TRUE(bus.routeMessage(msg));

  // Verify receipt
  auto received = receiver->getMessage();
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->sender_id, "sender");
  EXPECT_EQ(received->receiver_id, "receiver");
  EXPECT_EQ(received->command, "test command");
}

// =============================================================================
// Negative Tests (Compile-Time Verification)
// =============================================================================

// These are intentionally commented out to show what WOULD fail at compile
// time. Uncomment any of these to verify compile-time error messages.

/*
// Example 1: Type missing getAgentId()
class MissingGetAgentId {
public:
  void sendMessage(const KeystoneMessage& msg) {}
  void receiveMessage(const KeystoneMessage& msg) {}
  concurrency::Task<Response> processMessage(const KeystoneMessage& msg) {
    co_return Response{};
  }
};

TEST(AgentConcepts, CompileErrorMissingGetAgentId) {
  // This should fail at compile time with a clear error message
  static_assert(Agent<MissingGetAgentId>,
                "Should fail: missing getAgentId()");
}
*/

/*
// Example 2: Type with wrong processMessage return type
class WrongProcessMessageReturnType {
public:
  std::string getAgentId() const { return "id"; }
  void sendMessage(const KeystoneMessage& msg) {}
  void receiveMessage(const KeystoneMessage& msg) {}
  void processMessage(const KeystoneMessage& msg) {}  // Wrong: returns void,
not Task<Response>
};

TEST(AgentConcepts, CompileErrorWrongReturnType) {
  // This should fail at compile time
  static_assert(Agent<WrongProcessMessageReturnType>,
                "Should fail: wrong processMessage return type");
}
*/

/*
// Example 3: Type missing sendMessage()
class MissingSendMessage {
public:
  std::string getAgentId() const { return "id"; }
  void receiveMessage(const KeystoneMessage& msg) {}
  concurrency::Task<Response> processMessage(const KeystoneMessage& msg) {
    co_return Response{};
  }
};

TEST(AgentConcepts, CompileErrorMissingSendMessage) {
  // This should fail at compile time
  static_assert(Agent<MissingSendMessage>,
                "Should fail: missing sendMessage()");
}
*/

// =============================================================================
// Documentation Test
// =============================================================================

/**
 * @brief Test: Verify that concepts provide clear documentation
 *
 * This test doesn't actually test runtime behavior, but serves as
 * documentation for how to use the concepts.
 */
TEST(AgentConcepts, UsageDocumentation) {
  // Example 1: Generic function accepting any Agent type
  auto register_any_agent = []<Agent A>(InProcessRouter& bus, std::shared_ptr<A> agent) {
    bus.registerAgent(agent);
  };

  InProcessRouter bus;
  auto task_agent = std::make_shared<TaskAgent>("task");
  auto chief_agent = std::make_shared<ChiefArchitectAgent>("chief");

  // Both work seamlessly
  register_any_agent(bus, task_agent);
  register_any_agent(bus, chief_agent);

  EXPECT_TRUE(bus.hasAgent("task"));
  EXPECT_TRUE(bus.hasAgent("chief"));
}

#pragma GCC diagnostic pop
