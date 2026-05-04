#include "projectagamemnon/store.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <sstream>

namespace projectagamemnon {

// ── Helpers ──────────────────────────────────────────────────────────────────

std::string generate_uuid() {
  // Produces a random UUID v4 string: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
  static thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<unsigned int> dist(0, 15);
  std::uniform_int_distribution<unsigned int> dist8(8, 11);  // variant bits (10xx)

  auto hex = [](unsigned int v) -> char {
    return static_cast<char>(v < 10 ? '0' + v : 'a' + v - 10);
  };

  std::ostringstream ss;
  for (int i = 0; i < 8; ++i) ss << hex(dist(rng));
  ss << '-';
  for (int i = 0; i < 4; ++i) ss << hex(dist(rng));
  ss << "-4";
  for (int i = 0; i < 3; ++i) ss << hex(dist(rng));
  ss << '-';
  ss << hex(dist8(rng));
  for (int i = 0; i < 3; ++i) ss << hex(dist(rng));
  ss << '-';
  for (int i = 0; i < 12; ++i) ss << hex(dist(rng));
  return ss.str();
}

std::string now_iso8601() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
#ifdef _WIN32
  gmtime_s(&tm_buf, &t);
#else
  gmtime_r(&t, &tm_buf);
#endif
  std::ostringstream ss;
  ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

// ── Agents ────────────────────────────────────────────────────────────────────

json Store::create_agent(const json& body) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json agent;
  agent["id"] = id;
  agent["name"] = body.value("name", "unnamed");
  agent["label"] = body.value("label", "");
  agent["program"] = body.value("program", "");
  agent["workingDirectory"] = body.value("workingDirectory", "");
  agent["programArgs"] = body.value("programArgs", json::array());
  agent["taskDescription"] = body.value("taskDescription", "");
  agent["tags"] = body.value("tags", json::array());
  agent["owner"] = body.value("owner", "");
  agent["role"] = body.value("role", "worker");
  agent["host"] = body.value("host", "local");
  agent["status"] = "offline";
  agent["createdAt"] = now_iso8601();
  agents_[id] = agent;
  return {{"id", id}, {"agent", agent}};
}

json Store::get_agent(const std::string& id) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  return it->second;
}

json Store::get_agent_by_name(const std::string& name) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  for (auto& [id, agent] : agents_) {
    if (agent.value("name", "") == name) return agent;
  }
  return nullptr;
}

json Store::list_agents() {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  json arr = json::array();
  for (auto& [id, agent] : agents_) arr.push_back(agent);
  return {{"agents", arr}};
}

json Store::update_agent(const std::string& id, const json& fields) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  for (auto& [key, val] : fields.items()) {
    if (key != "id" && key != "createdAt") it->second[key] = val;
  }
  return it->second;
}

bool Store::delete_agent(const std::string& id) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  return agents_.erase(id) > 0;
}

json Store::start_agent(const std::string& id) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  it->second["status"] = "online";
  return {{"status", "online"}, {"id", id}};
}

json Store::stop_agent(const std::string& id) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  it->second["status"] = "offline";
  return {{"status", "offline"}, {"id", id}};
}

// ── Teams ─────────────────────────────────────────────────────────────────────

json Store::create_team(const json& body) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json team;
  team["id"] = id;
  team["name"] = body.value("name", "unnamed-team");
  team["agentIds"] =
      body.contains("agent_ids") ? body["agent_ids"] : body.value("agentIds", json::array());
  team["createdAt"] = now_iso8601();
  teams_[id] = team;
  return {{"team", team}};
}

json Store::get_team(const std::string& id) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  auto it = teams_.find(id);
  if (it == teams_.end()) return nullptr;
  return it->second;
}

json Store::list_teams() {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  json arr = json::array();
  for (auto& [id, team] : teams_) arr.push_back(team);
  return {{"teams", arr}};
}

json Store::update_team(const std::string& id, const json& body) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = teams_.find(id);
  if (it == teams_.end()) return nullptr;
  if (body.contains("agentIds"))
    it->second["agentIds"] = body["agentIds"];
  else if (body.contains("agent_ids"))
    it->second["agentIds"] = body["agent_ids"];
  if (body.contains("name")) it->second["name"] = body["name"];
  return it->second;
}

bool Store::delete_team(const std::string& id) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  return teams_.erase(id) > 0;
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

json Store::create_task(const std::string& team_id, const json& body) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json task;
  task["id"] = id;
  task["teamId"] = team_id;
  task["subject"] = body.value("subject", "");
  task["description"] = body.value("description", "");
  task["assigneeAgentId"] = body.value("assigneeAgentId", "");
  task["blockedBy"] = body.value("blockedBy", json::array());
  task["type"] = body.value("type", "general");
  task["status"] = "pending";
  task["createdAt"] = now_iso8601();
  task["completedAt"] = nullptr;
  tasks_[id] = task;
  return {{"task", task}};
}

json Store::get_task(const std::string& team_id, const std::string& task_id) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return nullptr;
  if (!team_id.empty() && it->second.value("teamId", "") != team_id) return nullptr;
  return it->second;
}

json Store::update_task(const std::string& team_id, const std::string& task_id, const json& body) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return nullptr;
  if (!team_id.empty() && it->second.value("teamId", "") != team_id) return nullptr;
  for (auto& [key, val] : body.items()) {
    if (key != "id" && key != "teamId" && key != "createdAt") it->second[key] = val;
  }
  if (body.contains("status") && body["status"] == "completed" &&
      it->second.value("completedAt", json(nullptr)).is_null()) {
    it->second["completedAt"] = now_iso8601();
  }
  return it->second;
}

json Store::list_tasks_for_team(const std::string& team_id) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  json arr = json::array();
  for (auto& [id, task] : tasks_) {
    if (task.value("teamId", "") == team_id) arr.push_back(task);
  }
  return {{"tasks", arr}};
}

json Store::list_all_tasks() {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  json arr = json::array();
  for (auto& [id, task] : tasks_) arr.push_back(task);
  return {{"tasks", arr}};
}

void Store::mark_task_completed(const std::string& task_id) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = tasks_.find(task_id);
  if (it != tasks_.end()) {
    it->second["status"] = "completed";
    it->second["completedAt"] = now_iso8601();
  }
}

// ── Chaos faults ──────────────────────────────────────────────────────────────

json Store::list_faults() {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  json arr = json::array();
  for (auto& [id, fault] : faults_) arr.push_back(fault);
  return {{"faults", arr}};
}

json Store::create_fault(const std::string& type) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json fault;
  fault["id"] = id;
  fault["type"] = type;
  fault["active"] = true;
  fault["createdAt"] = now_iso8601();
  faults_[id] = fault;
  return {{"fault", fault}};
}

bool Store::remove_fault(const std::string& id) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  return faults_.erase(id) > 0;
}

}  // namespace projectagamemnon
