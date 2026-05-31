/**
 * @file test_failure_injector.cpp
 * @brief Unit tests for keystone::core::FailureInjector (ported chaos tool)
 *
 * Exercises the ported src/core/failure_injector.cpp under the coverage
 * harness. A fixed RNG seed makes the probabilistic paths deterministic, so
 * rate 0.0 (never fail) and rate 1.0 (always fail) can be asserted exactly.
 */

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "core/failure_injector.hpp"
#include <gtest/gtest.h>

using keystone::core::FailureInjector;

namespace {

TEST(FailureInjectorTest, CrashRecoverRoundTrip) {
  FailureInjector injector(1234);
  EXPECT_FALSE(injector.isAgentCrashed("agent-1"));

  injector.injectAgentCrash("agent-1");
  EXPECT_TRUE(injector.isAgentCrashed("agent-1"));
  EXPECT_EQ(injector.getTotalFailures(), 1);

  injector.recoverAgent("agent-1");
  EXPECT_FALSE(injector.isAgentCrashed("agent-1"));
  // Recovery does not decrement the injected-failure counter.
  EXPECT_EQ(injector.getTotalFailures(), 1);
}

TEST(FailureInjectorTest, FailedAgentsListReflectsCrashes) {
  FailureInjector injector(7);
  injector.injectAgentCrash("a");
  injector.injectAgentCrash("b");

  auto failed = injector.getFailedAgents();
  EXPECT_EQ(failed.size(), 2u);
  EXPECT_NE(std::find(failed.begin(), failed.end(), "a"), failed.end());
  EXPECT_NE(std::find(failed.begin(), failed.end(), "b"), failed.end());
}

TEST(FailureInjectorTest, TimeoutInjectAndClear) {
  FailureInjector injector(42);
  EXPECT_EQ(injector.getAgentTimeout("slow").count(), 0);

  injector.injectAgentTimeout("slow", std::chrono::milliseconds(250));
  EXPECT_EQ(injector.getAgentTimeout("slow").count(), 250);
  EXPECT_EQ(injector.getTotalFailures(), 1);

  auto timeouts = injector.getTimeoutAgents();
  ASSERT_EQ(timeouts.size(), 1u);
  EXPECT_EQ(timeouts.front(), "slow");

  injector.clearAgentTimeout("slow");
  EXPECT_EQ(injector.getAgentTimeout("slow").count(), 0);
  EXPECT_TRUE(injector.getTimeoutAgents().empty());
}

TEST(FailureInjectorTest, FailureRateIsClamped) {
  FailureInjector injector(99);
  injector.setFailureRate(2.5);  // above 1.0
  EXPECT_DOUBLE_EQ(injector.getFailureRate(), 1.0);

  injector.setFailureRate(-1.0);  // below 0.0
  EXPECT_DOUBLE_EQ(injector.getFailureRate(), 0.0);

  injector.setFailureRate(0.3);
  EXPECT_DOUBLE_EQ(injector.getFailureRate(), 0.3);
}

TEST(FailureInjectorTest, ZeroRateNeverFails) {
  FailureInjector injector(2024);
  injector.setFailureRate(0.0);
  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(injector.shouldFail());
  }
}

TEST(FailureInjectorTest, FullRateAlwaysFails) {
  FailureInjector injector(2024);
  injector.setFailureRate(1.0);
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(injector.shouldFail());
  }
}

TEST(FailureInjectorTest, ShouldAgentFailCombinesCrashAndRandom) {
  FailureInjector injector(555);
  injector.setFailureRate(0.0);

  // Not crashed + zero rate -> never fails.
  EXPECT_FALSE(injector.shouldAgentFail("healthy"));

  // Crashed agent always fails regardless of random rate.
  injector.injectAgentCrash("broken");
  EXPECT_TRUE(injector.shouldAgentFail("broken"));
}

TEST(FailureInjectorTest, StatisticsStringSummarisesState) {
  FailureInjector injector(8);
  injector.injectAgentCrash("crashed-1");
  injector.injectAgentTimeout("slow-1", std::chrono::milliseconds(100));
  injector.setFailureRate(0.25);

  const std::string stats = injector.getStatistics();
  EXPECT_NE(stats.find("FailureInjector Statistics"), std::string::npos);
  EXPECT_NE(stats.find("crashed-1"), std::string::npos);
  EXPECT_NE(stats.find("slow-1"), std::string::npos);
  EXPECT_NE(stats.find("100ms"), std::string::npos);
  EXPECT_NE(stats.find("Random failure rate"), std::string::npos);
}

TEST(FailureInjectorTest, ResetClearsEverything) {
  FailureInjector injector(11);
  injector.injectAgentCrash("a");
  injector.injectAgentTimeout("b", std::chrono::milliseconds(10));
  EXPECT_EQ(injector.getTotalFailures(), 2);

  injector.reset();

  EXPECT_EQ(injector.getTotalFailures(), 0);
  EXPECT_TRUE(injector.getFailedAgents().empty());
  EXPECT_TRUE(injector.getTimeoutAgents().empty());
  EXPECT_FALSE(injector.isAgentCrashed("a"));
}

TEST(FailureInjectorTest, DefaultSeedConstructsWithoutThrowing) {
  // seed == 0 selects std::random_device; just ensure it constructs and runs.
  FailureInjector injector(0);
  injector.setFailureRate(1.0);
  EXPECT_TRUE(injector.shouldFail());
}

}  // namespace
