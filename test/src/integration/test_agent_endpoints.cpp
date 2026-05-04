#include "server_fixture.hpp"

#include <gtest/gtest.h>
#include "nlohmann/json.hpp"

namespace projectagamemnon::test {

using json = nlohmann::json;

class AgentEndpointTest : public AgamemnonServerFixture {};

TEST_F(AgentEndpointTest, CreateAgentReturns201WithIdAndName) {
  json body = {{"name", "test-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 201);

  auto data = json::parse(res->body);
  EXPECT_TRUE(data.contains("agent"));
  EXPECT_FALSE(data["agent"]["id"].get<std::string>().empty());
  EXPECT_EQ(data["agent"]["name"].get<std::string>(), "test-agent");
}

TEST_F(AgentEndpointTest, CreateAgentPublishesCreatedEvent) {
  json body = {{"name", "pub-agent"}, {"type", "worker"}, {"host", "myhost"}};
  auto res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(res->status, 201);

  EXPECT_TRUE(nats().has_subject_prefix("hi.agents.myhost.pub-agent.created"));
}

TEST_F(AgentEndpointTest, GetAgentById) {
  json body = {{"name", "get-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto create_res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);

  std::string id = json::parse(create_res->body)["agent"]["id"].get<std::string>();
  auto res = client().Get("/v1/agents/" + id);
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);

  auto data = json::parse(res->body);
  EXPECT_TRUE(data.contains("agent"));
  EXPECT_EQ(data["agent"]["id"].get<std::string>(), id);
}

TEST_F(AgentEndpointTest, GetAgentByIdNotFound) {
  auto res = client().Get("/v1/agents/nonexistent-id-xyz");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 404);
}

TEST_F(AgentEndpointTest, GetAgentByName) {
  json body = {{"name", "named-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto create_res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);

  auto res = client().Get("/v1/agents/by-name/named-agent");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["agent"]["name"].get<std::string>(), "named-agent");
}

TEST_F(AgentEndpointTest, ListAgentsContainsCreated) {
  json body = {{"name", "listed-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto create_res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string id = json::parse(create_res->body)["agent"]["id"].get<std::string>();

  auto res = client().Get("/v1/agents");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);

  // Response: {"agents": [...]}
  auto data = json::parse(res->body);
  ASSERT_TRUE(data.contains("agents")) << "Response: " << res->body;
  bool found = false;
  for (const auto& a : data["agents"]) {
    if (a.contains("id") && a["id"].get<std::string>() == id) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Created agent not found in list";
}

TEST_F(AgentEndpointTest, PatchAgentUpdatesFields) {
  json body = {{"name", "patch-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto create_res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string id = json::parse(create_res->body)["agent"]["id"].get<std::string>();

  json patch = {{"description", "updated description"}};
  auto res = client().Patch("/v1/agents/" + id, patch.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  EXPECT_TRUE(nats().has_subject_prefix("hi.agents."));
}

TEST_F(AgentEndpointTest, StartAgentSetsStatusOnline) {
  json body = {{"name", "start-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto create_res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string id = json::parse(create_res->body)["agent"]["id"].get<std::string>();

  auto res = client().Post("/v1/agents/" + id + "/start", "", "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"].get<std::string>(), "online");
}

TEST_F(AgentEndpointTest, StopAgentSetsStatusOffline) {
  json body = {{"name", "stop-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto create_res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string id = json::parse(create_res->body)["agent"]["id"].get<std::string>();

  client().Post("/v1/agents/" + id + "/start", "", "application/json");

  auto res = client().Post("/v1/agents/" + id + "/stop", "", "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(json::parse(res->body)["status"].get<std::string>(), "offline");
}

TEST_F(AgentEndpointTest, DeleteAgentReturns200ThenGetReturns404) {
  json body = {{"name", "delete-agent"}, {"type", "worker"}, {"host", "localhost"}};
  auto create_res = client().Post("/v1/agents", body.dump(), "application/json");
  ASSERT_NE(create_res, nullptr);
  ASSERT_EQ(create_res->status, 201);
  std::string id = json::parse(create_res->body)["agent"]["id"].get<std::string>();

  auto del_res = client().Delete("/v1/agents/" + id);
  ASSERT_NE(del_res, nullptr);
  EXPECT_EQ(del_res->status, 200);
  EXPECT_TRUE(nats().has_subject_prefix("hi.agents."));

  auto get_res = client().Get("/v1/agents/" + id);
  ASSERT_NE(get_res, nullptr);
  EXPECT_EQ(get_res->status, 404);
}

TEST_F(AgentEndpointTest, CreateDockerAgentReturns201) {
  json body = {{"name", "docker-agent"}, {"type", "worker"}, {"host", "docker"}, {"image", "ubuntu:22.04"}};
  auto res = client().Post("/v1/agents/docker", body.dump(), "application/json");
  ASSERT_NE(res, nullptr);
  EXPECT_EQ(res->status, 201);
  EXPECT_TRUE(nats().has_subject_prefix("hi.agents.docker.docker-agent.created"));
}

}  // namespace projectagamemnon::test
