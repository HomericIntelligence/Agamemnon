#pragma once

#include <chrono>
#include <string>
#include <thread>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "projectagamemnon/auth.hpp"
#include "projectagamemnon/fake_nats_publisher.hpp"
#include "projectagamemnon/rate_limiter.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace projectagamemnon::test {

/// Test fixture that starts a real httplib::Server on a free port.
/// All test classes in the integration binary share a single server instance
/// via inline static members (one-definition rule satisfied by C++17 inline).
///
/// Auth is disabled (empty API key — all requests pass).
/// Rate limiting is effectively unlimited (1e9 tokens/s, 1e9 burst).
/// NATS uses a FakeNatsPublisher to capture publish() calls for assertions.
class AgamemnonServerFixture : public ::testing::Test {
 public:
  static httplib::Client& client() { return *client_; }
  static Store& store() { return *store_; }
  static FakeNatsPublisher& nats() { return *nats_; }

 protected:
  static void SetUpTestSuite() {
    store_ = new Store();
    nats_ = new FakeNatsPublisher();
    rate_limiter_ = new RateLimiter(1e9, 1e9);  // effectively unlimited for tests
    auth_ = new AuthMiddleware("");              // empty key — all requests pass auth

    server_ = new httplib::Server();
    register_routes(*server_, *store_, *nats_, *rate_limiter_, *auth_);

    // Let the OS pick a free port.
    int bound_port = server_->bind_to_any_port("127.0.0.1");
    ASSERT_GT(bound_port, 0) << "Failed to bind server to a free port";
    port_ = bound_port;

    server_thread_ = new std::thread([] { server_->listen_after_bind(); });

    client_ = new httplib::Client("127.0.0.1", port_);
    client_->set_connection_timeout(5);
    client_->set_read_timeout(5);

    // Wait for the server to start accepting connections (up to 2 seconds).
    for (int i = 0; i < 40; ++i) {
      if (auto r = client_->Get("/health"); r && r->status == 200) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  static void TearDownTestSuite() {
    server_->stop();
    if (server_thread_->joinable()) server_thread_->join();
    delete client_;
    delete server_thread_;
    delete server_;
    delete auth_;
    delete rate_limiter_;
    delete nats_;
    delete store_;
    client_ = nullptr;
    server_thread_ = nullptr;
    server_ = nullptr;
    auth_ = nullptr;
    rate_limiter_ = nullptr;
    nats_ = nullptr;
    store_ = nullptr;
  }

  void SetUp() override { nats_->clear(); }

  // inline static: each TU that includes this header shares the same variable.
  inline static int port_ = 0;
  inline static httplib::Server* server_ = nullptr;
  inline static std::thread* server_thread_ = nullptr;
  inline static httplib::Client* client_ = nullptr;
  inline static Store* store_ = nullptr;
  inline static FakeNatsPublisher* nats_ = nullptr;
  inline static RateLimiter* rate_limiter_ = nullptr;
  inline static AuthMiddleware* auth_ = nullptr;
};

}  // namespace projectagamemnon::test
