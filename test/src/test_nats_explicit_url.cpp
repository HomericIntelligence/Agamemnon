#include "agamemnon/nats_client.hpp"

#include <nats.h>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// Link this file only into the standalone explicit-URL regression target, with
// the dynamic nats.c library. These executable symbols stop every connection at
// the API boundary, before any socket can contact a default or configured broker.
// Other nats.c functions (including options allocation/destruction) remain real.
namespace {
struct ConnectionTrace {
  std::vector<std::string> calls;
  std::vector<std::string> servers;
  std::vector<std::string> servers_at_connect;
  natsOptions* configured_options = nullptr;
  natsOptions* connected_options = nullptr;
  natsStatus configure_status = NATS_OK;
};

ConnectionTrace trace;
}  // namespace

extern "C" natsStatus natsOptions_SetServers(natsOptions* options, const char** servers,
                                             int count) {
  trace.calls.emplace_back("configure");
  trace.configured_options = options;
  trace.servers.clear();
  for (int index = 0; index < count; ++index) {
    trace.servers.emplace_back(servers[index]);
  }
  return trace.configure_status;
}

extern "C" natsStatus natsConnection_Connect(natsConnection** connection, natsOptions* options) {
  trace.calls.emplace_back("connect");
  trace.connected_options = options;
  trace.servers_at_connect = trace.servers;
  *connection = nullptr;
  return NATS_NO_SERVER;
}

extern "C" natsStatus natsConnection_ConnectTo(natsConnection** connection, const char*) {
  trace.calls.emplace_back("connect-to");
  *connection = nullptr;
  return NATS_NO_SERVER;
}

namespace agamemnon::test {
class NatsExplicitUrlTest : public ::testing::Test {
 protected:
  void SetUp() override { trace = {}; }
};

TEST_F(NatsExplicitUrlTest, FirstConnectionUsesOnlyTheExplicitEndpointWithoutFallback) {
  const std::string endpoint = "nats://127.0.0.1:55432";
  NatsClient client(endpoint);

  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.is_connected());
  EXPECT_EQ(trace.calls, (std::vector<std::string>{"configure", "connect"}));
  EXPECT_EQ(trace.servers_at_connect, (std::vector<std::string>{endpoint}));
  EXPECT_NE(trace.configured_options, nullptr);
  EXPECT_EQ(trace.connected_options, trace.configured_options);
}

TEST_F(NatsExplicitUrlTest, InvalidExplicitEndpointConfigurationCannotAttemptConnection) {
  trace.configure_status = NATS_INVALID_ARG;
  NatsClient client("nats://127.0.0.1:55432");

  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.is_connected());
  EXPECT_EQ(trace.calls, (std::vector<std::string>{"configure"}));
  EXPECT_EQ(trace.connected_options, nullptr);
}
}  // namespace agamemnon::test
