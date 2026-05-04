#include <string>

#include <gtest/gtest.h>

#include "projectagamemnon/nats_client.hpp"

namespace projectagamemnon::test {

// All tests use a dead port (14222) so no live NATS server is required.
// They verify graceful degradation: every public method behaves safely
// when the client has never successfully connected.

static constexpr const char* kDeadUrl = "nats://127.0.0.1:14222";

TEST(NatsClientTest, InitiallyNotConnected) {
  NatsClient client{kDeadUrl};
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, ConnectToDeadUrlReturnsFalse) {
  NatsClient client{kDeadUrl};
  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, PublishReturnsFalseWhenNotConnected) {
  NatsClient client{kDeadUrl};
  EXPECT_FALSE(client.publish("hi.agents.test", R"({"test":1})"));
}

TEST(NatsClientTest, SubscribeReturnsFalseWhenNotConnected) {
  NatsClient client{kDeadUrl};
  bool called = false;
  EXPECT_FALSE(client.subscribe("hi.tasks.>", [&called](const std::string&, const std::string&) {
    called = true;
  }));
  EXPECT_FALSE(called);
}

TEST(NatsClientTest, CloseOnNeverConnectedClientIsNoop) {
  NatsClient client{kDeadUrl};
  EXPECT_NO_THROW(client.close());
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, CloseIsIdempotent) {
  NatsClient client{kDeadUrl};
  EXPECT_NO_THROW(client.close());
  EXPECT_NO_THROW(client.close());
}

TEST(NatsClientTest, EnsureStreamsOnDisconnectedClientIsNoop) {
  NatsClient client{kDeadUrl};
  EXPECT_NO_THROW(client.ensure_streams());
}

TEST(NatsClientTest, PublishLogOnDisconnectedClientDoesNotThrow) {
  NatsClient client{kDeadUrl};
  EXPECT_NO_THROW(
      client.publish_log("hi.logs.agamemnon.test", "info", "test message", {{"key", "value"}}));
}

TEST(NatsClientTest, DestructorDoesNotCrash) {
  // Verify that destroying a never-connected client is safe.
  EXPECT_NO_THROW({
    NatsClient client{kDeadUrl};
    (void)client;
  });
}

TEST(NatsClientTest, ConnectFailureIsNotConnected) {
  NatsClient client{kDeadUrl};
  client.connect();
  // Even after a failed connect, state must remain false and subsequent
  // publish calls must be no-ops (not UB).
  EXPECT_FALSE(client.is_connected());
  EXPECT_FALSE(client.publish("subject", "payload"));
}

}  // namespace projectagamemnon::test
