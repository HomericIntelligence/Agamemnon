#include "projectagamemnon/routes.hpp"

#include "projectagamemnon/auth.hpp"
#include "projectagamemnon/circuit_breaker.hpp"
#include "projectagamemnon/dead_letter_queue.hpp"
#include "projectagamemnon/hmas_types.hpp"
#include "projectagamemnon/metrics.hpp"
#include "projectagamemnon/nats_publisher.hpp"
#include "projectagamemnon/orchestrator.hpp"
#include "projectagamemnon/rate_limiter.hpp"
#include "projectagamemnon/store.hpp"
#include "projectagamemnon/version.hpp"

// cpp-httplib — single-header, no SSL needed for internal mesh traffic
#define CPPHTTPLIB_NO_EXCEPTIONS
#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace projectagamemnon {

using json = nlohmann::json;

// ── Input length limits ───────────────────────────────────────────────────────
static constexpr std::size_t kMaxNameLen = 256;
static constexpr std::size_t kMaxLabelLen = 256;
static constexpr std::size_t kMaxDescriptionLen = 4096;
static constexpr std::size_t kMaxSubjectLen = 512;
static constexpr std::size_t kMaxProgramLen = 1024;

// ── Helpers ───────────────────────────────────────────────────────────────────

static constexpr std::size_t kMaxBodyBytes = 1U * 1024U * 1024U;  // 1 MiB

// #164: strip internal-only fields from any JSON reachable in `body` before it
// is serialised to the wire. Store retains `_github_issue` for its own GitHub-
// sync paths (see src/store.cpp:303, 488, 538, …); REST consumers must never
// see it. Add additional internal field names to kInternalFields when needed.
static constexpr std::array<std::string_view, 1> kInternalFields = {
    std::string_view{"_github_issue"},
};

static void strip_internal_fields(json& node) {  // NOLINT(misc-no-recursion)
  if (node.is_object()) {
    for (auto f : kInternalFields) node.erase(std::string(f));
    for (auto it = node.begin(); it != node.end(); ++it) strip_internal_fields(it.value());
  } else if (node.is_array()) {
    for (auto& v : node) strip_internal_fields(v);
  }
}

static void reply_json(httplib::Response& res, int status, json body) {
  strip_internal_fields(body);
  res.status = status;
  res.set_header("X-API-Version", std::string(kVersion));
  res.set_content(body.dump(), "application/json");
}

static void reply_not_found(httplib::Response& res, const std::string& what) {
  reply_json(res, 404, {{"error", what + " not found"}});
}

static void reply_bad_request(httplib::Response& res, const std::string& msg) {
  reply_json(res, 400, {{"error", msg}});
}

/// Set deprecation headers on a response.
/// Sets Deprecation: true and Sunset: <sunset_date> headers.
static void set_deprecation_headers(httplib::Response& res, const std::string& sunset_date) {
  res.set_header("Deprecation", "true");
  res.set_header("Sunset", sunset_date);
}

/// Returns false and sets 400 if value exceeds max_len.
static bool check_field_length(httplib::Response& res, const std::string& field_name,
                               const std::string& value, std::size_t max_len) {
  if (value.size() > max_len) {
    reply_bad_request(
        res, "field '" + field_name + "' exceeds maximum length of " + std::to_string(max_len));
    return false;
  }
  return true;
}

/// Parse JSON body; returns false and sets 400 on parse error.
static bool parse_body(const httplib::Request& req, httplib::Response& res, json& out) {
  if (req.body.size() > kMaxBodyBytes) {
    reply_json(res, 413, {{"error", "request body too large"}});
    return false;
  }
  if (req.body.empty()) {
    out = json::object();
    return true;
  }
  try {
    out = json::parse(req.body);
    return true;
  } catch (const json::parse_error& e) {
    reply_bad_request(res, std::string("invalid JSON: ") + e.what());
    return false;
  }
}

// ── Validation allowlists ─────────────────────────────────────────────────────

static const std::unordered_set<std::string> kValidAgentStatuses = {"offline", "online", "error"};
static const std::unordered_set<std::string> kValidTaskStatuses = {
    "pending", "running", "completed", "failed", "blocked"};
static const std::unordered_set<std::string> kValidTaskTypes = {
    "general", "research", "implementation", "review", "testing"};
static const std::unordered_set<std::string> kValidChaosTypes = {"latency", "partition", "crash",
                                                                 "corruption", "throttle"};

// ── Validation helpers ────────────────────────────────────────────────────────

// Returns false and sets 400 if value is empty or all-whitespace.
static bool require_nonempty_string(httplib::Response& res, const std::string& value,
                                    const std::string& field_name) {
  bool all_space =
      std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  if (value.empty() || all_space) {
    reply_bad_request(res, "'" + field_name + "' must be a non-empty string");
    return false;
  }
  return true;
}

// Returns false and sets 400 if value is not in the allowlist.
static bool require_enum(httplib::Response& res, const std::string& value,
                         const std::string& field_name,
                         const std::unordered_set<std::string>& allowed) {
  if (allowed.find(value) == allowed.end()) {
    reply_bad_request(res, "'" + field_name + "' has invalid value '" + value + "'");
    return false;
  }
  return true;
}

// Returns false and sets 400 if body[field] is present but not a string.
static bool require_string_if_present(httplib::Response& res, const json& body,
                                      const std::string& field_name) {
  if (body.contains(field_name) && !body[field_name].is_string()) {
    reply_bad_request(res, "'" + field_name + "' must be a string");
    return false;
  }
  return true;
}

// Returns false and sets 400 if any element of a JSON array is not a string.
static bool require_string_array(httplib::Response& res, const json& arr,
                                 const std::string& field_name) {
  if (!arr.is_array()) {
    reply_bad_request(res, "'" + field_name + "' must be an array");
    return false;
  }
  for (const auto& elem : arr) {
    if (!elem.is_string()) {
      reply_bad_request(res, "'" + field_name + "' elements must be strings");
      return false;
    }
  }
  return true;
}

// ── Pagination ────────────────────────────────────────────────────────────────

namespace {
constexpr std::size_t kDefaultLimit = 100;
constexpr std::size_t kMaxLimit = 1000;

struct PaginationParams {
  std::size_t limit;
  std::size_t offset;
};

// Returns {limit, offset} parsed from query params, or sets 400 and returns nullopt on error.
std::optional<PaginationParams> parse_pagination(const httplib::Request& req,
                                                 httplib::Response& res) {
  std::size_t limit = kDefaultLimit;
  std::size_t offset = 0;
  try {
    if (req.has_param("limit")) {
      auto v = std::stoul(req.get_param_value("limit"));
      limit = std::min(v, kMaxLimit);
    }
    if (req.has_param("offset")) {
      offset = std::stoul(req.get_param_value("offset"));
    }
  } catch (const std::exception&) {
    reply_bad_request(res, "limit and offset must be non-negative integers");
    return std::nullopt;
  }
  return PaginationParams{limit, offset};
}
}  // namespace

// ── Route registration ────────────────────────────────────────────────────────

// NOTE: We capture Store* and NatsPublisher* (raw pointers, not references) to
// avoid dangling-reference UB when the lambda outlives register_routes' stack.
// All are owned by main() and outlive the server.

// cppcheck-suppress unusedFunction
// register_routes is the public entry point invoked from server_main.cpp.
void register_routes(httplib::Server& server, Store& store, NatsPublisher& nats,
                     RateLimiter& rate_limiter, AuthMiddleware& auth, MetricsRegistry& metrics,
                     Orchestrator& orchestrator) {
  Store* sp = &store;
  NatsPublisher* np = &nats;
  // Production NatsClient overrides dead_letter_queue()/circuit_breaker() to
  // return non-null pointers; FakeNatsPublisher in tests returns nullptr.
  // Guarded accesses below skip these features when not available.
  CircuitBreaker* breaker = np->circuit_breaker();
  DeadLetterQueue* dlq = np->dead_letter_queue();
  RateLimiter* rl = &rate_limiter;
  AuthMiddleware* ap = &auth;
  MetricsRegistry* mp = &metrics;
  Orchestrator* op = &orchestrator;

  // ── Prometheus metrics endpoint ────────────────────────────────────────
  server.Get("/metrics", [mp](const httplib::Request&, httplib::Response& res) {
    res.status = 200;
    res.set_content(mp->serialize(), "text/plain; version=0.0.4; charset=utf-8");
  });

  // Enforce per-IP rate limit and API key auth on every request.
  // Health endpoints are exempt from rate limiting but still require auth.
  server.set_pre_routing_handler([rl, ap](const httplib::Request& req, httplib::Response& res) {
    // Authenticate first.
    if (!ap->validate(req)) {
      res.status = 401;
      res.set_content(R"({"error":"unauthorized"})", "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    // Health and version endpoints are exempt from rate limiting
    // (operational liveness/readiness/version probes by orchestrators).
    if (req.path == "/health" || req.path == "/v1/health" || req.path == "/v1/version") {
      return httplib::Server::HandlerResponse::Unhandled;
    }
    if (!rl->allow(req.remote_addr)) {
      double retry = rl->retry_after_seconds(req.remote_addr);
      int retry_int = static_cast<int>(retry) + 1;
      res.status = 429;
      res.set_header("Retry-After", std::to_string(retry_int));
      res.set_content(R"({"error":"rate limit exceeded","retry_after_seconds":)" +
                          std::to_string(retry_int) + "}",
                      "application/json");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  // ── Global transport-layer body size limit (1 MB) ───────────────────────
  static constexpr std::size_t kMaxBodyBytes = 1U << 20U;
  server.set_payload_max_length(kMaxBodyBytes);

  // ── Health / version ────────────────────────────────────────────────────
  server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, {{"status", "ok"}, {"service", "ProjectAgamemnon"}});
  });

  server.Get("/v1/health", [breaker, dlq](const httplib::Request&, httplib::Response& res) {
    json body = {{"status", "ok"}};
    if (breaker != nullptr) {
      body["nats_circuit"] = breaker->state_label();
    }
    if (dlq != nullptr) {
      body["dlq_depth"] = dlq->size();
    }
    reply_json(res, 200, body);
  });

  // GET /v1/dead-letter — drain and return all dead-lettered messages
  // dlq is owned by main() (via NatsClient) and outlives the server.
  server.Get("/v1/dead-letter", [dlq](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    if (dlq != nullptr) {
      auto entries = dlq->drain();
      for (const auto& e : entries) {
        arr.push_back({{"subject", e.subject},
                       {"payload", e.payload},
                       {"attempts", e.attempts},
                       {"timestamp_ms", e.timestamp_ms}});
      }
    }
    reply_json(res, 200, {{"dead_letter_queue", arr}});
  });

  // DELETE /v1/dead-letter — discard all dead-lettered messages
  server.Delete("/v1/dead-letter", [dlq](const httplib::Request&, httplib::Response& res) {
    if (dlq != nullptr) {
      dlq->clear();
    }
    reply_json(res, 200, {{"cleared", true}});
  });

  server.Get("/v1/version", [](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, {{"version", std::string(kVersion)}, {"name", std::string(kProjectName)}});
  });

  // ── Agents ──────────────────────────────────────────────────────────────

  // GET /v1/agents
  server.Get("/v1/agents", [sp](const httplib::Request& req, httplib::Response& res) {
    auto p = parse_pagination(req, res);
    if (!p) return;
    reply_json(res, 200, sp->list_agents(p->limit, p->offset));
  });

  // POST /v1/agents
  //
  // NOTE: `/v1/agents/docker` was removed (issue #144) — it was a deduplicated alias of this
  // route with no docker-specific logic. Docker-hosted agents are created by posting here with
  // `{"host": "docker", "image": "..."}`; the resulting NATS subject
  // `hi.agents.docker.{name}.created` is unchanged.
  server.Post("/v1/agents", [sp, np](const httplib::Request& req, httplib::Response& res) {
    json body;
    if (!parse_body(req, res, body)) return;
    if (!require_string_if_present(res, body, "name")) return;
    if (body.contains("name") &&
        !require_nonempty_string(res, body["name"].get<std::string>(), "name"))
      return;
    if (body.contains("name") &&
        !check_field_length(res, "name", body["name"].get<std::string>(), kMaxNameLen))
      return;
    if (body.contains("label") &&
        !check_field_length(res, "label", body["label"].get<std::string>(), kMaxLabelLen))
      return;
    if (body.contains("program") &&
        !check_field_length(res, "program", body["program"].get<std::string>(), kMaxProgramLen))
      return;
    if (body.contains("taskDescription") &&
        !check_field_length(res, "taskDescription", body["taskDescription"].get<std::string>(),
                            kMaxDescriptionLen))
      return;
    if (body.contains("status") && body["status"].is_string() &&
        !require_enum(res, body["status"].get<std::string>(), "status", kValidAgentStatuses))
      return;
    json result = sp->create_agent(body);
    auto& agent = result["agent"];
    std::string host = agent.value("host", "local");
    std::string name = agent.value("name", "unknown");
    std::string agent_id = agent.value("id", "unknown");
    std::string agent_type = agent.value("type", "unknown");
    np->publish("hi.agents." + host + "." + name + ".created", result.dump());
    np->publish_log("hi.logs.agamemnon.agent_created", "info", "Agent created: " + agent_id,
                    {{"agent_id", agent_id}, {"name", name}, {"type", agent_type}, {"host", host}});
    reply_json(res, 201, result);
  });

  // POST /v1/agents/:id/start  — registered BEFORE the generic :id route
  server.Post(R"(/v1/agents/([^/]+)/start)",
              [sp, np](const httplib::Request& req, httplib::Response& res) {
                std::string id = req.matches[1];
                json result = sp->start_agent(id);
                if (result.is_null()) {
                  reply_not_found(res, "agent");
                  return;
                }
                std::string host = result.value("host", "local");
                std::string name = result.value("name", "unknown");
                np->publish("hi.agents." + host + "." + name + ".updated", result.dump());
                reply_json(res, 200, result);
              });

  // POST /v1/agents/:id/stop
  server.Post(R"(/v1/agents/([^/]+)/stop)",
              [sp, np](const httplib::Request& req, httplib::Response& res) {
                std::string id = req.matches[1];
                json result = sp->stop_agent(id);
                if (result.is_null()) {
                  reply_not_found(res, "agent");
                  return;
                }
                std::string host = result.value("host", "local");
                std::string name = result.value("name", "unknown");
                np->publish("hi.agents." + host + "." + name + ".updated", result.dump());
                reply_json(res, 200, result);
              });

  // GET /v1/agents/by-name/:name — registered BEFORE the generic :id route
  server.Get(R"(/v1/agents/by-name/([^/]+))",
             [sp](const httplib::Request& req, httplib::Response& res) {
               std::string name = req.matches[1];
               json agent = sp->get_agent_by_name(name);
               if (agent.is_null()) {
                 reply_not_found(res, "agent");
                 return;
               }
               reply_json(res, 200, {{"agent", agent}});
             });

  // GET /v1/agents/:id
  server.Get(R"(/v1/agents/([^/]+))", [sp](const httplib::Request& req, httplib::Response& res) {
    std::string id = req.matches[1];
    json agent = sp->get_agent(id);
    if (agent.is_null()) {
      reply_not_found(res, "agent");
      return;
    }
    reply_json(res, 200, {{"agent", agent}});
  });

  // PATCH /v1/agents/:id
  server.Patch(
      R"(/v1/agents/([^/]+))", [sp, np](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        json body;
        if (!parse_body(req, res, body)) return;
        if (!require_string_if_present(res, body, "name")) return;
        if (body.contains("name") &&
            !require_nonempty_string(res, body["name"].get<std::string>(), "name"))
          return;
        if (body.contains("name") &&
            !check_field_length(res, "name", body["name"].get<std::string>(), kMaxNameLen))
          return;
        if (body.contains("label") && !body["label"].is_string()) {
          reply_bad_request(res, "'label' must be a string");
          return;
        }
        if (body.contains("label") &&
            !check_field_length(res, "label", body["label"].get<std::string>(), kMaxLabelLen))
          return;
        if (body.contains("program") && !body["program"].is_string()) {
          reply_bad_request(res, "'program' must be a string");
          return;
        }
        if (body.contains("program") &&
            !check_field_length(res, "program", body["program"].get<std::string>(), kMaxProgramLen))
          return;
        if (body.contains("taskDescription") && !body["taskDescription"].is_string()) {
          reply_bad_request(res, "'taskDescription' must be a string");
          return;
        }
        if (body.contains("taskDescription") &&
            !check_field_length(res, "taskDescription", body["taskDescription"].get<std::string>(),
                                kMaxDescriptionLen))
          return;
        if (body.contains("status") && body["status"].is_string() &&
            !require_enum(res, body["status"].get<std::string>(), "status", kValidAgentStatuses))
          return;
        json result = sp->update_agent(id, body);
        if (result.is_null()) {
          reply_not_found(res, "agent");
          return;
        }
        std::string host = result.value("host", "local");
        std::string name = result.value("name", "unknown");
        np->publish("hi.agents." + host + "." + name + ".updated", result.dump());
        reply_json(res, 200, {{"agent", result}});
      });

  // DELETE /v1/agents/:id
  server.Delete(
      R"(/v1/agents/([^/]+))", [sp, np](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        json agent = sp->get_agent(id);
        if (!sp->delete_agent(id)) {
          reply_not_found(res, "agent");
          return;
        }
        std::string host = agent.is_null() ? "local" : agent.value("host", "local");
        std::string name = agent.is_null() ? "unknown" : agent.value("name", "unknown");
        np->publish("hi.agents." + host + "." + name + ".deleted", json{{"id", id}}.dump());
        reply_json(res, 200, {{"deleted", id}});
      });

  // ── Teams ────────────────────────────────────────────────────────────────

  // GET /v1/teams
  server.Get("/v1/teams", [sp](const httplib::Request& req, httplib::Response& res) {
    auto p = parse_pagination(req, res);
    if (!p) return;
    reply_json(res, 200, sp->list_teams(p->limit, p->offset));
  });

  // POST /v1/teams
  server.Post("/v1/teams", [sp, np](const httplib::Request& req, httplib::Response& res) {
    json body;
    if (!parse_body(req, res, body)) return;
    if (!require_string_if_present(res, body, "name")) return;
    if (body.contains("name") &&
        !require_nonempty_string(res, body["name"].get<std::string>(), "name"))
      return;
    if (body.contains("name") &&
        !check_field_length(res, "name", body["name"].get<std::string>(), kMaxNameLen))
      return;
    if (body.contains("agentIds") && !require_string_array(res, body["agentIds"], "agentIds"))
      return;
    if (body.contains("agent_ids") && !require_string_array(res, body["agent_ids"], "agent_ids"))
      return;
    json result = sp->create_team(body);
    np->publish("hi.agents.team.created", result.dump());
    reply_json(res, 201, result);
  });

  // GET /v1/teams/:id
  server.Get(R"(/v1/teams/([^/]+))", [sp](const httplib::Request& req, httplib::Response& res) {
    std::string id = req.matches[1];
    json team = sp->get_team(id);
    if (team.is_null()) {
      reply_not_found(res, "team");
      return;
    }
    reply_json(res, 200, {{"team", team}});
  });

  // PUT /v1/teams/:id
  server.Put(R"(/v1/teams/([^/]+))", [sp, np](const httplib::Request& req, httplib::Response& res) {
    std::string id = req.matches[1];
    json body;
    if (!parse_body(req, res, body)) return;
    if (!require_string_if_present(res, body, "name")) return;
    if (body.contains("name") &&
        !require_nonempty_string(res, body["name"].get<std::string>(), "name"))
      return;
    if (body.contains("name") &&
        !check_field_length(res, "name", body["name"].get<std::string>(), kMaxNameLen))
      return;
    if (body.contains("agentIds") && !require_string_array(res, body["agentIds"], "agentIds"))
      return;
    if (body.contains("agent_ids") && !require_string_array(res, body["agent_ids"], "agent_ids"))
      return;
    json result = sp->update_team(id, body);
    if (result.is_null()) {
      reply_not_found(res, "team");
      return;
    }
    np->publish("hi.agents.team.updated", result.dump());
    reply_json(res, 200, {{"team", result}});
  });

  // DELETE /v1/teams/:id
  server.Delete(R"(/v1/teams/([^/]+))",
                [sp, np](const httplib::Request& req, httplib::Response& res) {
                  std::string id = req.matches[1];
                  if (!sp->delete_team(id)) {
                    reply_not_found(res, "team");
                    return;
                  }
                  np->publish("hi.agents.team.deleted", json{{"id", id}}.dump());
                  reply_json(res, 200, {{"deleted", id}});
                });

  // ── Tasks ────────────────────────────────────────────────────────────────

  // GET /v1/tasks  (all tasks across all teams)
  server.Get("/v1/tasks", [sp](const httplib::Request& req, httplib::Response& res) {
    auto p = parse_pagination(req, res);
    if (!p) return;
    reply_json(res, 200, sp->list_all_tasks(p->limit, p->offset));
  });

  // GET /v1/teams/:team_id/tasks — registered BEFORE the generic :team_id route
  server.Get(R"(/v1/teams/([^/]+)/tasks)",
             [sp](const httplib::Request& req, httplib::Response& res) {
               std::string team_id = req.matches[1];
               auto p = parse_pagination(req, res);
               if (!p) return;
               reply_json(res, 200, sp->list_tasks_for_team(team_id, p->limit, p->offset));
             });

  // POST /v1/teams/:team_id/tasks
  server.Post(R"(/v1/teams/([^/]+)/tasks)", [sp, np](const httplib::Request& req,
                                                     httplib::Response& res) {
    std::string team_id = req.matches[1];
    json body;
    if (!parse_body(req, res, body)) return;
    if (!require_string_if_present(res, body, "subject")) return;
    if (body.contains("subject") &&
        !require_nonempty_string(res, body["subject"].get<std::string>(), "subject"))
      return;
    if (body.contains("subject") &&
        !check_field_length(res, "subject", body["subject"].get<std::string>(), kMaxSubjectLen))
      return;
    if (body.contains("description") && !body["description"].is_string()) {
      reply_bad_request(res, "'description' must be a string");
      return;
    }
    if (body.contains("description") &&
        !check_field_length(res, "description", body["description"].get<std::string>(),
                            kMaxDescriptionLen))
      return;
    if (body.contains("type") && body["type"].is_string() &&
        !require_enum(res, body["type"].get<std::string>(), "type", kValidTaskTypes))
      return;
    if (body.contains("blockedBy") && !require_string_array(res, body["blockedBy"], "blockedBy"))
      return;
    json result = sp->create_task(team_id, body);
    np->publish("hi.tasks.created", result.dump());

    // Dispatch to myrmidon work queue: hi.myrmidon.{type}.{task_id}
    const auto& task = result["task"];
    std::string task_type = task.value("type", "general");
    std::string task_id = task.value("id", "unknown");
    std::string myrmidon_subject = "hi.myrmidon." + task_type + "." + task_id;
    json myrmidon_payload = {{"task_id", task_id},
                             {"team_id", team_id},
                             {"subject", task.value("subject", "")},
                             {"description", task.value("description", "")},
                             {"type", task_type},
                             {"assignee", task.value("assigneeAgentId", "")}};
    np->publish(myrmidon_subject, myrmidon_payload.dump());
    np->publish_log("hi.logs.agamemnon.task_dispatched", "info", "Task dispatched: " + task_id,
                    {{"task_id", task_id},
                     {"team_id", team_id},
                     {"type", task_type},
                     {"subject", myrmidon_subject}});

    reply_json(res, 201, result);
  });

  // GET /v1/teams/:team_id/tasks/:task_id
  server.Get(R"(/v1/teams/([^/]+)/tasks/([^/]+))",
             [sp](const httplib::Request& req, httplib::Response& res) {
               std::string team_id = req.matches[1];
               std::string task_id = req.matches[2];
               json task = sp->get_task(team_id, task_id);
               if (task.is_null()) {
                 reply_not_found(res, "task");
                 return;
               }
               reply_json(res, 200, {{"task", task}});
             });

  // Shared handler for PUT and PATCH /v1/teams/:team_id/tasks/:task_id
  auto update_task_handler = [sp, np](const httplib::Request& req, httplib::Response& res) {
    std::string team_id = req.matches[1];
    std::string task_id = req.matches[2];
    json body;
    if (!parse_body(req, res, body)) return;
    if (body.contains("subject") && !body["subject"].is_string()) {
      reply_bad_request(res, "'subject' must be a string");
      return;
    }
    if (body.contains("subject") &&
        !check_field_length(res, "subject", body["subject"].get<std::string>(), kMaxSubjectLen))
      return;
    if (body.contains("description") && !body["description"].is_string()) {
      reply_bad_request(res, "'description' must be a string");
      return;
    }
    if (body.contains("description") &&
        !check_field_length(res, "description", body["description"].get<std::string>(),
                            kMaxDescriptionLen))
      return;
    if (body.contains("status") && body["status"].is_string() &&
        !require_enum(res, body["status"].get<std::string>(), "status", kValidTaskStatuses))
      return;
    if (body.contains("type") && body["type"].is_string() &&
        !require_enum(res, body["type"].get<std::string>(), "type", kValidTaskTypes))
      return;
    if (body.contains("blockedBy") && !require_string_array(res, body["blockedBy"], "blockedBy"))
      return;
    json result = sp->update_task(team_id, task_id, body);
    if (result.is_null()) {
      reply_not_found(res, "task");
      return;
    }
    std::string status = result.value("status", "");
    json wrapped = {{"task", result}};
    np->publish("hi.tasks." + team_id + "." + task_id + ".updated", wrapped.dump());
    if (status == "completed") {
      std::string task_type = result.value("type", "unknown");
      std::string assignee = result.value("assigneeAgentId", "");
      np->publish_log("hi.logs.agamemnon.task_completed", "info", "Task completed: " + task_id,
                      {{"task_id", task_id},
                       {"team_id", team_id},
                       {"type", task_type},
                       {"assignee", assignee}});
    }
    reply_json(res, 200, wrapped);
  };

  // PUT /v1/teams/:team_id/tasks/:task_id — Telemachy uses PUT for task updates
  server.Put(R"(/v1/teams/([^/]+)/tasks/([^/]+))", update_task_handler);

  // PATCH /v1/teams/:team_id/tasks/:task_id — same semantics as PUT, share the handler.
  server.Patch(R"(/v1/teams/([^/]+)/tasks/([^/]+))", update_task_handler);

  // ── Chaos ────────────────────────────────────────────────────────────────

  // GET /v1/chaos
  server.Get("/v1/chaos", [sp](const httplib::Request& req, httplib::Response& res) {
    auto p = parse_pagination(req, res);
    if (!p) return;
    reply_json(res, 200, sp->list_faults(p->limit, p->offset));
  });

  // POST /v1/chaos/:type
  server.Post(R"(/v1/chaos/([^/]+))",
              [sp, np](const httplib::Request& req, httplib::Response& res) {
                std::string type = req.matches[1];
                // Chaos accepts any non-empty type string (flexible fault injection)
                json result = sp->create_fault(type);
                np->publish("hi.agents.chaos.injected", result.dump());
                reply_json(res, 201, result);
              });

  // DELETE /v1/chaos/:id
  server.Delete(R"(/v1/chaos/([^/]+))",
                [sp, np](const httplib::Request& req, httplib::Response& res) {
                  std::string id = req.matches[1];
                  if (!sp->remove_fault(id)) {
                    reply_not_found(res, "fault");
                    return;
                  }
                  np->publish("hi.agents.chaos.removed", json{{"id", id}}.dump());
                  reply_json(res, 200, {{"deleted", id}});
                });

  // ── HMAS Orchestration ────────────────────────────────────────────────────

  // POST /v1/briefs — submit a TaskBrief for HMAS orchestration
  server.Post("/v1/briefs", [op](const httplib::Request& req, httplib::Response& res) {
    json body;
    if (!parse_body(req, res, body)) {
      return;
    }
    if (!body.contains("title") || body["title"].get<std::string>().empty()) {
      reply_bad_request(res, "title is required");
      return;
    }
    try {
      TaskBrief brief = task_brief_from_json(body);
      const std::string brief_id = op->submit(std::move(brief));
      const json plan = op->get_plan(brief_id);
      reply_json(res, 201, plan);
    } catch (const std::exception& e) {
      reply_bad_request(res, std::string("invalid brief: ") + e.what());
    }
  });

  // GET /v1/briefs/:brief_id/plan — retrieve the full task tree for a brief
  server.Get(R"(/v1/briefs/([^/]+)/plan)",
             [op](const httplib::Request& req, httplib::Response& res) {
               const std::string brief_id = req.matches[1];
               const json plan = op->get_plan(brief_id);
               // 404 only when both the brief store AND task list are empty.
               const bool no_tasks = plan["tasks"].empty();
               const bool no_brief = !plan.contains("brief") && plan["root"].is_null();
               if (no_tasks && no_brief) {
                 reply_not_found(res, "brief");
                 return;
               }
               reply_json(res, 200, plan);
             });

  // POST /v1/tasks/:task_id/escalate — escalate an in-progress task
  server.Post(R"(/v1/tasks/([^/]+)/escalate)",
              [op](const httplib::Request& req, httplib::Response& res) {
                const std::string task_id = req.matches[1];
                json body;
                if (!parse_body(req, res, body)) {
                  return;
                }
                const std::string reason = body.value("reason", "unspecified");
                if (!op->escalate(task_id, reason)) {
                  reply_not_found(res, "task");
                  return;
                }
                reply_json(res, 200, {{"task_id", task_id}, {"escalated", true}});
              });

  // POST /v1/tasks/:task_id/complete — mark an HMAS task completed
  server.Post(R"(/v1/tasks/([^/]+)/complete)",
              [op](const httplib::Request& req, httplib::Response& res) {
                const std::string task_id = req.matches[1];
                json body;
                if (!parse_body(req, res, body)) {
                  return;
                }
                const json payload = {{"task_id", task_id}};
                op->on_myrmidon_completion("v1.tasks." + task_id + ".complete", payload.dump());
                reply_json(res, 200, {{"task_id", task_id}, {"completed", true}});
              });

  // GET /v1/tasks/:task_id/state — return current HMAS task state
  server.Get(R"(/v1/tasks/([^/]+)/state)",
             [sp](const httplib::Request& req, httplib::Response& res) {
               const std::string task_id = req.matches[1];
               auto task = sp->get_hmas_task(task_id);  // std::optional<HmasTask> — value copy
               if (!task.has_value()) {
                 reply_not_found(res, "task");
                 return;
               }
               const json task_json = hmas_task_to_json(*task);
               reply_json(res, 200,
                          {{"task_id", task_id},
                           {"state", task_state_to_string(task->state)},
                           {"layer", hmas_layer_to_string(task->layer)},
                           {"task", task_json}});
             });

  std::cout << "[agamemnon] routes registered\n";
}

}  // namespace projectagamemnon
