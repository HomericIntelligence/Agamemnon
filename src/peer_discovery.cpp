#include "projectagamemnon/peer_discovery.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace projectagamemnon {

namespace {

bool is_tailscale_ip(const std::string& ip) {
  // Allow 127.0.0.1 for testing (integration tests that inject loopback).
  if (ip == "127.0.0.1") return true;

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
  if (!pipe) {
    // popen failed - daemon likely not running or command not found
    return "";
  }
  std::string result;
  std::array<char, 4096> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    result += buf.data();
  }
  int exit_code = pclose(pipe);
  if (exit_code != 0) {
    // tailscale command failed - daemon not running or error
    return "";
  }
  return result;
}

bool tcp_connect_with_timeout(const std::string& ip, int port, int timeout_ms) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;

  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(sock);
    return false;
  }

  bool ok = (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
  close(sock);
  return ok;
}

bool matches_hostname_pattern(const std::string& hostname, const std::string& pattern) {
  if (pattern.empty()) return true;    // no pattern = match all
  if (hostname.empty()) return false;  // empty hostname cannot match

  // If pattern starts with ^ or contains .*, treat as regex
  if (pattern.find('^') != std::string::npos || pattern.find(".*") != std::string::npos) {
    try {
      std::regex re(pattern);
      return std::regex_match(hostname, re);
    } catch (const std::regex_error&) {
      // Invalid regex — fall back to substring match
      return hostname.find(pattern) != std::string::npos;
    }
  }

  // Otherwise, simple substring match
  return hostname.find(pattern) != std::string::npos;
}

bool check_nats_monitoring(const std::string& ip, int monitor_port, int timeout_ms) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;

  struct timeval tv {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr {};
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

bool probe_nats_peer(const std::string& ip, int nats_port, int monitor_port, int timeout_ms) {
  // First check the monitoring endpoint — faster failure than a full NATS handshake.
  if (!check_nats_monitoring(ip, monitor_port, timeout_ms)) return false;
  return tcp_connect_with_timeout(ip, nats_port, timeout_ms);
}

std::string discover_nats_url(const std::vector<PeerCandidate>& peers,
                              const std::string& hostname_pattern, int nats_port, int monitor_port,
                              const DiscoveryOptions& opts) {
  if (peers.empty()) return "";

  // Filter by hostname pattern up front so worker count reflects real work.
  std::vector<PeerCandidate> matched;
  matched.reserve(peers.size());
  for (const auto& p : peers) {
    if (matches_hostname_pattern(p.hostname, hostname_pattern)) matched.push_back(p);
  }
  if (matched.empty()) return "";

  // Hard cap — no sentinel value. Operators get predictable thread counts.
  const std::size_t clamped_workers = std::clamp<std::size_t>(opts.max_workers, 1U, 32U);
  const std::size_t worker_count = std::min(matched.size(), clamped_workers);

  const auto deadline = std::chrono::steady_clock::now() + opts.total_budget;

  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<bool> found{false};
  std::string winning_url;  // guarded by mtx
  std::atomic<std::size_t> next_idx{0};

  auto remaining_ms = [&]() -> int {
    using namespace std::chrono;
    const auto left = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
  };

  auto worker = [&]() {
    while (!found.load(std::memory_order_acquire)) {
      const std::size_t i = next_idx.fetch_add(1, std::memory_order_relaxed);
      if (i >= matched.size()) return;

      // Clamp socket timeout to remaining wall-clock budget.
      // probe_nats_peer programs SO_RCVTIMEO/SO_SNDTIMEO from this value
      // (src/peer_discovery.cpp:62-65, :104-107), so the kernel cannot
      // exceed the deadline by more than its timer-wheel granularity (~tens of ms).
      const int budget_left = remaining_ms();
      if (budget_left == 0) return;
      const int probe_timeout = std::min(opts.per_probe_timeout_ms, budget_left);

      const auto& peer = matched[i];
      if (!probe_nats_peer(peer.tailscale_ip, nats_port, monitor_port, probe_timeout)) continue;

      // Memory order: CAS publishes the win before any reader can observe it.
      // Main thread reads winning_url ONLY after acquiring `mtx`, which the
      // worker releases AFTER writing. Happens-before chain:
      //   CAS(found=true, acq_rel) -> lock(mtx) -> write -> unlock
      //   -> main lock(mtx) -> read.
      bool expected = false;
      if (found.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        {
          std::lock_guard<std::mutex> lk(mtx);
          winning_url = "nats://" + peer.tailscale_ip + ":" + std::to_string(nats_port);
        }
        cv.notify_one();
      }
      return;
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::size_t w = 0; w < worker_count; ++w) workers.emplace_back(worker);

  // Wait until either a winner is published or the deadline expires.
  // No polling — cv.wait_until returns precisely at the deadline.
  {
    std::unique_lock<std::mutex> lk(mtx);
    cv.wait_until(lk, deadline, [&]() { return found.load(std::memory_order_acquire); });
  }

  // Losing workers either already saw found==true (and returned), or are
  // mid-probe with a socket timeout clamped to remaining budget — so the
  // join wait is bounded by kernel timer jitter (~tens of ms), NOT by the
  // original per-probe timeout. This is the documented overshoot.
  for (auto& t : workers) t.join();

  std::lock_guard<std::mutex> lk(mtx);
  return winning_url;  // "" if deadline beat every probe
}

namespace {

std::string discover_nats_url_impl(const std::string& hostname_pattern,
                                   const std::vector<PeerCandidate>& peers) {
  std::string pattern = hostname_pattern;
  if (pattern.empty()) {
    if (const char* env = std::getenv("NATS_PEER_HOSTNAME_PATTERN")) pattern = env;
  }

  int nats_port = 4222;
  int monitor_port = 8222;
  DiscoveryOptions opts;  // defaults: 2 s budget, 500 ms per probe, 8 workers

  if (const char* e = std::getenv("NATS_PORT")) {
    int v = std::atoi(e);
    if (v > 0 && v <= 65535) nats_port = v;
  }
  if (const char* e = std::getenv("NATS_MONITOR_PORT")) {
    int v = std::atoi(e);
    if (v > 0 && v <= 65535) monitor_port = v;
  }
  if (const char* e = std::getenv("NATS_DISCOVERY_TIMEOUT_MS")) {
    int v = std::atoi(e);
    if (v > 0) opts.per_probe_timeout_ms = v;
  }
  if (const char* e = std::getenv("NATS_DISCOVERY_BUDGET_MS")) {
    int v = std::atoi(e);
    if (v > 0) opts.total_budget = std::chrono::milliseconds{v};
  }
  if (const char* e = std::getenv("NATS_DISCOVERY_MAX_WORKERS")) {
    int v = std::atoi(e);
    if (v > 0) opts.max_workers = static_cast<std::size_t>(v);
  }

  return discover_nats_url(peers, pattern, nats_port, monitor_port, opts);
}

}  // namespace

std::string discover_nats_url(const std::string& hostname_pattern) {
  return discover_nats_url_impl(hostname_pattern, enumerate_tailscale_peers());
}

std::string discover_nats_url(const std::string& hostname_pattern,
                              const std::string& status_json) {
  return discover_nats_url_impl(hostname_pattern,
                                enumerate_tailscale_peers(status_json));
}

}  // namespace projectagamemnon
