#include "agamemnon/env_parse.hpp"

#include <gtest/gtest.h>

namespace agamemnon::test {

TEST(EnvParseTest, ValidPositiveDouble) {
  EXPECT_EQ(parse_env_double("100").value(), 100.0);
  EXPECT_EQ(parse_env_double("2.5").value(), 2.5);
  EXPECT_EQ(parse_env_double("0.001").value(), 0.001);
}

TEST(EnvParseTest, NullptrReturnsNullopt) {
  EXPECT_FALSE(parse_env_double(nullptr).has_value());
}

TEST(EnvParseTest, EmptyStringReturnsNullopt) {
  EXPECT_FALSE(parse_env_double("").has_value());
}

TEST(EnvParseTest, TrailingJunkReturnsNullopt) {
  EXPECT_FALSE(parse_env_double("100x").has_value());
  EXPECT_FALSE(parse_env_double("60 ").has_value());
}

TEST(EnvParseTest, NonNumericReturnsNullopt) {
  EXPECT_FALSE(parse_env_double("abc").has_value());
}

TEST(EnvParseTest, ZeroIsRejected) {
  EXPECT_FALSE(parse_env_double("0").has_value());
}

TEST(EnvParseTest, NegativeIsRejected) {
  EXPECT_FALSE(parse_env_double("-1").has_value());
  EXPECT_FALSE(parse_env_double("-0.5").has_value());
}

TEST(EnvParseTest, NonFiniteIsRejected) {
  EXPECT_FALSE(parse_env_double("inf").has_value());
  EXPECT_FALSE(parse_env_double("-inf").has_value());
  EXPECT_FALSE(parse_env_double("nan").has_value());
}

}  // namespace agamemnon::test
