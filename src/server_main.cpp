#include "projectagamemnon/github_client.hpp"
#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/peer_discovery.hpp"
#include "projectagamemnon/port_parse.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"
#include "projectagamemnon/version.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "httplib.h"

int main() {
  // Disable stdout buffering for container logging
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);

  std::cout << projectagamemnon::kProjectName << " v" << projectagamemnon::kVersion
            << " starting...\n";

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
  if (nats.connect()) {
    std::cout << "[agamemnon] connected to NATS at " << nats_url << "\n";
    nats.ensure_streams();

    // Subscribe to task-completion events published by myrmidons.
    // Myrmidons publish to hi.tasks.{team_id}.{task_id}.completed
    nats.subscribe(
        "hi.tasks.*.*.completed", [&store](const std::string& subject, const std::string& data) {
          try {
            auto msg = nlohmann::json::parse(data);
            // Myrmidon payload uses "task_id" (snake_case)
            std::string task_id;
            if (msg.contains("data") && msg["data"].contains("task_id")) {
              task_id = msg["data"]["task_id"].get<std::string>();
            } else if (msg.contains("task_id")) {
              task_id = msg["task_id"].get<std::string>();
            }
            if (!task_id.empty()) {
              store.mark_task_completed(task_id);
              std::cout << "[agamemnon] task completed via " << subject << ": " << task_id << "\n";
            }
          } catch (...) {
            // Ignore malformed payloads.
          }
        });
  } else {
    std::cerr << "[agamemnon] WARNING: running without NATS — events will be skipped\n";
  }

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

  projectagamemnon::register_routes(server, store, nats);

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

  std::cout << "[agamemnon] listening on 0.0.0.0:" << port << "\n";
  server.listen("0.0.0.0", port);

  nats.close();
  return 0;
}
