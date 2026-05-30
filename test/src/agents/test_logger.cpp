/**
 * @file test_logger.cpp
 * @brief Unit tests for keystone::concurrency Logger / LogContext / formatter
 *
 * Exercises the ported, self-contained logger (src/concurrency/logger.cpp and
 * the header-only formatter + Logger template wrappers in logger.hpp). The
 * logger writes to stderr; these tests drive every public entry point so the
 * formatter, context injection, level gating and correlation-scope paths are
 * all executed. Output correctness on stderr is not asserted (it is a side
 * channel), but the context/format helpers that build the messages are.
 */

#include <string>

#include "concurrency/logger.hpp"
#include <gtest/gtest.h>

using keystone::concurrency::CorrelationScope;
using keystone::concurrency::generateCorrelationId;
using keystone::concurrency::LogContext;
using keystone::concurrency::Logger;
using keystone::concurrency::LogLevel;

namespace {

class LoggerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Logger::init(LogLevel::trace);  // lowest level so every emit path runs
    LogContext::clear();
  }
  void TearDown() override {
    LogContext::clear();
    Logger::setLevel(LogLevel::info);
  }
};

TEST_F(LoggerTest, FormatterReplacesPlaceholdersLeftToRight) {
  const std::string out =
      keystone::concurrency::detail::format("a={} b={} c={}", 1, "two", 3.5);
  EXPECT_EQ(out, "a=1 b=two c=3.5");
}

TEST_F(LoggerTest, FormatterLeavesSurplusPlaceholders) {
  // More placeholders than args: trailing "{}" stays intact.
  const std::string out = keystone::concurrency::detail::format("{} and {}", 1);
  EXPECT_EQ(out, "1 and {}");
}

TEST_F(LoggerTest, FormatterIgnoresSurplusArgs) {
  // More args than placeholders: extra args are dropped.
  const std::string out = keystone::concurrency::detail::format("only {}", 1, 2, 3);
  EXPECT_EQ(out, "only 1");
}

TEST_F(LoggerTest, FormatterWithNoPlaceholders) {
  const std::string out = keystone::concurrency::detail::format("plain text", 42);
  EXPECT_EQ(out, "plain text");
}

TEST_F(LoggerTest, LogContextRoundTrip) {
  LogContext::set("agent-7", 3, "session-x");
  EXPECT_EQ(LogContext::getAgentId(), "agent-7");
  EXPECT_EQ(LogContext::getWorkerId(), 3);
  EXPECT_EQ(LogContext::getSessionId(), "session-x");

  const std::string ctx = LogContext::getContextString();
  EXPECT_NE(ctx.find("agent-7"), std::string::npos);

  LogContext::clear();
  EXPECT_EQ(LogContext::getWorkerId(), -1);
  EXPECT_TRUE(LogContext::getAgentId().empty());
}

TEST_F(LoggerTest, CorrelationIdSetAndClear) {
  LogContext::setCorrelationId("corr-123");
  EXPECT_EQ(LogContext::getCorrelationId(), "corr-123");
  LogContext::clearCorrelationId();
  EXPECT_TRUE(LogContext::getCorrelationId().empty());
}

TEST_F(LoggerTest, GenerateCorrelationIdHasUuidShape) {
  const std::string id = generateCorrelationId();
  // UUID4 canonical form: 8-4-4-4-12 = 36 chars with hyphens.
  EXPECT_EQ(id.size(), 36u);
  EXPECT_EQ(id[8], '-');
  EXPECT_EQ(id[13], '-');
  EXPECT_EQ(id[18], '-');
  EXPECT_EQ(id[23], '-');
}

TEST_F(LoggerTest, CorrelationScopeRestoresPreviousId) {
  LogContext::setCorrelationId("outer");
  {
    CorrelationScope scope("inner");
    EXPECT_EQ(scope.id(), "inner");
    EXPECT_EQ(LogContext::getCorrelationId(), "inner");
  }
  // Previous correlation id restored on scope exit.
  EXPECT_EQ(LogContext::getCorrelationId(), "outer");
}

TEST_F(LoggerTest, CorrelationScopeDefaultGeneratesId) {
  LogContext::clearCorrelationId();
  {
    CorrelationScope scope;  // auto-generated id
    EXPECT_FALSE(scope.id().empty());
    EXPECT_EQ(LogContext::getCorrelationId(), scope.id());
  }
  EXPECT_TRUE(LogContext::getCorrelationId().empty());
}

TEST_F(LoggerTest, AllSeverityLevelsEmitWithoutThrowing) {
  LogContext::set("logger-agent", 1, "sess");
  Logger::trace("trace {}", 1);
  Logger::debug("debug {}", 2);
  Logger::info("info {}", 3);
  Logger::warn("warn {}", 4);
  Logger::error("error {}", 5);
  Logger::critical("critical {}", 6);
  SUCCEED();
}

TEST_F(LoggerTest, LevelGatingSuppressesLowerSeverity) {
  // Raise the threshold; trace/debug should be filtered out (early-return path).
  Logger::setLevel(LogLevel::err);
  Logger::trace("should be suppressed {}", 0);
  Logger::debug("should be suppressed {}", 0);
  Logger::error("should emit {}", 1);
  SUCCEED();
}

TEST_F(LoggerTest, ShutdownIsSafeToCall) {
  Logger::shutdown();
  // Re-init so other tests in the binary keep a known level.
  Logger::init(LogLevel::info);
  SUCCEED();
}

}  // namespace
