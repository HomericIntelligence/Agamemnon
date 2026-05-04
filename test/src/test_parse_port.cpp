#include "projectagamemnon/port_parse.hpp"

#include <gtest/gtest.h>

namespace projectagamemnon::test {

TEST(ParsePortTest, ValidPorts) {
    EXPECT_EQ(parse_port("8080").port.value(), 8080);
    EXPECT_EQ(parse_port("1").port.value(), 1);
    EXPECT_EQ(parse_port("65535").port.value(), 65535);
    EXPECT_EQ(parse_port("443").port.value(), 443);
    EXPECT_EQ(parse_port("9090").port.value(), 9090);
}

TEST(ParsePortTest, NonNumericReturnsNullopt) {
    auto r = parse_port("abc");
    EXPECT_FALSE(r.port.has_value());
    EXPECT_STREQ(r.error, "not a valid integer");
}

TEST(ParsePortTest, PartialNumericReturnsNullopt) {
    auto r = parse_port("80abc");
    EXPECT_FALSE(r.port.has_value());
    EXPECT_STREQ(r.error, "not a valid integer");
}

TEST(ParsePortTest, ZeroIsOutOfRange) {
    auto r = parse_port("0");
    EXPECT_FALSE(r.port.has_value());
    EXPECT_STREQ(r.error, "out of range [1,65535]");
}

TEST(ParsePortTest, OutOfRangeValues) {
    EXPECT_FALSE(parse_port("65536").port.has_value());
    EXPECT_FALSE(parse_port("99999").port.has_value());
    EXPECT_STREQ(parse_port("99999").error, "out of range [1,65535]");
}

TEST(ParsePortTest, NullptrReturnsNullopt) {
    auto r = parse_port(nullptr);
    EXPECT_FALSE(r.port.has_value());
    EXPECT_STREQ(r.error, "empty");
}

TEST(ParsePortTest, EmptyStringReturnsNullopt) {
    auto r = parse_port("");
    EXPECT_FALSE(r.port.has_value());
    EXPECT_STREQ(r.error, "empty");
}

}  // namespace projectagamemnon::test
