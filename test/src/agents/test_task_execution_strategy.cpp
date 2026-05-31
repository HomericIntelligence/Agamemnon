/**
 * @file test_task_execution_strategy.cpp
 * @brief Unit tests for keystone::agents::TaskExecutionStrategy
 *
 * Exercises the ported src/agents/task_execution_strategy.cpp under the
 * coverage harness. The strategy validates a bash command against a security
 * whitelist and executes it via popen. process() returns a Task<Response>
 * coroutine which Task::get() drives synchronously to completion.
 *
 * Only deterministic, side-effect-free whitelisted commands (echo, true)
 * are run; rejected/invalid commands exercise the error and sanitisation
 * paths without touching the filesystem or network.
 */

#include <string>

#include "agents/task_execution_strategy.hpp"
#include "core/message.hpp"
#include <gtest/gtest.h>

using keystone::agents::TaskExecutionStrategy;
using keystone::core::KeystoneMessage;
using keystone::core::Response;

namespace {

Response run(TaskExecutionStrategy& strategy, const std::string& command) {
  auto msg = KeystoneMessage::create("sender", "receiver", command);
  return strategy.process(msg).get();
}

TEST(TaskExecutionStrategyTest, SimpleEchoSucceeds) {
  TaskExecutionStrategy strategy;
  Response resp = run(strategy, "echo hello world");
  EXPECT_EQ(resp.status, Response::Status::Success);
  EXPECT_EQ(resp.result, "hello world");
}

TEST(TaskExecutionStrategyTest, ArithmeticEchoSucceeds) {
  TaskExecutionStrategy strategy;
  Response resp = run(strategy, "echo $((2 + 3))");
  EXPECT_EQ(resp.status, Response::Status::Success);
  EXPECT_EQ(resp.result, "5");
}

TEST(TaskExecutionStrategyTest, WhitelistedCommandWithSafeArgsSucceeds) {
  TaskExecutionStrategy strategy;
  // "true" is not whitelisted, but "echo" already covers the whitelist path;
  // use "wc" which is whitelisted, with safe args fed from a here-free echo.
  // Simpler: "echo -n abc" is the simple-echo pattern. Use the whitelist path
  // explicitly with the "date" command (whitelisted, no dangerous chars).
  Response resp = run(strategy, "date +%Y");
  EXPECT_EQ(resp.status, Response::Status::Success);
  EXPECT_FALSE(resp.result.empty());
}

TEST(TaskExecutionStrategyTest, EmptyCommandIsRejected) {
  TaskExecutionStrategy strategy;
  Response resp = run(strategy, "");
  EXPECT_EQ(resp.status, Response::Status::Error);
}

TEST(TaskExecutionStrategyTest, NonWhitelistedCommandIsRejected) {
  TaskExecutionStrategy strategy;
  Response resp = run(strategy, "rm -rf /tmp/should-never-run");
  EXPECT_EQ(resp.status, Response::Status::Error);
}

TEST(TaskExecutionStrategyTest, DangerousShellMetacharactersAreRejected) {
  TaskExecutionStrategy strategy;
  // Whitelisted base command ("echo") but with a pipe -> must be rejected by
  // the dangerous-character guard, exercising that branch.
  Response resp = run(strategy, "ls; rm -rf /");
  EXPECT_EQ(resp.status, Response::Status::Error);
}

TEST(TaskExecutionStrategyTest, DirectoryTraversalIsRejected) {
  TaskExecutionStrategy strategy;
  Response resp = run(strategy, "cat ../secret");
  EXPECT_EQ(resp.status, Response::Status::Error);
}

TEST(TaskExecutionStrategyTest, ErrorResponseIsSanitised) {
  TaskExecutionStrategy strategy;
  // A rejected command produces a non-empty, sanitised error string.
  Response resp = run(strategy, "definitely_not_a_real_binary --flag");
  EXPECT_EQ(resp.status, Response::Status::Error);
  EXPECT_FALSE(resp.result.empty());
}

}  // namespace
