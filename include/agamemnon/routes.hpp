#pragma once

#include "agamemnon/route_limits.hpp"  // RouteLimits — full type needed for the default arg

// Forward declarations to avoid pulling in heavy headers here.
namespace httplib {
class Server;
}

namespace agamemnon {

class Store;
class NatsPublisher;
class RateLimiter;
class AuthMiddleware;
class MetricsRegistry;
class Orchestrator;

/// Register all /v1/ route handlers on the given server.
/// Store, NatsPublisher, RateLimiter, AuthMiddleware, MetricsRegistry, and
/// Orchestrator are passed by reference; they must outlive the server (owned by
/// main). In production, pass a NatsClient (which derives from NatsPublisher).
/// In tests, pass a FakeNatsPublisher for call recording.
/// `limits` carries the configurable input-length / body-size caps (#275);
/// the default preserves the historical compile-time constants. The referenced
/// object must outlive the server (handlers capture it by pointer).
const RouteLimits& default_route_limits();

void register_routes(httplib::Server& server, Store& store, NatsPublisher& nats,
                     RateLimiter& rate_limiter, AuthMiddleware& auth, MetricsRegistry& metrics,
                     Orchestrator& orchestrator, const RouteLimits& limits = default_route_limits());


}  // namespace agamemnon
