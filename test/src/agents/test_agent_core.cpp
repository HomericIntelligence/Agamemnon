/**
 * @file test_agent_core.cpp
 * @brief Unit tests for keystone::agents::AgentCore base behaviour
 *
 * Exercises the ported src/agents/agent_core.cpp: priority inbox routing,
 * HIGH/NORMAL/LOW dequeue ordering, the "no message bus" send error path,
 * message routing through an IMessageRouter, and the task-cancellation
 * bookkeeping. AgentCore is concrete (no pure virtuals), so it is instantiated
 * directly. A tiny capturing router validates sendMessage().
 */

#include <optional>
#include <string>
#include <vector>

#include "agents/agent_core.hpp"
#include "core/i_message_router.hpp"
#include "core/message.hpp"
#include <gtest/gtest.h>

using keystone::agents::AgentCore;
using keystone::core::IMessageRouter;
using keystone::core::KeystoneMessage;
using keystone::core::Priority;

namespace {

// Minimal router that records everything routed through it.
class CapturingRouter : public IMessageRouter {
 public:
  bool routeMessage(const KeystoneMessage& msg) override {
    routed.push_back(msg);
    return true;
  }
  std::vector<KeystoneMessage> routed;
};

KeystoneMessage msgWithPriority(const std::string& cmd, Priority p) {
  auto m = KeystoneMessage::create("sender", "agent", cmd);
  m.priority = p;
  return m;
}

TEST(AgentCoreTest, SendMessageThrowsWithoutBus) {
  AgentCore agent("agent-1");
  auto m = KeystoneMessage::create("agent-1", "other", "ping");
  EXPECT_THROW(agent.sendMessage(m), std::runtime_error);
}

TEST(AgentCoreTest, SendMessageRoutesThroughBus) {
  AgentCore agent("agent-1");
  CapturingRouter router;
  agent.setMessageBus(&router);

  auto m = KeystoneMessage::create("agent-1", "other", "ping");
  agent.sendMessage(m);
  ASSERT_EQ(router.routed.size(), 1u);
  EXPECT_EQ(router.routed.front().sender_id, "agent-1");
}

TEST(AgentCoreTest, GetMessageReturnsNulloptWhenEmpty) {
  AgentCore agent("agent-1");
  EXPECT_FALSE(agent.getMessage().has_value());
}

TEST(AgentCoreTest, HighPriorityDequeuedBeforeNormalAndLow) {
  AgentCore agent("agent-1");
  agent.receiveMessage(msgWithPriority("low", Priority::LOW));
  agent.receiveMessage(msgWithPriority("normal", Priority::NORMAL));
  agent.receiveMessage(msgWithPriority("high", Priority::HIGH));

  // HIGH first.
  auto first = agent.getMessage();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->priority, Priority::HIGH);

  // Then NORMAL.
  auto second = agent.getMessage();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->priority, Priority::NORMAL);

  // Then LOW.
  auto third = agent.getMessage();
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third->priority, Priority::LOW);

  // Drained.
  EXPECT_FALSE(agent.getMessage().has_value());
}

TEST(AgentCoreTest, NormalAndLowOnlyDequeueInOrder) {
  AgentCore agent("agent-1");
  agent.receiveMessage(msgWithPriority("n", Priority::NORMAL));
  agent.receiveMessage(msgWithPriority("l", Priority::LOW));

  auto a = agent.getMessage();
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->priority, Priority::NORMAL);

  auto b = agent.getMessage();
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->priority, Priority::LOW);
}

TEST(AgentCoreTest, AgentIdAccessor) {
  AgentCore agent("the-id");
  EXPECT_EQ(agent.getAgentId(), "the-id");
}

TEST(AgentCoreTest, CancellationBookkeeping) {
  AgentCore agent("agent-1");
  EXPECT_FALSE(agent.isCancelled("task-1"));

  agent.requestCancellation("task-1");
  EXPECT_TRUE(agent.isCancelled("task-1"));
  EXPECT_FALSE(agent.isCancelled("task-2"));

  agent.clearCancellation("task-1");
  EXPECT_FALSE(agent.isCancelled("task-1"));
}

}  // namespace
