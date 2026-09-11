#include "agamemnon/auth.hpp"
#include "agamemnon/fleet.hpp"
#include "agamemnon/metrics.hpp"
#include "agamemnon/nats_client.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/peer_discovery.hpp"
#include "agamemnon/port_parse.hpp"
#include "agamemnon/projects.hpp"
#include "agamemnon/rate_limiter.hpp"
#include "agamemnon/routes.hpp"
#include "agamemnon/store.hpp"
#include "agamemnon/version.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "httplib.h"

// File-scope pointers used by the signal trampoline.
// Set before sigaction(), nulled after cleanup to guard against late signals.
namespace {
std::atomic<bool>* g_shutdown_flag =
    nullptr;                           // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
httplib::Server* g_server = nullptr;   // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::jthread* g_reconciler = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void shutdown_handler(int /*sig*/) {
  if (g_shutdown_flag) {
    g_shutdown_flag->store(true, std::memory_order_relaxed);
  }
  if (g_reconciler) {
    g_reconciler->request_stop();
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

  std::cout << agamemnon::kProjectName << " v" << agamemnon::kVersion << " starting...\n";

  // Validate required authorization before constructing any service that can
  // contact GitHub, discover peers, or attach to a work consumer.
  const char* api_key_env = std::getenv("AGAMEMNON_API_KEY");
  if (!api_key_env || std::string(api_key_env).empty()) {
    std::cerr << "[agamemnon] FATAL: AGAMEMNON_API_KEY is not set. Refusing to start.\n";
    return 1;
  }
  agamemnon::AuthMiddleware auth(api_key_env);

  // ── Rate limiter ──────────────────────────────────────────────────────────
  const char* rps_env = std::getenv("RATE_LIMIT_RPS");
  const char* burst_env = std::getenv("RATE_LIMIT_BURST");
  double rate_limit_rps = rps_env ? std::stod(rps_env) : 60.0;
  double rate_limit_burst = burst_env ? std::stod(burst_env) : 120.0;
  agamemnon::RateLimiter rate_limiter(rate_limit_rps, rate_limit_burst);
  std::cout << "[agamemnon] rate limiting: " << rate_limit_rps << " req/s, burst "
            << rate_limit_burst << "\n";

  auto env_int = [](const char* name, int def) -> int {
    const char* v = std::getenv(name);
    return v ? std::stoi(v) : def;
  };

  auto reconcile_env = std::getenv("GITHUB_RECONCILE_INTERVAL_SEC");
  int reconcile_sec = reconcile_env ? std::stoi(reconcile_env) : 300;
  const char* port_env = std::getenv("PORT");
  int port = 8080;
  if (port_env) {
    auto result = agamemnon::parse_port(port_env);
    if (!result.port.has_value()) {
      std::cerr << "[agamemnon] WARNING: PORT=\"" << port_env << "\" is invalid (" << result.error
                << "), defaulting to " << port << "\n";
    } else {
      port = result.port.value();
    }
  }

  const int server_threads = env_int("SERVER_THREAD_COUNT", 8);
  const int read_timeout = env_int("SERVER_READ_TIMEOUT_SEC", 10);
  const int write_timeout = env_int("SERVER_WRITE_TIMEOUT_SEC", 10);
  const int request_limit_mb = env_int("SERVER_REQUEST_SIZE_LIMIT_MB", 4);

  // ── Metrics registry ─────────────────────────────────────────────────────
  agamemnon::MetricsRegistry metrics;

  // ── GitHub-backed store ──────────────────────────────────────────────────
  std::shared_ptr<agamemnon::IGitHubClient> gh_client;

  const char* gh_token = std::getenv("GITHUB_TOKEN");
  const char* gh_repo_env = std::getenv("GITHUB_REPO");
  std::string gh_repo = gh_repo_env ? gh_repo_env : "HomericIntelligence/Agamemnon";

  if (gh_token && gh_token[0] != '\0') {
    std::cout << "[agamemnon] GitHub persistence enabled (repo: " << gh_repo << ")\n";
    gh_client = std::make_shared<agamemnon::CurlGitHubClient>(gh_repo, gh_token);
  } else {
    std::cerr << "[agamemnon] WARNING: GITHUB_TOKEN not set — running in pure in-memory mode (no "
                 "persistence)\n";
  }

  agamemnon::Store store(gh_client);
  store.set_metrics(&metrics);

  // ── GitHub reconciliation (#165) ─────────────────────────────────────────
  // Heap-allocated so the signal trampoline's g_reconciler pointer never
  // holds a stack address (CodeQL cpp/stack-address-escape). Owned by main().
  auto reconciler = std::make_unique<std::jthread>();
  if (gh_client && reconcile_sec > 0) {
    *reconciler = std::jthread([&store, reconcile_sec](std::stop_token st) {
      while (!st.stop_requested()) {
        try {
          std::size_t n = store.reconcile_from_github();
          if (n) std::cout << "[agamemnon] reconcile applied " << n << " GitHub edits\n";
        } catch (const std::exception& e) {
          std::cerr << "[agamemnon] reconcile failed: " << e.what() << "\n";
        }
        for (int i = 0; i < reconcile_sec && !st.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    });
  }

  // ── NATS client ──────────────────────────────────────────────────────────
  const char* nats_url_env = std::getenv("NATS_URL");
  std::string nats_url;
  if (nats_url_env) {
    nats_url = nats_url_env;
  } else {
    std::cout << "[agamemnon] NATS_URL not set — attempting Tailscale peer discovery\n";
    nats_url = agamemnon::discover_nats_url();
    if (nats_url.empty()) {
      nats_url = "nats://localhost:4222";
      std::cout << "[agamemnon] no Tailscale NATS peer found, falling back to " << nats_url << "\n";
    } else {
      std::cout << "[agamemnon] discovered NATS peer: " << nats_url << "\n";
    }
  }

  agamemnon::NatsClient nats(nats_url);
  nats.set_metrics(&metrics);

  // ── HMAS Orchestrator ────────────────────────────────────────────────────
  agamemnon::Orchestrator orchestrator(store, nats);
  const char* resolution_key = std::getenv("AGAMEMNON_FLEET_RESOLUTION_KEY");
  // Explicit configuration enables only a derived ProjectV2 view. Failure here
  // is exposed through Fleet health and never substitutes for durable task state.
  nlohmann::json project_config = nullptr;
  if (const char* path = std::getenv("AGAMEMNON_PROJECTS_CONFIG"); path && *path) {
    try {
      std::ifstream input(path);
      if (!input) throw std::runtime_error("project configuration unreadable");
      input >> project_config;
    } catch (const std::exception&) {
      project_config = nlohmann::json::object();
    }
  }
  auto projects = std::make_shared<agamemnon::ProjectProjection>(gh_client, project_config);
  std::jthread project_reconciler;
  if (projects->health()["state"] == "pending") {
    project_reconciler = std::jthread([projects](std::stop_token stop) {
      while (!stop.stop_requested()) {
        projects->reconcile();
        for (int second = 0; second < 300 && !stop.stop_requested(); ++second)
          std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    });
  }
  auto fleet = std::make_shared<agamemnon::FleetService>(
      store, nats, &orchestrator, resolution_key ? resolution_key : "", projects);
  // GitHub-backed Fleet must not silently use Core epic delivery. Explicit
  // durable mode without GitHub is rejected before attaching a consumer.
  const char* durable_env = std::getenv("AGAMEMNON_DURABLE_EPICS");
  const bool durable_epics = gh_client || (durable_env && std::string(durable_env) == "1");
  if (durable_epics && !gh_client) {
    std::cerr << "[agamemnon] FATAL: durable epic delivery requires GitHub persistence\n";
    return 1;
  }
  std::jthread parent_reconciler;

  if (nats.connect()) {
    std::cout << "[agamemnon] connected to NATS at " << nats_url << "\n";
    nats.ensure_streams(durable_epics);

    // Worker journals retain facts until this durable-owner acknowledgment is
    // observed. Core NATS delivery alone is not a persistence acknowledgment.
    nats.subscribe(
        "hi.fleet.events.*", [fleet, &nats](const std::string& subject, const std::string& data) {
          try {
            auto fact = nlohmann::json::parse(data);
            auto result = fleet->on_worker_event(subject, fact);
            nats.publish("hi.fleet.acks." + fact.at("workerId").get<std::string>(), result.dump());
          } catch (const std::exception& error) {
            std::cerr << "[agamemnon] Fleet fact was not acknowledged: " << error.what() << "\n";
          }
        });

    // Subscribe to task state events published by myrmidons (ADR-013 §2):
    // hi.tasks.{team_id}.{task_id}.{started|completed|failed}. `started`
    // records the assignment (claim = assignment); `failed` drives Fail.
    nats.subscribe("hi.tasks.*.*.completed",
                   [&orchestrator](const std::string& subject, const std::string& data) {
                     orchestrator.on_myrmidon_completion(subject, data);
                   });
    nats.subscribe("hi.tasks.*.*.started",
                   [&orchestrator](const std::string& subject, const std::string& data) {
                     orchestrator.on_myrmidon_started(subject, data);
                   });
    nats.subscribe("hi.tasks.*.*.failed",
                   [&orchestrator](const std::string& subject, const std::string& data) {
                     orchestrator.on_myrmidon_failed(subject, data);
                   });

    if (durable_epics) {
      if (!nats.subscribe_durable(
              "homeric-pipeline", "hi.pipeline.epic.*.registered", "agamemnon-epics",
              [&orchestrator](const std::string& subject, const std::string& data) {
                orchestrator.on_epic_registered(subject, data, true);
              })) {
        std::cerr << "[agamemnon] FATAL: required durable epic attachment unavailable\n";
        nats.close();
        return 1;
      }
      parent_reconciler = std::jthread([&orchestrator](std::stop_token stop) {
        while (!stop.stop_requested()) {
          try {
            orchestrator.reconcile_parent_wakeups();
          } catch (const std::exception& error) {
            std::cerr << "[agamemnon] parent wakeup requires reconciliation: " << error.what()
                      << "\n";
          }
          for (int tick = 0; tick < 50 && !stop.stop_requested(); ++tick)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      });
    } else {
      nats.subscribe("hi.pipeline.epic.*.registered",
                     [&orchestrator](const std::string& subject, const std::string& data) {
                       orchestrator.on_epic_registered(subject, data);
                     });
    }
  } else {
    if (durable_epics) {
      std::cerr << "[agamemnon] FATAL: durable Fleet requires a connected JetStream broker\n";
      return 1;
    }
    std::cerr << "[agamemnon] WARNING: running without NATS — events will be skipped\n";
  }

  // ── HTTP server ───────────────────────────────────────────────────────────
  // Heap-allocated so g_server never holds a stack address (CodeQL
  // cpp/stack-address-escape). Owned by main(); nulled before return.
  auto server = std::make_unique<httplib::Server>();
  server->new_task_queue = [server_threads]() { return new httplib::ThreadPool(server_threads); };
  server->set_read_timeout(read_timeout);
  server->set_write_timeout(write_timeout);
  server->set_payload_max_length(static_cast<size_t>(request_limit_mb) * 1024UL * 1024UL);

  agamemnon::register_routes(*server, store, nats, rate_limiter, auth, metrics, orchestrator,
                             fleet);

  // ── Signal handling ───────────────────────────────────────────────────────
  // Heap-allocated so the signal trampoline's g_shutdown_flag pointer never
  // holds a stack address (CodeQL cpp/stack-address-escape). Owned by main().
  auto shutdown_requested = std::make_unique<std::atomic<bool>>(false);
  g_shutdown_flag = shutdown_requested.get();
  g_server = server.get();
  g_reconciler = reconciler.get();

  struct sigaction sa = {};
  sa.sa_handler = shutdown_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  const auto* bind_address = agamemnon::server_bind_address(std::getenv("AGAMEMNON_BIND_ADDRESS"));
  std::cout << "[agamemnon] listening on " << bind_address << ":" << port << "\n";
  server->listen(bind_address, port);  // blocks until server.stop() is called

  // Null the static pointers before any further work so late signals are no-ops.
  g_server = nullptr;
  g_shutdown_flag = nullptr;
  g_reconciler = nullptr;

  if (shutdown_requested->load()) {
    std::cout << "[agamemnon] shutdown signal received — draining complete\n";
  }

  parent_reconciler.request_stop();
  if (parent_reconciler.joinable()) parent_reconciler.join();
  nats.close();
  return 0;
}
