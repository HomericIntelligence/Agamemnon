#pragma once

#include "projectagamemnon/auth.hpp"
#include "projectagamemnon/metrics.hpp"
#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/orchestrator.hpp"
#include "projectagamemnon/rate_limiter.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

#include <memory>
#include <string>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace projectagamemnon::test {

// Shared lifecycle for any GTest fixture that needs a live httplib::Server
// wired through the production register_routes() with a Store/NatsClient/etc.
//
// Knobs are protected members the subclass sets in its OWN default
// constructor — GTest constructs the test object before SetUp() runs, so
// the values are in place when SetUp() reads them. No virtual hook is used.
//
// Migrated from per-file duplicated fixtures in test_routes*.cpp (issue #174).
//
// Quirk preservation:
//   - RoutesLimitsTest originally binds BEFORE register_routes and never
//     calls wait_until_ready(). Those behaviours are opt-in via knobs so
//     the migration does not silently change initialisation order.
//   - RoutesAuthTest constructs a fresh httplib::Client per helper call.
//     That fixture overrides its own helpers and does not use client_.
class RouteTestFixture : public ::testing::Test {
 protected:
  // ── Knobs (set in subclass ctor; read in SetUp()) ─────────────────────────
  std::string api_key_{""};          // empty = auth disabled
  double rate_tokens_per_sec_{1e9};  // effectively unlimited
  double rate_burst_{1e9};
  std::string nats_url_{"nats://127.0.0.1:14222"};  // unreachable — publishes are no-ops

  // Preserves RoutesLimitsTest's bind-before-register-routes order
  // (test_routes_limits.cpp:25-30). Default false matches every other fixture.
  bool bind_before_register_routes_{false};

  // Preserves RoutesLimitsTest's no-wait_until_ready() behaviour.
  bool skip_wait_until_ready_{false};

  void SetUp() override {
    store_ = std::make_unique<Store>();
    nats_ = std::make_unique<NatsClient>(nats_url_);
    rate_limiter_ = std::make_unique<RateLimiter>(rate_tokens_per_sec_, rate_burst_);
    auth_ = std::make_unique<AuthMiddleware>(api_key_);
    metrics_ = std::make_unique<MetricsRegistry>();
    orchestrator_ = std::make_unique<Orchestrator>(*store_, *nats_);
    server_ = std::make_unique<httplib::Server>();

    if (bind_before_register_routes_) {
      port_ = server_->bind_to_any_port("127.0.0.1");
      ASSERT_GT(port_, 0) << "bind_to_any_port failed";
      register_routes(*server_, *store_, *nats_, *rate_limiter_, *auth_, *metrics_, *orchestrator_);
    } else {
      register_routes(*server_, *store_, *nats_, *rate_limiter_, *auth_, *metrics_, *orchestrator_);
      port_ = server_->bind_to_any_port("127.0.0.1");
      ASSERT_GT(port_, 0) << "bind_to_any_port failed";
    }

    server_thread_ = std::thread([this] { server_->listen_after_bind(); });

    client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
    client_->set_connection_timeout(5);
    client_->set_read_timeout(5);

    if (!skip_wait_until_ready_) {
      server_->wait_until_ready();
    }
  }

  void TearDown() override {
    // Destroy in reverse construction order to avoid dangling references
    // (Orchestrator holds Store& and NatsClient& by reference).
    client_.reset();
    if (server_) server_->stop();
    if (server_thread_.joinable()) server_thread_.join();
    server_.reset();
    orchestrator_.reset();
    metrics_.reset();
    auth_.reset();
    rate_limiter_.reset();
    nats_.reset();
    store_.reset();
  }

  // ── Bare JSON helpers (used by all non-auth fixtures) ─────────────────────
  httplib::Result Get(const std::string& path) { return client_->Get(path); }
  httplib::Result Post(const std::string& path, const nlohmann::json& body) {
    return client_->Post(path, body.dump(), "application/json");
  }
  httplib::Result Patch(const std::string& path, const nlohmann::json& body) {
    return client_->Patch(path, body.dump(), "application/json");
  }
  httplib::Result Put(const std::string& path, const nlohmann::json& body) {
    return client_->Put(path, body.dump(), "application/json");
  }
  httplib::Result Delete(const std::string& path) { return client_->Delete(path); }

  // ── State (owned; lifetime managed by SetUp/TearDown) ─────────────────────
  std::unique_ptr<Store> store_;
  std::unique_ptr<NatsClient> nats_;
  std::unique_ptr<RateLimiter> rate_limiter_;
  std::unique_ptr<AuthMiddleware> auth_;
  std::unique_ptr<MetricsRegistry> metrics_;
  std::unique_ptr<Orchestrator> orchestrator_;
  std::unique_ptr<httplib::Server> server_;
  std::unique_ptr<httplib::Client> client_;
  std::thread server_thread_;
  int port_{0};
};

}  // namespace projectagamemnon::test
