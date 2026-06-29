#include "projectagamemnon/peer_discovery.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace projectagamemnon::test {

namespace {

// Reserve an ephemeral port by binding to :0, reading back the kernel-chosen
// port, and closing. Race-y in principle but acceptable: nats-server is
// launched immediately afterwards and binds to the same port number; CI
// runners don't hot-cycle ephemeral ports faster than we can re-bind.
int reserve_ephemeral_port() {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return -1;
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(sock);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(sock, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
    ::close(sock);
    return -1;
  }
  int port = ntohs(addr.sin_port);
  ::close(sock);
  return port;
}

bool varz_responds(int monitor_port, int timeout_ms) {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;
  struct timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(monitor_port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(sock);
    return false;
  }
  const std::string req = "GET /varz HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  ::send(sock, req.c_str(), req.size(), 0);
  char buf[16] = {};
  ssize_t n = ::recv(sock, buf, sizeof(buf) - 1, 0);
  ::close(sock);
  return n >= 6 && std::strncmp(buf, "HTTP/1", 6) == 0;
}

class NatsServerFixture : public ::testing::Test {
 protected:
  int nats_port_ = 0;
  int monitor_port_ = 0;
  pid_t pid_ = -1;
  bool ready_ = false;

  static bool is_required() {
    const char* v = std::getenv("AGAMEMNON_PEER_DISCOVERY_INTEGRATION");
    return v != nullptr && std::string(v) == "required";
  }

  void SetUp() override {
    // Binary presence check. CI sets AGAMEMNON_PEER_DISCOVERY_INTEGRATION=required
    // so a missing nats-server is a hard failure (no silent skip in CI).
    if (std::system("command -v nats-server >/dev/null 2>&1") != 0) {
      if (is_required()) {
        FAIL() << "nats-server not on PATH but "
                  "AGAMEMNON_PEER_DISCOVERY_INTEGRATION=required";
      }
      GTEST_SKIP() << "nats-server not on PATH; install nats-server to run "
                      "peer_discovery integration tests";
    }

    // Verify the bind-address flag exists in the installed nats-server before
    // forking — guards against an unrelated CLI break in future versions.
    if (std::system("nats-server -h 2>&1 | grep -q '\\-a'") != 0) {
      FAIL() << "installed nats-server lacks the -a/--addr flag this fixture "
                "depends on (CLI surface drifted)";
    }

    nats_port_ = reserve_ephemeral_port();
    monitor_port_ = reserve_ephemeral_port();
    ASSERT_GT(nats_port_, 0);
    ASSERT_GT(monitor_port_, 0);
    ASSERT_NE(nats_port_, monitor_port_);

    pid_ = ::fork();
    ASSERT_GE(pid_, 0) << "fork() failed";
    if (pid_ == 0) {
      // Child: redirect stdio to /dev/null, then exec.
      int devnull = ::open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        if (devnull > 2) ::close(devnull);
      }
      const std::string nats_port_s = std::to_string(nats_port_);
      const std::string mon_port_s = std::to_string(monitor_port_);
      ::execlp("nats-server", "nats-server",
               "-a", "127.0.0.1",
               "-p", nats_port_s.c_str(),
               "-m", mon_port_s.c_str(),
               static_cast<char*>(nullptr));
      _exit(127);
    }

    // Parent: poll /varz until ready (≤ 5 s).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      // Bail early if the child died (e.g. exec failed).
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) > 0) {
        FAIL() << "nats-server child exited prematurely (status " << status << ")";
      }
      if (varz_responds(monitor_port_, 200)) {
        ready_ = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready_) << "nats-server did not become ready within 5 s on "
                        << "ports " << nats_port_ << "/" << monitor_port_;

    // Make the env-var configuration path of discover_nats_url see the
    // ephemeral ports and a generous timeout for slow CI runners.
    ::setenv("NATS_PORT", std::to_string(nats_port_).c_str(), 1);
    ::setenv("NATS_MONITOR_PORT", std::to_string(monitor_port_).c_str(), 1);
    ::setenv("NATS_DISCOVERY_TIMEOUT_MS", "2000", 1);
  }

  void TearDown() override {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
    ::unsetenv("NATS_PORT");
    ::unsetenv("NATS_MONITOR_PORT");
    ::unsetenv("NATS_DISCOVERY_TIMEOUT_MS");
  }

  std::string loopback_peer_json() const {
    return R"({
      "Self": {"HostName": "ci-self", "TailscaleIPs": ["100.64.0.1"]},
      "Peer": {
        "loopback": {
          "HostName": "ci-loopback",
          "TailscaleIPs": ["127.0.0.1"]
        }
      }
    })";
  }
};

}  // namespace

TEST_F(NatsServerFixture, DiscoverReturnsLoopbackUrlForInjectedPeer) {
  // Headline end-to-end path: JSON injects 127.0.0.1 as a peer, /varz answers,
  // TCP connect succeeds, discover_nats_url returns the loopback URL with the
  // ephemeral NATS port.
  const std::string url = discover_nats_url("", loopback_peer_json());
  EXPECT_EQ(url, "nats://127.0.0.1:" + std::to_string(nats_port_));
}

TEST_F(NatsServerFixture, DiscoverReturnsEmptyWhenHostnamePatternExcludesAllPeers) {
  // Even with a live server, hostname filter must short-circuit before probing.
  EXPECT_EQ(discover_nats_url("no-such-host", loopback_peer_json()), "");
}

TEST_F(NatsServerFixture, DiscoverReturnsEmptyWhenPeerIpIsNonTailscale) {
  // is_tailscale_ip() must filter 192.0.2.x even if a live server is running
  // elsewhere on the host. enumerate_tailscale_peers drops the peer; discover
  // sees an empty peer list; returns "".
  const std::string non_tailscale_json = R"({
    "Self": {"HostName": "ci-self", "TailscaleIPs": ["100.64.0.1"]},
    "Peer": {
      "wrong": {"HostName": "wrong-host", "TailscaleIPs": ["192.0.2.1"]}
    }
  })";
  EXPECT_EQ(discover_nats_url("", non_tailscale_json), "");
}

}  // namespace projectagamemnon::test
