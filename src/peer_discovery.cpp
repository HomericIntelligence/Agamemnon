#include "projectagamemnon/peer_discovery.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace projectagamemnon {

namespace {

bool is_tailscale_ip(const std::string& ip) {
  // Tailscale CGNAT range: 100.64.0.0/10
  // All Tailscale IPs start with "100."
  if (ip.rfind("100.", 0) != 0) return false;
  // Parse the second octet and ensure it's in [64, 127]
  const std::string rest = ip.substr(4);
  const std::size_t dot = rest.find('.');
  if (dot == std::string::npos) return false;
  int second = 0;
  try {
    second = std::stoi(rest.substr(0, dot));
  } catch (...) {
    return false;
  }
  return second >= 64 && second <= 127;
}

std::string run_tailscale_status() {
  std::FILE* pipe = popen("tailscale status --json 2>/dev/null", "r");
  if (!pipe) return "";
  std::string result;
  std::array<char, 4096> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    result += buf.data();
  }
  pclose(pipe);
  return result;
}

bool tcp_connect_with_timeout(const std::string& ip, int port, int timeout_ms) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;

  struct timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(sock);
    return false;
  }

  bool ok =
      (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
  close(sock);
  return ok;
}

bool check_nats_monitoring(const std::string& ip, int monitor_port, int timeout_ms) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;

  struct timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(monitor_port));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(sock);
    return false;
  }

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(sock);
    return false;
  }

  const std::string req = "GET /varz HTTP/1.0\r\nHost: " + ip + "\r\n\r\n";
  if (send(sock, req.c_str(), req.size(), 0) < 0) {
    close(sock);
    return false;
  }

  std::array<char, 256> buf{};
  ssize_t n = recv(sock, buf.data(), buf.size() - 1, 0);
  close(sock);
  if (n <= 0) return false;

  buf[n] = '\0';  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  return std::strncmp(buf.data(), "HTTP/1", 6) == 0;
}

}  // namespace

std::vector<PeerCandidate> enumerate_tailscale_peers(const std::string& status_json) {
  const std::string json_str = status_json.empty() ? run_tailscale_status() : status_json;
  if (json_str.empty()) return {};

  std::vector<PeerCandidate> candidates;
  try {
    auto root = nlohmann::json::parse(json_str);

    // Extract self IP to filter it out.
    std::string self_ip;
    if (root.contains("Self") && root["Self"].is_object()) {
      auto& self = root["Self"];
      if (self.contains("TailscaleIPs") && self["TailscaleIPs"].is_array()) {
        for (auto& ip_val : self["TailscaleIPs"]) {
          std::string ip = ip_val.get<std::string>();
          if (is_tailscale_ip(ip)) {
            self_ip = ip;
            break;
          }
        }
      }
    }

    if (!root.contains("Peer") || !root["Peer"].is_object()) return candidates;

    for (auto& [key, peer] : root["Peer"].items()) {
      if (!peer.is_object()) continue;
      if (!peer.contains("TailscaleIPs") || !peer["TailscaleIPs"].is_array()) continue;

      std::string hostname;
      if (peer.contains("HostName")) hostname = peer["HostName"].get<std::string>();

      for (auto& ip_val : peer["TailscaleIPs"]) {
        std::string ip = ip_val.get<std::string>();
        if (!is_tailscale_ip(ip)) continue;
        if (!self_ip.empty() && ip == self_ip) continue;
        candidates.push_back({ip, hostname});
        break;  // one IP per peer is enough
      }
    }
  } catch (...) {
    // Malformed JSON — return what we have (empty).
  }
  return candidates;
}

bool probe_nats_peer(const std::string& ip, int nats_port, int monitor_port,
                     int timeout_ms) {
  // First check the monitoring endpoint — faster failure than a full NATS handshake.
  if (!check_nats_monitoring(ip, monitor_port, timeout_ms)) return false;
  return tcp_connect_with_timeout(ip, nats_port, timeout_ms);
}

std::string discover_nats_url() {
  auto peers = enumerate_tailscale_peers();
  for (const auto& peer : peers) {
    if (probe_nats_peer(peer.tailscale_ip)) {
      return "nats://" + peer.tailscale_ip + ":4222";
    }
  }
  return "";
}

}  // namespace projectagamemnon
