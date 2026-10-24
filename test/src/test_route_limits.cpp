#include "agamemnon/route_limits.hpp"

#include <cstdlib>

#include <gtest/gtest.h>

namespace agamemnon::test {

// ── env_size helper ───────────────────────────────────────────────────────────

TEST(EnvSizeTest, ReturnsDefaultWhenUnset) {
  unsetenv("_AGAM_TEST_ABSENT");
  EXPECT_EQ(env_size("_AGAM_TEST_ABSENT", 256), 256U);
}

TEST(EnvSizeTest, ReturnsDefaultWhenEmpty) {
  setenv("_AGAM_TEST_EMPTY", "", 1);
  EXPECT_EQ(env_size("_AGAM_TEST_EMPTY", 256), 256U);
  unsetenv("_AGAM_TEST_EMPTY");
}

TEST(EnvSizeTest, ReturnsOverrideWhenSet) {
  setenv("_AGAM_TEST_SET", "9000", 1);
  EXPECT_EQ(env_size("_AGAM_TEST_SET", 256), 9000U);
  unsetenv("_AGAM_TEST_SET");
}

TEST(EnvSizeTest, RejectsZeroNegativeAndGarbage) {
  for (const char* bad : {"0", "-5", "notanumber"}) {
    setenv("_AGAM_TEST_BAD", bad, 1);
    EXPECT_EQ(env_size("_AGAM_TEST_BAD", 256), 256U) << "input=" << bad;
  }
  unsetenv("_AGAM_TEST_BAD");
}

// ── RouteLimits::from_env ─────────────────────────────────────────────────────

namespace {
void unset_all_limit_env_vars() {
  for (const char* n : {"AGAMEMNON_MAX_NAME_LEN", "AGAMEMNON_MAX_LABEL_LEN",
                        "AGAMEMNON_MAX_DESCRIPTION_LEN", "AGAMEMNON_MAX_SUBJECT_LEN",
                        "AGAMEMNON_MAX_PROGRAM_LEN", "AGAMEMNON_MAX_BODY_BYTES",
                        "SERVER_REQUEST_SIZE_LIMIT_MB"}) {
    unsetenv(n);
  }
}
}  // namespace

TEST(RouteLimitsTest, DefaultsMatchHistoricalConstants) {
  // Ensure no env override leaks in from the runner.
  unset_all_limit_env_vars();
  const RouteLimits l = RouteLimits::from_env();
  EXPECT_EQ(l.max_name_len, 256U);
  EXPECT_EQ(l.max_label_len, 256U);
  EXPECT_EQ(l.max_description_len, 4096U);
  EXPECT_EQ(l.max_subject_len, 512U);
  EXPECT_EQ(l.max_program_len, 1024U);
  EXPECT_EQ(l.max_body_bytes, 1U * 1024U * 1024U);  // 1 MiB
}

TEST(RouteLimitsTest, FromEnvAppliesOverrides) {
  setenv("AGAMEMNON_MAX_DESCRIPTION_LEN", "65536", 1);
  setenv("AGAMEMNON_MAX_BODY_BYTES", "4194304", 1);
  const RouteLimits l = RouteLimits::from_env();
  EXPECT_EQ(l.max_description_len, 65536U);
  EXPECT_EQ(l.max_body_bytes, 4194304U);
  EXPECT_EQ(l.max_name_len, 256U);  // untouched -> default
  unsetenv("AGAMEMNON_MAX_DESCRIPTION_LEN");
  unsetenv("AGAMEMNON_MAX_BODY_BYTES");
}

TEST(RouteLimitsTest, InvalidOverrideFallsBackToDefault) {
  unset_all_limit_env_vars();
  setenv("AGAMEMNON_MAX_NAME_LEN", "garbage", 1);
  const RouteLimits l = RouteLimits::from_env();
  EXPECT_EQ(l.max_name_len, 256U);
  unsetenv("AGAMEMNON_MAX_NAME_LEN");
}

TEST(RouteLimitsTest, LegacyMbKnobConvertsToBytes) {
  unset_all_limit_env_vars();
  setenv("SERVER_REQUEST_SIZE_LIMIT_MB", "4", 1);  // MB
  const RouteLimits l = RouteLimits::from_env();
  EXPECT_EQ(l.max_body_bytes, 4U * 1024U * 1024U);  // bytes
  unsetenv("SERVER_REQUEST_SIZE_LIMIT_MB");
}

TEST(RouteLimitsTest, NewBytesKnobWinsOverLegacyMbKnob) {
  setenv("AGAMEMNON_MAX_BODY_BYTES", "1048576", 1);
  setenv("SERVER_REQUEST_SIZE_LIMIT_MB", "4", 1);
  const RouteLimits l = RouteLimits::from_env();
  EXPECT_EQ(l.max_body_bytes, 1048576U);  // bytes knob wins
  unsetenv("AGAMEMNON_MAX_BODY_BYTES");
  unsetenv("SERVER_REQUEST_SIZE_LIMIT_MB");
}

}  // namespace agamemnon::test
