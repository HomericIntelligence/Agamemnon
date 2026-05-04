#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/peer_discovery.hpp"
#include "projectagamemnon/port_parse.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"
#include "projectagamemnon/version.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <cstdlib>
#include <iostream>
#include <string>

#include "httplib.h"

int main() {
  // Disable stdout buffering for container logging
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);

  std::cout << projectagamemnon::kProjectName << " v" << projectagamemnon::kVersion
            << " starting...\n";

  // ── In-memory store ──────────────────────────────────────────────────────
  projectagamemnon::Store store;

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
      std::cout << "[agamemnon] no Tailscale NATS peer found, falling back to " << nats_url
                << "\n";
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
  httplib::Server server;
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
