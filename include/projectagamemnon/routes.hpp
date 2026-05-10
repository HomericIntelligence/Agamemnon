#pragma once

// Forward declarations to avoid pulling in heavy headers here.
namespace httplib {
class Server;
}

namespace projectagamemnon {

class Store;
class NatsPublisher;
class RateLimiter;
class AuthMiddleware;
class MetricsRegistry;

/// Register all /v1/ route handlers on the given server.
/// Store, NatsPublisher, RateLimiter, AuthMiddleware, and MetricsRegistry are
/// passed by reference; they must outlive the server (they are owned by main).
/// In production, pass a NatsClient (which derives from NatsPublisher).
/// In tests, pass a FakeNatsPublisher for call recording.
void register_routes(httplib::Server& server, Store& store, NatsPublisher& nats,
                     RateLimiter& rate_limiter, AuthMiddleware& auth, MetricsRegistry& metrics);

}  // namespace projectagamemnon
