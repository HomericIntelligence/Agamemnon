/**
 * @file test_message_factory.cpp
 * @brief Unit tests for keystone::core::KeystoneMessage / Response factories
 *
 * Exercises the ported src/core/message.cpp factory methods, deadline helpers
 * and Response constructors under the coverage harness. These are pure value
 * builders with no external dependencies.
 */

#include <chrono>
#include <thread>

#include "core/message.hpp"
#include <gtest/gtest.h>

using keystone::core::ActionType;
using keystone::core::ContentType;
using keystone::core::KeystoneMessage;
using keystone::core::Priority;
using keystone::core::Response;

namespace {

TEST(MessageFactoryTest, CreateCommandMessageSetsDefaults) {
  auto msg = KeystoneMessage::create("alice", "bob", "echo hi");
  EXPECT_EQ(msg.sender_id, "alice");
  EXPECT_EQ(msg.receiver_id, "bob");
  EXPECT_EQ(msg.action_type, ActionType::EXECUTE);
  EXPECT_EQ(msg.content_type, ContentType::TEXT_PLAIN);
  EXPECT_EQ(msg.session_id, "default");
  EXPECT_EQ(msg.priority, Priority::NORMAL);
  // Auto-generated UUID-shaped id (32 hex + 4 hyphens).
  EXPECT_EQ(msg.msg_id.size(), 36u);
}

TEST(MessageFactoryTest, GeneratedIdsAreUnique) {
  auto a = KeystoneMessage::create("s", "r", "cmd");
  auto b = KeystoneMessage::create("s", "r", "cmd");
  EXPECT_NE(a.msg_id, b.msg_id);
}

TEST(MessageFactoryTest, CreateWithActionOverload) {
  auto msg = KeystoneMessage::create("alice", "bob", ActionType::DECOMPOSE, "sess-1",
                                     std::string("payload"), ContentType::BINARY_CISTA);
  EXPECT_EQ(msg.action_type, ActionType::DECOMPOSE);
  EXPECT_EQ(msg.session_id, "sess-1");
  EXPECT_EQ(msg.content_type, ContentType::BINARY_CISTA);
  ASSERT_TRUE(msg.payload.has_value());
  EXPECT_EQ(*msg.payload, "payload");
  EXPECT_FALSE(msg.deadline.has_value());
}

TEST(MessageFactoryTest, CreateCancellationMessage) {
  auto msg = KeystoneMessage::createCancellation("parent", "child", "task-42", "sess");
  EXPECT_EQ(msg.action_type, ActionType::CANCEL_TASK);
  EXPECT_EQ(msg.task_id, "task-42");
  EXPECT_EQ(msg.priority, Priority::HIGH);
  EXPECT_EQ(msg.session_id, "sess");
}

TEST(MessageFactoryTest, NoDeadlineByDefault) {
  auto msg = KeystoneMessage::create("s", "r", "cmd");
  EXPECT_FALSE(msg.hasDeadlinePassed());
  EXPECT_FALSE(msg.getTimeUntilDeadline().has_value());
}

TEST(MessageFactoryTest, FutureDeadlineNotPassed) {
  auto msg = KeystoneMessage::create("s", "r", "cmd");
  msg.setDeadlineFromNow(std::chrono::milliseconds(10000));
  EXPECT_FALSE(msg.hasDeadlinePassed());
  auto remaining = msg.getTimeUntilDeadline();
  ASSERT_TRUE(remaining.has_value());
  EXPECT_GT(remaining->count(), 0);
}

TEST(MessageFactoryTest, PastDeadlineIsPassed) {
  auto msg = KeystoneMessage::create("s", "r", "cmd");
  msg.setDeadlineFromNow(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(msg.hasDeadlinePassed());
  auto remaining = msg.getTimeUntilDeadline();
  ASSERT_TRUE(remaining.has_value());
  EXPECT_EQ(remaining->count(), 0);  // clamped to zero once passed
}

TEST(MessageFactoryTest, ResponseSuccessFactory) {
  auto msg = KeystoneMessage::create("alice", "bob", "cmd");
  auto resp = Response::createSuccess(msg, "bob", "result-data");
  EXPECT_EQ(resp.status, Response::Status::Success);
  EXPECT_EQ(resp.msg_id, msg.msg_id);
  EXPECT_EQ(resp.sender_id, "bob");
  EXPECT_EQ(resp.receiver_id, "alice");  // routed back to original sender
  EXPECT_EQ(resp.result, "result-data");
}

TEST(MessageFactoryTest, ResponseErrorFactory) {
  auto msg = KeystoneMessage::create("alice", "bob", "cmd");
  auto resp = Response::createError(msg, "bob", "boom");
  EXPECT_EQ(resp.status, Response::Status::Error);
  EXPECT_EQ(resp.receiver_id, "alice");
  EXPECT_EQ(resp.result, "boom");
}

TEST(MessageFactoryTest, CopyAndMovePreserveFields) {
  auto original = KeystoneMessage::create("alice", "bob", "cmd");
  KeystoneMessage copy = original;  // copy ctor (out-of-line)
  EXPECT_EQ(copy.msg_id, original.msg_id);

  KeystoneMessage moved = std::move(copy);  // move ctor (out-of-line)
  EXPECT_EQ(moved.msg_id, original.msg_id);
}

}  // namespace
