#include "projectagamemnon/version.hpp"

#include <gtest/gtest.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <thread>

#include "httplib.h"
#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

namespace projectagamemnon::test {

TEST(VersionTest, ProjectNameIsCorrect) { EXPECT_EQ(kProjectName, "ProjectAgamemnon"); }

TEST(VersionTest, VersionIsSet) { EXPECT_FALSE(kVersion.empty()); }

// Verify the removed /v1/workflows stub returns 404 (not registered).
TEST(RoutesTest, WorkflowsEndpointRemoved) {
  Store store;
  NatsClient nats("nats://localhost:4222");  // never connected — publish is a no-op
  httplib::Server server;
  register_routes(server, store, nats);

  int port = server.bind_to_any_port("127.0.0.1");
  ASSERT_GT(port, 0);

  std::thread srv_thread([&]() { server.listen_after_bind(); });

  httplib::Client cli("127.0.0.1", port);
  auto res = cli.Get("/v1/workflows");

  server.stop();
  srv_thread.join();

  ASSERT_TRUE(res != nullptr);
  EXPECT_EQ(res->status, 404);
}

}  // namespace projectagamemnon::test
