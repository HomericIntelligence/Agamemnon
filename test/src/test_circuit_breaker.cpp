#include "projectagamemnon/circuit_breaker.hpp"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace projectagamemnon::test {

using State = CircuitBreaker::State;

TEST(CircuitBreakerTest, InitiallyClosedAndAllowsAttempts) {
  CircuitBreaker cb;
  EXPECT_EQ(cb.state(), State::Closed);
  EXPECT_TRUE(cb.allow_attempt());
}

TEST(CircuitBreakerTest, OpensAfterFailureThreshold) {
  CircuitBreaker::Config cfg;
  cfg.failure_threshold = 3;
  cfg.probe_interval = std::chrono::milliseconds{60'000};
  CircuitBreaker cb(cfg);

  cb.record_failure();
  EXPECT_EQ(cb.state(), State::Closed);
  cb.record_failure();
  EXPECT_EQ(cb.state(), State::Closed);
  cb.record_failure();
  EXPECT_EQ(cb.state(), State::Open);
}

TEST(CircuitBreakerTest, OpenCircuitDeniesAttempts) {
  CircuitBreaker::Config cfg;
  cfg.failure_threshold = 1;
  cfg.probe_interval = std::chrono::milliseconds{60'000};
  CircuitBreaker cb(cfg);

  cb.record_failure();
  ASSERT_EQ(cb.state(), State::Open);
  EXPECT_FALSE(cb.allow_attempt());
}

TEST(CircuitBreakerTest, TransitionsToHalfOpenAfterProbeInterval) {
  CircuitBreaker::Config cfg;
  cfg.failure_threshold = 1;
  cfg.probe_interval = std::chrono::milliseconds{50};
  CircuitBreaker cb(cfg);

  cb.record_failure();
  ASSERT_EQ(cb.state(), State::Open);

  std::this_thread::sleep_for(std::chrono::milliseconds{100});

  EXPECT_TRUE(cb.allow_attempt());
  EXPECT_EQ(cb.state(), State::HalfOpen);
}

TEST(CircuitBreakerTest, ClosesOnProbeSuccess) {
  CircuitBreaker::Config cfg;
  cfg.failure_threshold = 1;
  cfg.probe_interval = std::chrono::milliseconds{50};
  CircuitBreaker cb(cfg);

  cb.record_failure();
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  cb.allow_attempt();  // transitions to HalfOpen
  ASSERT_EQ(cb.state(), State::HalfOpen);

  cb.record_success();
  EXPECT_EQ(cb.state(), State::Closed);
  EXPECT_EQ(cb.failure_count(), 0);
}

TEST(CircuitBreakerTest, ReopensOnProbeFailure) {
  CircuitBreaker::Config cfg;
  cfg.failure_threshold = 1;
  cfg.probe_interval = std::chrono::milliseconds{50};
  CircuitBreaker cb(cfg);

  cb.record_failure();
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  cb.allow_attempt();  // transitions to HalfOpen
  ASSERT_EQ(cb.state(), State::HalfOpen);

  cb.record_failure();
  EXPECT_EQ(cb.state(), State::Open);
}

TEST(CircuitBreakerTest, SuccessResetsFailureCount) {
  CircuitBreaker cb;
  cb.record_failure();
  cb.record_failure();
  EXPECT_GT(cb.failure_count(), 0);

  cb.record_success();
  EXPECT_EQ(cb.failure_count(), 0);
  EXPECT_EQ(cb.state(), State::Closed);
}

TEST(CircuitBreakerTest, StateLabelMatchesState) {
  CircuitBreaker::Config cfg;
  cfg.failure_threshold = 1;
  cfg.probe_interval = std::chrono::milliseconds{60'000};
  CircuitBreaker cb(cfg);

  EXPECT_EQ(cb.state_label(), "closed");
  cb.record_failure();
  EXPECT_EQ(cb.state_label(), "open");
}

}  // namespace projectagamemnon::test
