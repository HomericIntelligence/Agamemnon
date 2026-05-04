#include "projectagamemnon/nats_client.hpp"

#include <gtest/gtest.h>

namespace projectagamemnon::test {

// All tests use an unreachable server so they exercise the disconnected path
// without requiring a live NATS instance.
static constexpr const char* kUnreachable = "nats://127.0.0.1:4228";

TEST(NatsClientTest, ConstructionDoesNotConnect) {
  NatsClient client(kUnreachable);
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, ConnectReturnsFalseOnUnreachable) {
  NatsClient client(kUnreachable);
  EXPECT_FALSE(client.connect());
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, PublishReturnsFalseWhenDisconnected) {
  NatsClient client(kUnreachable);
  EXPECT_FALSE(client.publish("hi.test.subject", R"({"key":"value"})"));
}

TEST(NatsClientTest, SubscribeReturnsFalseWhenDisconnected) {
  NatsClient client(kUnreachable);
  bool called = false;
  EXPECT_FALSE(client.subscribe("hi.test.>", [&](const std::string&, const std::string&) {
    called = true;
  }));
  EXPECT_FALSE(called);
}

TEST(NatsClientTest, PublishLogDoesNotThrowWhenDisconnected) {
  NatsClient client(kUnreachable);
  EXPECT_NO_THROW(
      client.publish_log("hi.logs.agamemnon.test", "info", "test message", {{"key", "val"}}));
}

TEST(NatsClientTest, EnsureStreamsNoopWhenDisconnected) {
  NatsClient client(kUnreachable);
  EXPECT_NO_THROW(client.ensure_streams());
}

TEST(NatsClientTest, CloseOnDisconnectedClientDoesNotCrash) {
  NatsClient client(kUnreachable);
  EXPECT_NO_THROW(client.close());
  EXPECT_FALSE(client.is_connected());
}

TEST(NatsClientTest, DestructorOnDisconnectedClientDoesNotCrash) {
  EXPECT_NO_THROW({
    NatsClient client(kUnreachable);
    // destructor called at end of scope
  });
}

}  // namespace projectagamemnon::test
