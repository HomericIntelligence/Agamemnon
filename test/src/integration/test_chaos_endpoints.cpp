#include "nlohmann/json.hpp"
#include "server_fixture.hpp"
#include <gtest/gtest.h>

namespace agamemnon::test {

using json = nlohmann::json;

class ChaosEndpointTest : public AgamemnonServerFixture {};

TEST_F(ChaosEndpointTest, ListFaultsIsArray) {
  auto res = client().Get("/v1/chaos");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  // Response: {"faults": [...]}
  auto data = json::parse(res->body);
  ASSERT_TRUE(data.contains("faults")) << "Response: " << res->body;
  EXPECT_TRUE(data["faults"].is_array());
}

TEST_F(ChaosEndpointTest, InjectLatencyFaultReturns201) {
  auto res = client().Post("/v1/chaos/latency", "", "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 201);

  // Response: {"fault": {...}}
  auto data = json::parse(res->body);
  ASSERT_TRUE(data.contains("fault")) << "Response: " << res->body;
  const auto& fault = data["fault"];
  EXPECT_TRUE(fault.contains("id"));
  EXPECT_EQ(fault.value("type", ""), "latency");
  EXPECT_EQ(fault.value("active", false), true);

  EXPECT_TRUE(nats().has_subject("hi.agents.chaos.injected"));
}

TEST_F(ChaosEndpointTest, InjectFaultAppearsInList) {
  auto inject_res = client().Post("/v1/chaos/packet-loss", "", "application/json");
  ASSERT_NE(inject_res, nullptr);
  ASSERT_EQ(inject_res->status, 201);
  std::string fault_id = json::parse(inject_res->body)["fault"]["id"].get<std::string>();

  auto list_res = client().Get("/v1/chaos");
  ASSERT_NE(list_res, nullptr);
  EXPECT_EQ(list_res->status, 200);

  auto data = json::parse(list_res->body);
  ASSERT_TRUE(data.contains("faults"));
  const auto& faults = data["faults"];
  ASSERT_TRUE(faults.is_array());

  bool found = false;
  for (const auto& f : faults) {
    if (f.contains("id") && f["id"].get<std::string>() == fault_id) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Injected fault not found in chaos list";
}

TEST_F(ChaosEndpointTest, RemoveFaultReturns200AndPublishesNats) {
  auto inject_res = client().Post("/v1/chaos/cpu-spike", "", "application/json");
  ASSERT_NE(inject_res, nullptr);
  ASSERT_EQ(inject_res->status, 201);
  std::string fault_id = json::parse(inject_res->body)["fault"]["id"].get<std::string>();

  nats().clear();

  auto del_res = client().Delete("/v1/chaos/" + fault_id);
  ASSERT_NE(del_res, nullptr);
  EXPECT_EQ(del_res->status, 200);

  auto data = json::parse(del_res->body);
  EXPECT_EQ(data.value("deleted", ""), fault_id);
  EXPECT_TRUE(nats().has_subject("hi.agents.chaos.removed"));
}

TEST_F(ChaosEndpointTest, RemoveNonExistentFaultReturns404) {
  auto res = client().Delete("/v1/chaos/does-not-exist-999");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 404);
}

TEST_F(ChaosEndpointTest, RemoveFaultDisappearsFromList) {
  auto inject_res = client().Post("/v1/chaos/network-drop", "", "application/json");
  ASSERT_NE(inject_res, nullptr);
  ASSERT_EQ(inject_res->status, 201);
  std::string fault_id = json::parse(inject_res->body)["fault"]["id"].get<std::string>();

  auto del_res = client().Delete("/v1/chaos/" + fault_id);
  ASSERT_NE(del_res, nullptr);
  ASSERT_EQ(del_res->status, 200);

  auto list_res = client().Get("/v1/chaos");
  ASSERT_NE(list_res, nullptr);
  EXPECT_EQ(list_res->status, 200);

  auto data = json::parse(list_res->body);
  ASSERT_TRUE(data.contains("faults"));
  for (const auto& f : data["faults"]) {
    if (f.contains("id")) {
      EXPECT_NE(f["id"].get<std::string>(), fault_id) << "Deleted fault still in list";
    }
  }
}

TEST_F(ChaosEndpointTest, InjectMultipleFaultTypes) {
  for (const auto& fault_type : {"latency", "jitter", "disk-full"}) {
    auto res = client().Post(std::string("/v1/chaos/") + fault_type, "", "application/json");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->status, 201) << "Failed to inject fault type: " << fault_type;
    auto data = json::parse(res->body);
    ASSERT_TRUE(data.contains("fault")) << "Response: " << res->body;
    EXPECT_EQ(data["fault"].value("type", ""), fault_type);
  }
}

}  // namespace agamemnon::test
