/**
 * @file test_metrics.cpp
 * @brief Unit tests for keystone::core::Metrics (ported HMAS metrics collector)
 *
 * Exercises the ported src/core/metrics.cpp under the coverage harness. The
 * Metrics singleton tracks throughput, latency, queue depth, worker
 * utilisation, priority distribution and deadline misses. All operations are
 * pure in-memory bookkeeping, so they can be validated deterministically.
 */

#include <string>

#include "core/message.hpp"
#include "core/metrics.hpp"
#include <gtest/gtest.h>

using keystone::core::Metrics;
using keystone::core::Priority;

namespace {

// The Metrics instance is a process-wide singleton; reset it before each test
// so accumulated state from other suites does not leak in.
class MetricsTest : public ::testing::Test {
 protected:
  void SetUp() override { Metrics::getInstance().reset(); }
  void TearDown() override { Metrics::getInstance().reset(); }
};

TEST_F(MetricsTest, StartsEmptyAfterReset) {
  auto& m = Metrics::getInstance();
  EXPECT_EQ(m.getTotalMessagesSent(), 0u);
  EXPECT_EQ(m.getTotalMessagesProcessed(), 0u);
  EXPECT_EQ(m.getMaxQueueDepth(), 0u);
  EXPECT_EQ(m.getTotalDeadlineMisses(), 0u);
  EXPECT_EQ(m.getInFlightCount(), 0);
  EXPECT_FALSE(m.getAverageLatencyUs().has_value());
  EXPECT_FALSE(m.getAverageDeadlineMissMs().has_value());
  EXPECT_DOUBLE_EQ(m.getWorkerUtilization(), 0.0);
}

TEST_F(MetricsTest, RecordsMessagesSentAndPriorityDistribution) {
  auto& m = Metrics::getInstance();
  m.recordMessageSent("m-high", Priority::HIGH);
  m.recordMessageSent("m-normal", Priority::NORMAL);
  m.recordMessageSent("m-low", Priority::LOW);
  m.recordMessageSent("m-high2", Priority::HIGH);

  EXPECT_EQ(m.getTotalMessagesSent(), 4u);

  auto stats = m.getPriorityStats();
  EXPECT_EQ(stats.high_count, 2u);
  EXPECT_EQ(stats.normal_count, 1u);
  EXPECT_EQ(stats.low_count, 1u);
}

TEST_F(MetricsTest, RecordsProcessedAndComputesLatency) {
  auto& m = Metrics::getInstance();
  m.recordMessageSent("msg-1", Priority::NORMAL);
  m.recordMessageProcessed("msg-1");

  EXPECT_EQ(m.getTotalMessagesProcessed(), 1u);
  // A send/process pair produces at least one latency sample.
  auto latency = m.getAverageLatencyUs();
  ASSERT_TRUE(latency.has_value());
  EXPECT_GE(*latency, 0.0);
}

TEST_F(MetricsTest, ProcessedWithoutSendDoesNotCreateLatencySample) {
  auto& m = Metrics::getInstance();
  // No matching recordMessageSent -> no timestamp -> no latency sample.
  m.recordMessageProcessed("never-sent");
  EXPECT_EQ(m.getTotalMessagesProcessed(), 1u);
  EXPECT_FALSE(m.getAverageLatencyUs().has_value());
}

TEST_F(MetricsTest, TracksMaxQueueDepth) {
  auto& m = Metrics::getInstance();
  m.recordQueueDepth("agent-a", 5);
  m.recordQueueDepth("agent-b", 12);
  m.recordQueueDepth("agent-a", 3);  // lower value must not lower the max
  EXPECT_EQ(m.getMaxQueueDepth(), 12u);
}

TEST_F(MetricsTest, QueueDepthWarningAndCriticalThresholdsAreHandled) {
  auto& m = Metrics::getInstance();
  // Exceed the WARNING threshold (1000) and the CRITICAL threshold (10000).
  // These branches log via the Logger but must not throw and must update max.
  m.recordQueueDepth("warn-agent", 1500);
  m.recordQueueDepth("crit-agent", 15000);
  EXPECT_EQ(m.getMaxQueueDepth(), 15000u);
}

TEST_F(MetricsTest, WorkerUtilisationReflectsBusySamples) {
  auto& m = Metrics::getInstance();
  m.recordWorkerActivity(0, true);
  m.recordWorkerActivity(1, true);
  m.recordWorkerActivity(2, false);
  m.recordWorkerActivity(3, false);
  // 2 busy of 4 samples == 50%.
  EXPECT_DOUBLE_EQ(m.getWorkerUtilization(), 50.0);
}

TEST_F(MetricsTest, InFlightCountRoundTrips) {
  auto& m = Metrics::getInstance();
  m.setInFlightCount(7);
  EXPECT_EQ(m.getInFlightCount(), 7);
  m.setInFlightCount(0);
  EXPECT_EQ(m.getInFlightCount(), 0);
}

TEST_F(MetricsTest, DeadlineMissesAggregate) {
  auto& m = Metrics::getInstance();
  m.recordDeadlineMiss("late-1", 10);
  m.recordDeadlineMiss("late-2", 30);
  EXPECT_EQ(m.getTotalDeadlineMisses(), 2u);

  auto avg = m.getAverageDeadlineMissMs();
  ASSERT_TRUE(avg.has_value());
  EXPECT_DOUBLE_EQ(*avg, 20.0);
}

TEST_F(MetricsTest, MessagesPerSecondIsNonNegative) {
  auto& m = Metrics::getInstance();
  m.recordMessageSent("x", Priority::NORMAL);
  m.recordMessageProcessed("x");
  EXPECT_GE(m.getMessagesPerSecond(), 0.0);
}

TEST_F(MetricsTest, GenerateReportContainsKeySections) {
  auto& m = Metrics::getInstance();
  m.recordMessageSent("r-high", Priority::HIGH);
  m.recordMessageSent("r-normal", Priority::NORMAL);
  m.recordMessageProcessed("r-high");
  m.recordQueueDepth("agent", 4);
  m.recordWorkerActivity(0, true);
  m.recordDeadlineMiss("d", 5);

  const std::string report = m.generateReport();
  EXPECT_NE(report.find("HMAS Performance Metrics"), std::string::npos);
  EXPECT_NE(report.find("Throughput:"), std::string::npos);
  EXPECT_NE(report.find("Latency:"), std::string::npos);
  EXPECT_NE(report.find("Priority Distribution:"), std::string::npos);
  EXPECT_NE(report.find("Queue Management:"), std::string::npos);
  EXPECT_NE(report.find("Worker Utilization:"), std::string::npos);
  EXPECT_NE(report.find("Deadline Tracking:"), std::string::npos);
}

TEST_F(MetricsTest, GenerateReportHandlesEmptyState) {
  auto& m = Metrics::getInstance();
  // With no data, the "No data" branches for latency and deadlines run.
  const std::string report = m.generateReport();
  EXPECT_NE(report.find("No data"), std::string::npos);
}

TEST_F(MetricsTest, ResetClearsAllCounters) {
  auto& m = Metrics::getInstance();
  m.recordMessageSent("a", Priority::HIGH);
  m.recordMessageProcessed("a");
  m.recordQueueDepth("agent", 50);
  m.recordWorkerActivity(0, true);
  m.recordDeadlineMiss("d", 9);
  m.setInFlightCount(3);

  m.reset();

  EXPECT_EQ(m.getTotalMessagesSent(), 0u);
  EXPECT_EQ(m.getTotalMessagesProcessed(), 0u);
  EXPECT_EQ(m.getMaxQueueDepth(), 0u);
  EXPECT_EQ(m.getTotalDeadlineMisses(), 0u);
  EXPECT_EQ(m.getInFlightCount(), 0);
  auto stats = m.getPriorityStats();
  EXPECT_EQ(stats.high_count, 0u);
  EXPECT_EQ(stats.normal_count, 0u);
  EXPECT_EQ(stats.low_count, 0u);
}

}  // namespace
