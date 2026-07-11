#include "agamemnon/dead_letter_queue.hpp"

#include <gtest/gtest.h>

namespace agamemnon::test {

TEST(DeadLetterQueueTest, InitiallyEmpty) {
  DeadLetterQueue dlq;
  EXPECT_TRUE(dlq.empty());
  EXPECT_EQ(dlq.size(), 0u);
}

TEST(DeadLetterQueueTest, PushIncreasesSize) {
  DeadLetterQueue dlq;
  dlq.push("hi.tasks.created", R"({"id":"1"})", 3);
  EXPECT_EQ(dlq.size(), 1u);
  EXPECT_FALSE(dlq.empty());
}

TEST(DeadLetterQueueTest, DrainReturnsAllEntries) {
  DeadLetterQueue dlq;
  dlq.push("subj.a", "payload_a", 1);
  dlq.push("subj.b", "payload_b", 2);

  auto entries = dlq.drain();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].subject, "subj.a");
  EXPECT_EQ(entries[0].payload, "payload_a");
  EXPECT_EQ(entries[0].attempts, 1);
  EXPECT_EQ(entries[1].subject, "subj.b");
  EXPECT_EQ(entries[1].attempts, 2);
}

TEST(DeadLetterQueueTest, DrainEmptiesQueue) {
  DeadLetterQueue dlq;
  dlq.push("s", "p", 1);
  dlq.drain();
  EXPECT_TRUE(dlq.empty());
}

TEST(DeadLetterQueueTest, ClearEmptiesQueue) {
  DeadLetterQueue dlq;
  dlq.push("s", "p", 1);
  dlq.clear();
  EXPECT_TRUE(dlq.empty());
}

TEST(DeadLetterQueueTest, ClearDoesNotReturnEntries) {
  DeadLetterQueue dlq;
  dlq.push("s", "p", 1);
  dlq.clear();
  auto entries = dlq.drain();
  EXPECT_TRUE(entries.empty());
}

TEST(DeadLetterQueueTest, BoundedCapacityEvictsOldest) {
  DeadLetterQueue dlq(3);
  dlq.push("s1", "p1", 1);
  dlq.push("s2", "p2", 1);
  dlq.push("s3", "p3", 1);
  // At capacity; next push evicts s1
  dlq.push("s4", "p4", 1);

  EXPECT_EQ(dlq.size(), 3u);
  auto entries = dlq.drain();
  EXPECT_EQ(entries[0].subject, "s2");
  EXPECT_EQ(entries[2].subject, "s4");
}

TEST(DeadLetterQueueTest, TimestampIsSet) {
  DeadLetterQueue dlq;
  dlq.push("s", "p", 1);
  auto entries = dlq.drain();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_GT(entries[0].timestamp_ms, 0);
}

// ── ADR-005 log level and service fields ───────────────────────────────────

TEST(DeadLetterQueueTest, StoresLevelAndService) {
  DeadLetterQueue dlq;
  dlq.push("hi.logs.agamemnon.task_completed", R"({"timestamp":1.0,"level":"info"})", 2, "info",
           "agamemnon");

  auto entries = dlq.drain();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].level, "info");
  EXPECT_EQ(entries[0].service, "agamemnon");
}

TEST(DeadLetterQueueTest, LevelAndServiceDefaultEmpty) {
  DeadLetterQueue dlq;
  dlq.push("hi.logs.test", R"({"payload":"test"})", 1);

  auto entries = dlq.drain();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].level, "");
  EXPECT_EQ(entries[0].service, "");
}

TEST(DeadLetterQueueTest, LevelAndServicePreservedAcrossMultipleEntries) {
  DeadLetterQueue dlq;
  dlq.push("log1", "p1", 1, "error", "agamemnon");
  dlq.push("log2", "p2", 2, "warn", "orchestrator");
  dlq.push("log3", "p3", 3, "info", "agamemnon");

  auto entries = dlq.drain();
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].level, "error");
  EXPECT_EQ(entries[0].service, "agamemnon");
  EXPECT_EQ(entries[1].level, "warn");
  EXPECT_EQ(entries[1].service, "orchestrator");
  EXPECT_EQ(entries[2].level, "info");
  EXPECT_EQ(entries[2].service, "agamemnon");
}

}  // namespace agamemnon::test
