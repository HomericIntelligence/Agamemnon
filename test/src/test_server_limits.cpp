#include <cstdlib>
#include <stdexcept>
#include <string>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"
#include <gtest/gtest.h>

namespace agamemnon::test {

// Mirrors the env_int helper in server_main.cpp so limits logic can be tested
// independently without linking the server binary.
static int env_int(const char* name, int def) {
  const char* v = std::getenv(name);
  return v ? std::stoi(v) : def;
}

static int64_t env_int64(const char* name, int64_t def) {
  const char* v = std::getenv(name);
  return v ? static_cast<int64_t>(std::stoll(v)) : def;
}

// ── env_int helper ────────────────────────────────────────────────────────────

TEST(EnvIntTest, ReturnsDefaultWhenUnset) {
  unsetenv("_TEST_ENV_INT_ABSENT");
  EXPECT_EQ(env_int("_TEST_ENV_INT_ABSENT", 42), 42);
}

TEST(EnvIntTest, ReturnsValueWhenSet) {
  setenv("_TEST_ENV_INT_PRESENT", "99", 1);
  EXPECT_EQ(env_int("_TEST_ENV_INT_PRESENT", 42), 99);
  unsetenv("_TEST_ENV_INT_PRESENT");
}

TEST(EnvIntTest, ReturnsZeroDefault) {
  unsetenv("_TEST_ENV_INT_ZERO");
  EXPECT_EQ(env_int("_TEST_ENV_INT_ZERO", 0), 0);
}

// ── env_int64 helper ──────────────────────────────────────────────────────────

TEST(EnvInt64Test, ReturnsDefaultWhenUnset) {
  unsetenv("_TEST_ENV_INT64_ABSENT");
  EXPECT_EQ(env_int64("_TEST_ENV_INT64_ABSENT", 3600LL), 3600LL);
}

TEST(EnvInt64Test, ReturnsValueWhenSet) {
  setenv("_TEST_ENV_INT64_PRESENT", "7200", 1);
  EXPECT_EQ(env_int64("_TEST_ENV_INT64_PRESENT", 3600LL), 7200LL);
  unsetenv("_TEST_ENV_INT64_PRESENT");
}

// ── NATS stream retention constants ──────────────────────────────────────────

TEST(NatsStreamLimitsTest, MaxBytesDefaultIs50MB) {
  unsetenv("NATS_STREAM_MAX_BYTES_MB");
  const int64_t max_bytes = env_int64("NATS_STREAM_MAX_BYTES_MB", 50) * 1024LL * 1024LL;
  EXPECT_EQ(max_bytes, 50LL * 1024 * 1024);
}

TEST(NatsStreamLimitsTest, MaxAgeDefaultIs3600Seconds) {
  unsetenv("NATS_STREAM_MAX_AGE_SEC");
  const int64_t max_age_ns = env_int64("NATS_STREAM_MAX_AGE_SEC", 3600) * 1000000000LL;
  EXPECT_EQ(max_age_ns, 3600LL * 1000000000LL);
}

TEST(NatsStreamLimitsTest, MaxBytesOverride) {
  setenv("NATS_STREAM_MAX_BYTES_MB", "100", 1);
  const int64_t max_bytes = env_int64("NATS_STREAM_MAX_BYTES_MB", 50) * 1024LL * 1024LL;
  EXPECT_EQ(max_bytes, 100LL * 1024 * 1024);
  unsetenv("NATS_STREAM_MAX_BYTES_MB");
}

TEST(NatsStreamLimitsTest, MaxAgeOverride) {
  setenv("NATS_STREAM_MAX_AGE_SEC", "7200", 1);
  const int64_t max_age_ns = env_int64("NATS_STREAM_MAX_AGE_SEC", 3600) * 1000000000LL;
  EXPECT_EQ(max_age_ns, 7200LL * 1000000000LL);
  unsetenv("NATS_STREAM_MAX_AGE_SEC");
}

// ── httplib server limit knobs ────────────────────────────────────────────────

TEST(HttplibServerLimitsTest, PayloadLimitApplied) {
  unsetenv("SERVER_REQUEST_SIZE_LIMIT_MB");
  const size_t limit =
      static_cast<size_t>(env_int("SERVER_REQUEST_SIZE_LIMIT_MB", 4)) * 1024UL * 1024UL;
  EXPECT_EQ(limit, 4UL * 1024 * 1024);

  httplib::Server srv;
  // set_payload_max_length must not throw/crash
  srv.set_payload_max_length(limit);
}

TEST(HttplibServerLimitsTest, PayloadLimitOverride) {
  setenv("SERVER_REQUEST_SIZE_LIMIT_MB", "8", 1);
  const size_t limit =
      static_cast<size_t>(env_int("SERVER_REQUEST_SIZE_LIMIT_MB", 4)) * 1024UL * 1024UL;
  EXPECT_EQ(limit, 8UL * 1024 * 1024);
  unsetenv("SERVER_REQUEST_SIZE_LIMIT_MB");
}

TEST(HttplibServerLimitsTest, ThreadCountDefault) {
  unsetenv("SERVER_THREAD_COUNT");
  EXPECT_EQ(env_int("SERVER_THREAD_COUNT", 8), 8);
}

TEST(HttplibServerLimitsTest, ReadTimeoutDefault) {
  unsetenv("SERVER_READ_TIMEOUT_SEC");
  EXPECT_EQ(env_int("SERVER_READ_TIMEOUT_SEC", 10), 10);
}

TEST(HttplibServerLimitsTest, WriteTimeoutDefault) {
  unsetenv("SERVER_WRITE_TIMEOUT_SEC");
  EXPECT_EQ(env_int("SERVER_WRITE_TIMEOUT_SEC", 10), 10);
}

TEST(HttplibServerLimitsTest, TimeoutSetDoesNotCrash) {
  httplib::Server srv;
  srv.set_read_timeout(10);
  srv.set_write_timeout(10);
}

}  // namespace agamemnon::test
