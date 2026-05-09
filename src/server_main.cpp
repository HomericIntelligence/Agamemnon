#include "projectagamemnon/auth.hpp"
#include "projectagamemnon/metrics.hpp"
#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/peer_discovery.hpp"
#include "projectagamemnon/port_parse.hpp"
#include "projectagamemnon/orchestrator.hpp"
#include "projectagamemnon/rate_limiter.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"
#include "projectagamemnon/version.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "httplib.h"

// File-scope pointers used by the signal trampoline.
// Set before sigaction(), nulled after cleanup to guard against late signals.
namespace {
std::atomic<bool>* g_shutdown_flag =
    nullptr;                          // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
httplib::Server* g_server = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void shutdown_handler(int /*sig*/) {
  if (g_shutdown_flag) {
    g_shutdown_flag->store(true, std::memory_order_relaxed);
  }
  if (g_server) {
    g_server->stop();
  }
}
}  // namespace

int main() {
  // Disable stdout buffering for container logging
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);

  std::cout << projectagamemnon::kProjectName << " v" << projectagamemnon::kVersion
            << " starting...\n";

  // ── Metrics registry ─────────────────────────────────────────────────────
  projectagamemnon::MetricsRegistry metrics;

  // ── GitHub-backed store ──────────────────────────────────────────────────
  std::shared_ptr<projectagamemnon::IGitHubClient> gh_client;

  const char* gh_token = std::getenv("GITHUB_TOKEN");
  const char* gh_repo_env = std::getenv("GITHUB_REPO");
  std::string gh_repo = gh_repo_env ? gh_repo_env : "HomericIntelligence/ProjectAgamemnon";

  if (gh_token && gh_token[0] != '\0') {
    std::cout << "[agamemnon] GitHub persistence enabled (repo: " << gh_repo << ")\n";
    gh_client = std::make_shared<projectagamemnon::CurlGitHubClient>(gh_repo, gh_token);
  } else {
    std::cerr << "[agamemnon] WARNING: GITHUB_TOKEN not set — running in pure in-memory mode (no "
                 "persistence)\n";
  }

  projectagamemnon::Store store(gh_client);
  store.set_metrics(&metrics);

  // ── NATS client ──────────────────────────────────────────────────────────
  const char* nats_url_env = std::getenv("NATS_URL");
  std::string nats_url;
  if (nats_url_env) {
    nats_url = nats_url_env;
  } else {
    std::cout << "[agamemnon] NATS_URL not set — attempting Tailscale peer discovery\n";
    nats_url = projectagamemnon::discover_nats_url();
    if (nats_url.empty()) {
      nats_url = "nats://localhost:4222";
      std::cout << "[agamemnon] no Tailscale NATS peer found, falling back to " << nats_url << "\n";
    } else {
      std::cout << "[agamemnon] discovered NATS peer: " << nats_url << "\n";
    }
  }

  projectagamemnon::NatsClient nats(nats_url);
  nats.set_metrics(&metrics);

  // ── HMAS Orchestrator ────────────────────────────────────────────────────
  projectagamemnon::Orchestrator orchestrator(store, nats);

  if (nats.connect()) {
    std::cout << "[agamemnon] connected to NATS at " << nats_url << "\n";
    nats.ensure_streams();

    // Subscribe to task-completion events published by myrmidons.
    // Myrmidons publish to hi.tasks.{team_id}.{task_id}.completed
    nats.subscribe("hi.tasks.*.*.completed",
                   [&orchestrator](const std::string& subject, const std::string& data) {
                     orchestrator.on_myrmidon_completion(subject, data);
                   });
  } else {
    std::cerr << "[agamemnon] WARNING: running without NATS — events will be skipped\n";
  }

  // ── Rate limiter ──────────────────────────────────────────────────────────
  const char* rps_env = std::getenv("RATE_LIMIT_RPS");
  const char* burst_env = std::getenv("RATE_LIMIT_BURST");
  double rate_limit_rps = rps_env ? std::stod(rps_env) : 60.0;
  double rate_limit_burst = burst_env ? std::stod(burst_env) : 120.0;
  projectagamemnon::RateLimiter rate_limiter(rate_limit_rps, rate_limit_burst);
  std::cout << "[agamemnon] rate limiting: " << rate_limit_rps << " req/s, burst "
            << rate_limit_burst << "\n";

  // ── API key (fail-secure: refuse to start if unset) ──────────────────────
  const char* api_key_env = std::getenv("AGAMEMNON_API_KEY");
  if (!api_key_env || std::string(api_key_env).empty()) {
    std::cerr << "[agamemnon] FATAL: AGAMEMNON_API_KEY is not set. Refusing to start.\n";
    return 1;
  }
  projectagamemnon::AuthMiddleware auth(api_key_env);

  // ── HTTP server ───────────────────────────────────────────────────────────
  auto env_int = [](const char* name, int def) -> int {
    const char* v = std::getenv(name);
    return v ? std::stoi(v) : def;
  };

  httplib::Server server;
  server.new_task_queue = [&env_int]() {
    return new httplib::ThreadPool(env_int("SERVER_THREAD_COUNT", 8));
  };
  server.set_read_timeout(env_int("SERVER_READ_TIMEOUT_SEC", 10));
  server.set_write_timeout(env_int("SERVER_WRITE_TIMEOUT_SEC", 10));
  server.set_payload_max_length(static_cast<size_t>(env_int("SERVER_REQUEST_SIZE_LIMIT_MB", 4)) *
                                1024UL * 1024UL);

  projectagamemnon::register_routes(server, store, nats, rate_limiter, auth, metrics, orchestrator);

  const char* port_env = std::getenv("PORT");
  int port = 8080;
  if (port_env) {
    auto result = projectagamemnon::parse_port(port_env);
    if (!result.port.has_value()) {
      std::cerr << "[agamemnon] WARNING: PORT=\"" << port_env << "\" is invalid (" << result.error
                << "), defaulting to " << port << "\n";
    } else {
      port = result.port.value();
    }
  }

  // ── Signal handling ───────────────────────────────────────────────────────
  std::atomic<bool> shutdown_requested{false};
  g_shutdown_flag = &shutdown_requested;
  g_server = &server;

  struct sigaction sa {};
  sa.sa_handler = shutdown_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  std::cout << "[agamemnon] listening on 0.0.0.0:" << port << "\n";
  server.listen("0.0.0.0", port);  // blocks until server.stop() is called

  // Null the static pointers before any further work so late signals are no-ops.
  g_server = nullptr;
  g_shutdown_flag = nullptr;

  if (shutdown_requested.load()) {
    std::cout << "[agamemnon] shutdown signal received — draining complete\n";
  }

  nats.close();
  return 0;
}
