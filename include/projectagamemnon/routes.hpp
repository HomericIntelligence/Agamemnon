#pragma once

// Forward declarations to avoid pulling in heavy headers here.
namespace httplib {
class Server;
}

namespace projectagamemnon {

class Store;
class NatsClient;
class RateLimiter;
class AuthMiddleware;
class MetricsRegistry;
class Orchestrator;

/// Register all /v1/ route handlers on the given server.
/// Store, NatsPublisher, RateLimiter, AuthMiddleware, MetricsRegistry, and
/// Orchestrator are passed by reference; they must outlive the server (owned by
/// main). In production, pass a NatsClient (which derives from NatsPublisher).
/// In tests, pass a FakeNatsPublisher for call recording.
void register_routes(httplib::Server& server, Store& store, NatsPublisher& nats,
                     RateLimiter& rate_limiter, AuthMiddleware& auth, MetricsRegistry& metrics,
                     Orchestrator& orchestrator);

}  // namespace projectagamemnon
