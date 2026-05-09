#include "projectagamemnon/routes.hpp"

#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/store.hpp"
#include "projectagamemnon/version.hpp"

// cpp-httplib — single-header, no SSL needed for internal mesh traffic
#define CPPHTTPLIB_NO_EXCEPTIONS
#include <iostream>
#include <string>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace projectagamemnon {

using json = nlohmann::json;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void reply_json(httplib::Response& res, int status, const json& body) {
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

/// Parse JSON body; returns false and sets 400 on parse error.
static bool parse_body(const httplib::Request& req, httplib::Response& res, json& out) {
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

// ── Route registration ────────────────────────────────────────────────────────

// NOTE: We capture Store* and NatsClient* (raw pointers, not references) to
// avoid dangling-reference UB when the lambda outlives register_routes' stack.
// Both store and nats are owned by main() and outlive the server.

void register_routes(httplib::Server& server, Store& store, NatsClient& nats) {
  Store* sp = &store;
  NatsClient* np = &nats;

  // ── Health / version ────────────────────────────────────────────────────
  server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, {{"status", "ok"}, {"service", "ProjectAgamemnon"}});
  });

  server.Get("/v1/health", [](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, {{"status", "ok"}});
  });

  server.Get("/v1/version", [](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, {{"version", std::string(kVersion)}, {"name", std::string(kProjectName)}});
  });

  // ── Agents ──────────────────────────────────────────────────────────────

  // GET /v1/agents
  server.Get("/v1/agents", [sp](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, sp->list_agents());
  });

  // POST /v1/agents/docker — registered BEFORE generic /v1/agents POST
  server.Post("/v1/agents/docker", [sp, np](const httplib::Request& req, httplib::Response& res) {
    json body;
    if (!parse_body(req, res, body)) return;
    // Docker agents are created the same way but with hostId and image fields
    json result = sp->create_agent(body);
    auto& agent = result["agent"];
    std::string host = agent.value("host", "docker");
    std::string name = agent.value("name", "unknown");
    std::string agent_id = agent.value("id", "unknown");
    std::string agent_type = agent.value("type", "unknown");
    np->publish("hi.agents." + host + "." + name + ".created", result.dump());
    np->publish_log("hi.logs.agamemnon.agent_created", "info", "Agent created: " + agent_id,
                    {{"agent_id", agent_id}, {"name", name}, {"type", agent_type}, {"host", host}});
    reply_json(res, 201, result);
  });

  // POST /v1/agents
  server.Post("/v1/agents", [sp, np](const httplib::Request& req, httplib::Response& res) {
    json body;
    if (!parse_body(req, res, body)) return;
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
  server.Patch(R"(/v1/agents/([^/]+))",
               [sp, np](const httplib::Request& req, httplib::Response& res) {
                 std::string id = req.matches[1];
                 json body;
                 if (!parse_body(req, res, body)) return;
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
  server.Get("/v1/teams", [sp](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, sp->list_teams());
  });

  // POST /v1/teams
  server.Post("/v1/teams", [sp, np](const httplib::Request& req, httplib::Response& res) {
    json body;
    if (!parse_body(req, res, body)) return;
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
  server.Get("/v1/tasks", [sp](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, sp->list_all_tasks());
  });

  // GET /v1/teams/:team_id/tasks — registered BEFORE the generic :team_id route
  server.Get(R"(/v1/teams/([^/]+)/tasks)",
             [sp](const httplib::Request& req, httplib::Response& res) {
               std::string team_id = req.matches[1];
               reply_json(res, 200, sp->list_tasks_for_team(team_id));
             });

  // POST /v1/teams/:team_id/tasks
  server.Post(
      R"(/v1/teams/([^/]+)/tasks)", [sp, np](const httplib::Request& req, httplib::Response& res) {
        std::string team_id = req.matches[1];
        json body;
        if (!parse_body(req, res, body)) return;
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
    json result = sp->update_task(team_id, task_id, body);
    if (result.is_null()) {
      reply_not_found(res, "task");
      return;
    }
    const auto& task = result["task"].is_null() ? result : result["task"];
    std::string status = task.value("status", "");
    np->publish("hi.tasks." + team_id + "." + task_id + ".updated", result.dump());
    if (status == "completed") {
      std::string task_type = task.value("type", "unknown");
      std::string assignee = task.value("assigneeAgentId", "");
      np->publish_log("hi.logs.agamemnon.task_completed", "info", "Task completed: " + task_id,
                      {{"task_id", task_id},
                       {"team_id", team_id},
                       {"type", task_type},
                       {"assignee", assignee}});
    }
    reply_json(res, 200, {{"task", result}});
  };

  // PUT /v1/teams/:team_id/tasks/:task_id — Telemachy uses PUT for task updates
  server.Put(R"(/v1/teams/([^/]+)/tasks/([^/]+))", update_task_handler);

  // PATCH /v1/teams/:team_id/tasks/:task_id
  server.Patch(R"(/v1/teams/([^/]+)/tasks/([^/]+))", update_task_handler);

  // ── Chaos ────────────────────────────────────────────────────────────────

  // GET /v1/chaos
  server.Get("/v1/chaos", [sp](const httplib::Request&, httplib::Response& res) {
    reply_json(res, 200, sp->list_faults());
  });

  // POST /v1/chaos/:type
  server.Post(R"(/v1/chaos/([^/]+))",
              [sp, np](const httplib::Request& req, httplib::Response& res) {
                std::string type = req.matches[1];
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

  std::cout << "[agamemnon] routes registered\n";
}

}  // namespace projectagamemnon
