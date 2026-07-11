#include "agamemnon/rate_limiter.hpp"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace agamemnon::test {

// Use high burst/rps so tests don't need long sleeps.
static constexpr double kRps = 10.0;
static constexpr double kBurst = 5.0;

TEST(RateLimiterTest, AllowsRequestsWithinBurst) {
  RateLimiter rl(kRps, kBurst);
  for (int i = 0; i < static_cast<int>(kBurst); ++i) {
    EXPECT_TRUE(rl.allow("1.2.3.4")) << "request " << i << " should be allowed";
  }
}

TEST(RateLimiterTest, DeniesRequestsWhenBucketExhausted) {
  RateLimiter rl(kRps, kBurst);
  for (int i = 0; i < static_cast<int>(kBurst); ++i) {
    rl.allow("1.2.3.4");
  }
  EXPECT_FALSE(rl.allow("1.2.3.4"));
}

TEST(RateLimiterTest, AllowsAgainAfterRefill) {
  RateLimiter rl(kRps, kBurst);
  for (int i = 0; i < static_cast<int>(kBurst); ++i) {
    rl.allow("1.2.3.4");
  }
  ASSERT_FALSE(rl.allow("1.2.3.4"));

  // Wait long enough for at least 1 token to refill (1/kRps seconds + margin).
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_TRUE(rl.allow("1.2.3.4"));
}

TEST(RateLimiterTest, DifferentIpsAreIndependent) {
  RateLimiter rl(kRps, kBurst);
  // Exhaust bucket for IP A.
  for (int i = 0; i < static_cast<int>(kBurst); ++i) {
    rl.allow("10.0.0.1");
  }
  ASSERT_FALSE(rl.allow("10.0.0.1"));

  // IP B should still have a full bucket.
  EXPECT_TRUE(rl.allow("10.0.0.2"));
}

TEST(RateLimiterTest, RetryAfterIsZeroWhenTokensAvailable) {
  RateLimiter rl(kRps, kBurst);
  EXPECT_DOUBLE_EQ(rl.retry_after_seconds("1.2.3.4"), 0.0);
}

TEST(RateLimiterTest, RetryAfterIsPositiveWhenBucketExhausted) {
  RateLimiter rl(kRps, kBurst);
  for (int i = 0; i < static_cast<int>(kBurst); ++i) {
    rl.allow("1.2.3.4");
  }
  rl.allow("1.2.3.4");  // one over the limit to ensure exhaustion
  double secs = rl.retry_after_seconds("1.2.3.4");
  EXPECT_GT(secs, 0.0);
  EXPECT_LE(secs, 1.0 / kRps + 0.1);  // should not be more than 1 token's worth
}

TEST(RateLimiterTest, NewClientHasFullBurst) {
  RateLimiter rl(kRps, kBurst);
  // A brand-new IP should be able to make kBurst consecutive requests.
  int allowed = 0;
  for (int i = 0; i < static_cast<int>(kBurst) + 2; ++i) {
    if (rl.allow("192.168.1.1")) ++allowed;
  }
  EXPECT_EQ(allowed, static_cast<int>(kBurst));
}

}  // namespace agamemnon::test
