#include "agamemnon/version.hpp"

#include <string>

#include "server_fixture.hpp"
#include <gtest/gtest.h>

namespace agamemnon::test {

// #333: runtime confirmation that reply_json() (src/routes.cpp) stamps
// X-API-Version on every endpoint category — previously verified by
// code inspection only (follow-up from #81).
class ApiVersionHeaderTest : public AgamemnonServerFixture,
                             public ::testing::WithParamInterface<const char*> {};

TEST_P(ApiVersionHeaderTest, GetResponseCarriesApiVersionHeader) {
  auto res = client().Get(GetParam());
  ASSERT_NE(res, nullptr) << "no response from " << GetParam();
  ASSERT_TRUE(res->has_header("X-API-Version")) << "missing on " << GetParam();
  EXPECT_EQ(res->get_header_value("X-API-Version"), std::string(kVersion));
}

INSTANTIATE_TEST_SUITE_P(AllEndpointCategories, ApiVersionHeaderTest,
                         ::testing::Values("/health", "/v1/health", "/v1/version", "/v1/agents",
                                           "/v1/teams", "/v1/tasks", "/v1/chaos"),
                         [](const ::testing::TestParamInfo<const char*>& info) {
                           std::string name = info.param;
                           for (auto& c : name) {
                             if (c == '/' || c == '-') {
                               c = '_';
                             }
                           }
                           return name.empty() ? std::string("root") : name.substr(1);
                         });

class ApiVersionHeaderNonGetTest : public AgamemnonServerFixture {};

TEST_F(ApiVersionHeaderNonGetTest, ChaosInject201CarriesApiVersionHeader) {
  auto res = client().Post("/v1/chaos/latency", "", "application/json");
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(res->status, 201);
  ASSERT_TRUE(res->has_header("X-API-Version"));
  EXPECT_EQ(res->get_header_value("X-API-Version"), std::string(kVersion));
}

TEST_F(ApiVersionHeaderNonGetTest, NotFound404CarriesApiVersionHeader) {
  auto res = client().Get("/v1/agents/does-not-exist-999");
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(res->status, 404);
  ASSERT_TRUE(res->has_header("X-API-Version"));
  EXPECT_EQ(res->get_header_value("X-API-Version"), std::string(kVersion));
}

}  // namespace agamemnon::test
