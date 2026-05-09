#include "projectagamemnon/dead_letter_queue.hpp"

#include <gtest/gtest.h>

namespace projectagamemnon::test {

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

}  // namespace projectagamemnon::test
