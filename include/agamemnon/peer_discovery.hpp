#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace agamemnon {

struct PeerCandidate {
  std::string tailscale_ip;
  std::string hostname;
};

// Returns Tailscale peers from `tailscale status --json` output.
// Pass raw JSON via `status_json` to inject test data; pass empty string to
// actually invoke tailscale.
std::vector<PeerCandidate> enumerate_tailscale_peers(const std::string& status_json = "");

// Probes ip:monitor_port/varz via TCP, then verifies ip:nats_port is open.
// Returns true only when NATS is confirmed listening.
bool probe_nats_peer(const std::string& ip, int nats_port = 4222, int monitor_port = 8222,
                     int timeout_ms = 500);

// Returns the first live "nats://<ip>:4222" URL discovered via Tailscale,
// or an empty string if no live peer is found.
// If hostname_pattern is provided, filters peers by matching hostname.
std::string discover_nats_url(const std::string& hostname_pattern = "");

// Parallel probe knobs introduced by issue #167.
// Pre-existing knobs (hostname_pattern, nats_port, monitor_port) remain
// separate arguments on the overload below — they are not concurrency config.
struct DiscoveryOptions {
  std::chrono::milliseconds total_budget{2000};  // wall-clock cap across ALL probes
  int per_probe_timeout_ms = 500;  // SO_*TIMEO ceiling; clamped to remaining budget at call site
  std::size_t max_workers = 8;     // hard cap on threads spawned; clamped to [1, 32]
};

// Parallel-probe variant. Pre-enumerated `peers` lets tests bypass the
// tailscale CLI. Returns "nats://ip:port" of the first peer to respond
// within `opts.total_budget`, or "" if none responds in time.
// Worst-case wall clock: opts.total_budget + ~50 ms kernel timer jitter.
std::string discover_nats_url(const std::vector<PeerCandidate>& peers,
                              const std::string& hostname_pattern, int nats_port, int monitor_port,
                              const DiscoveryOptions& opts);

// Same as discover_nats_url(hostname_pattern), but uses `status_json` as the
// tailscale peer source instead of invoking `tailscale status`. Intended for
// integration tests that need to point discovery at a fixed IP (e.g. 127.0.0.1)
// without depending on a live tailscale daemon. status_json must be the same
// JSON shape that enumerate_tailscale_peers() accepts.
std::string discover_nats_url(const std::string& hostname_pattern, const std::string& status_json);

}  // namespace agamemnon
