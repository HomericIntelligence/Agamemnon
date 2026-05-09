#pragma once

#include <string>
#include <vector>

namespace projectagamemnon {

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
std::string discover_nats_url();

}  // namespace projectagamemnon
