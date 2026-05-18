#include "projectagamemnon/store.hpp"

#include "projectagamemnon/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <utility>

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

// ── GitHub persistence helpers ────────────────────────────────────────────────

// Issue body format:
//   ## AgamemnonEntity
//   ```json
//   { ...entity JSON... }
//   ```
std::string Store::make_issue_body_(std::string_view entity_type, const json& entity) {
  std::ostringstream ss;
  ss << "## AgamemnonEntity: " << entity_type << "\n\n";
  ss << "```json\n";
  ss << entity.dump(2);
  ss << "\n```\n";
  return ss.str();
}

json Store::parse_issue_entity_(const json& issue) {
  if (!issue.contains("body") || !issue["body"].is_string()) return nullptr;
  const std::string& body = issue["body"].get_ref<const std::string&>();

  static const std::string open_fence = "```json\n";
  static const std::string close_fence = "\n```";

  auto start = body.find(open_fence);
  if (start == std::string::npos) return nullptr;
  start += open_fence.size();

  auto end = body.find(close_fence, start);
  if (end == std::string::npos) return nullptr;

  try {
    return json::parse(body.substr(start, end - start));
  } catch (...) {
    return nullptr;
  }
}

// ensure_*_loaded_ helpers (#161 — drop mutex during GitHub HTTP fetch)
//
// IMPORTANT: These functions MUST be called WITHOUT holding mutex_. They use
// std::call_once to guarantee exactly one network fetch across all racing
// threads, then acquire mutex_ internally to merge results. After returning,
// the caller re-acquires mutex_ for map operations. The atomic flags allow
// a cheap early-exit on the hot (already-loaded) path.
void Store::ensure_agents_loaded_() {
  // Fast-path: already loaded (atomic, no lock needed).
  if (agents_loaded_.load(std::memory_order_acquire)) return;

  if (gh_) {
    std::call_once(agents_once_, [this]() {
      std::vector<json> issues;
      try {
        issues = gh_->list_issues("agamemnon-agent");
      } catch (const std::exception& e) {
        std::cerr << "[agamemnon] hydration error (agents): " << e.what() << "\n";
        agents_loaded_.store(true, std::memory_order_release);
        return;
      }
      std::unique_lock<std::shared_mutex> lk(mutex_);
      for (auto& issue : issues) {
        json entity = parse_issue_entity_(issue);
        if (entity.is_null() || !entity.contains("id")) {
          std::cerr << "[agamemnon] skipping malformed agent issue\n";
          continue;
        }
        if (issue.contains("number"))
          entity["_github_issue"] = std::to_string(issue["number"].get<int>());
        agents_[entity["id"].get<std::string>()] = entity;
      }
      agents_loaded_.store(true, std::memory_order_release);
    });
  } else {
    agents_loaded_.store(true, std::memory_order_release);
  }
}

void Store::ensure_teams_loaded_() {
  if (teams_loaded_.load(std::memory_order_acquire)) return;

  if (gh_) {
    std::call_once(teams_once_, [this]() {
      std::vector<json> issues;
      try {
        issues = gh_->list_issues("agamemnon-team");
      } catch (const std::exception& e) {
        std::cerr << "[agamemnon] hydration error (teams): " << e.what() << "\n";
        teams_loaded_.store(true, std::memory_order_release);
        return;
      }
      std::unique_lock<std::shared_mutex> lk(mutex_);
      for (auto& issue : issues) {
        json entity = parse_issue_entity_(issue);
        if (entity.is_null() || !entity.contains("id")) {
          std::cerr << "[agamemnon] skipping malformed team issue\n";
          continue;
        }
        if (issue.contains("number"))
          entity["_github_issue"] = std::to_string(issue["number"].get<int>());
        teams_[entity["id"].get<std::string>()] = entity;
      }
      teams_loaded_.store(true, std::memory_order_release);
    });
  } else {
    teams_loaded_.store(true, std::memory_order_release);
  }
}

void Store::ensure_tasks_loaded_() {
  if (tasks_loaded_.load(std::memory_order_acquire)) return;

  if (gh_) {
    std::call_once(tasks_once_, [this]() {
      std::vector<json> issues;
      try {
        issues = gh_->list_issues("agamemnon-task");
      } catch (const std::exception& e) {
        std::cerr << "[agamemnon] hydration error (tasks): " << e.what() << "\n";
        tasks_loaded_.store(true, std::memory_order_release);
        return;
      }
      std::unique_lock<std::shared_mutex> lk(mutex_);
      for (auto& issue : issues) {
        json entity = parse_issue_entity_(issue);
        if (entity.is_null() || !entity.contains("id")) {
          std::cerr << "[agamemnon] skipping malformed task issue\n";
          continue;
        }
        if (issue.contains("number"))
          entity["_github_issue"] = std::to_string(issue["number"].get<int>());
        tasks_[entity["id"].get<std::string>()] = entity;
      }
      tasks_loaded_.store(true, std::memory_order_release);
    });
  } else {
    tasks_loaded_.store(true, std::memory_order_release);
  }
}

void Store::ensure_faults_loaded_() {
  if (faults_loaded_.load(std::memory_order_acquire)) return;

  if (gh_) {
    std::call_once(faults_once_, [this]() {
      std::vector<json> issues;
      try {
        issues = gh_->list_issues("agamemnon-fault");
      } catch (const std::exception& e) {
        std::cerr << "[agamemnon] hydration error (faults): " << e.what() << "\n";
        faults_loaded_.store(true, std::memory_order_release);
        return;
      }
      std::unique_lock<std::shared_mutex> lk(mutex_);
      for (auto& issue : issues) {
        json entity = parse_issue_entity_(issue);
        if (entity.is_null() || !entity.contains("id")) {
          std::cerr << "[agamemnon] skipping malformed fault issue\n";
          continue;
        }
        if (issue.contains("number"))
          entity["_github_issue"] = std::to_string(issue["number"].get<int>());
        faults_[entity["id"].get<std::string>()] = entity;
      }
      faults_loaded_.store(true, std::memory_order_release);
    });
  } else {
    faults_loaded_.store(true, std::memory_order_release);
  }
}

// ── Agents ────────────────────────────────────────────────────────────────────

json Store::create_agent(const json& body) {
  ensure_agents_loaded_();  // hydrate before acquiring mutex_ (#161)
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

  if (gh_) {
    std::string issue_num =
        gh_->create_issue("agent: " + agent["name"].get<std::string>(),
                          make_issue_body_("agents/" + id, agent), "agamemnon-agent");
    if (!issue_num.empty()) agent["_github_issue"] = issue_num;
  }

  agents_[id] = agent;
  if (metrics_) metrics_->adjust_agent_count(1);
  return {{"id", id}, {"agent", agent}};
}

json Store::get_agent(const std::string& id) {
  ensure_agents_loaded_();
  std::shared_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  return it->second;
}

json Store::get_agent_by_name(const std::string& name) {
  ensure_agents_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  for (auto& [id, agent] : agents_) {
    if (agent.value("name", "") == name) return agent;
  }
  return nullptr;
}

json Store::list_agents(std::size_t limit, std::size_t offset) {
  ensure_agents_loaded_();
  std::shared_lock<std::shared_mutex> lk(mutex_);
  // #340: deterministic pagination — collect into sorted vector, then slice.
  std::vector<std::pair<std::string, json>> sorted(agents_.begin(), agents_.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  json arr = json::array();
  for (std::size_t i = offset; i < sorted.size() && arr.size() < limit; ++i) {
    arr.push_back(sorted[i].second);
  }
  return {{"agents", arr}, {"total", agents_.size()}, {"limit", limit}, {"offset", offset}};
}

json Store::update_agent(const std::string& id, const json& fields) {
  // Guard against null/non-object payloads from direct (non-route) callers;
  // body.items() throws type_error.306 on a null json. See #209.
  if (!fields.is_object()) return nullptr;
  ensure_agents_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  for (auto& [key, val] : fields.items()) {
    if (key != "id" && key != "createdAt" && key != "_github_issue") it->second[key] = val;
  }
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                           make_issue_body_("agents/" + id, it->second));
  }
  return it->second;
}

bool Store::delete_agent(const std::string& id) {
  ensure_agents_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return false;
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->close_issue(it->second["_github_issue"].get<std::string>());
  }
  agents_.erase(it);
  if (metrics_) metrics_->adjust_agent_count(-1);
  return true;
}

json Store::start_agent(const std::string& id) {
  ensure_agents_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  it->second["status"] = "online";
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                           make_issue_body_("agents/" + id, it->second));
  }
  return {{"status", "online"}, {"id", id}};
}

json Store::stop_agent(const std::string& id) {
  ensure_agents_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  it->second["status"] = "offline";
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                           make_issue_body_("agents/" + id, it->second));
  }
  return {{"status", "offline"}, {"id", id}};
}

// ── Teams ─────────────────────────────────────────────────────────────────────

json Store::create_team(const json& body) {
  ensure_teams_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json team;
  team["id"] = id;
  team["name"] = body.value("name", "unnamed-team");
  team["agentIds"] =
      body.contains("agent_ids") ? body["agent_ids"] : body.value("agentIds", json::array());
  team["createdAt"] = now_iso8601();

  if (gh_) {
    std::string issue_num =
        gh_->create_issue("team: " + team["name"].get<std::string>(),
                          make_issue_body_("teams/" + id, team), "agamemnon-team");
    if (!issue_num.empty()) team["_github_issue"] = issue_num;
  }

  teams_[id] = team;
  return {{"team", team}};
}

json Store::get_team(const std::string& id) {
  ensure_teams_loaded_();
  std::shared_lock<std::shared_mutex> lk(mutex_);
  auto it = teams_.find(id);
  if (it == teams_.end()) return nullptr;
  return it->second;
}

json Store::list_teams(std::size_t limit, std::size_t offset) {
  ensure_teams_loaded_();
  std::shared_lock<std::shared_mutex> lk(mutex_);
  // #340: deterministic pagination — sort by key then slice.
  std::vector<std::pair<std::string, json>> sorted(teams_.begin(), teams_.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  json arr = json::array();
  for (std::size_t i = offset; i < sorted.size() && arr.size() < limit; ++i) {
    arr.push_back(sorted[i].second);
  }
  return {{"teams", arr}, {"total", teams_.size()}, {"limit", limit}, {"offset", offset}};
}

json Store::update_team(const std::string& id, const json& body) {
  ensure_teams_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = teams_.find(id);
  if (it == teams_.end()) return nullptr;
  if (body.contains("agentIds"))
    it->second["agentIds"] = body["agentIds"];
  else if (body.contains("agent_ids"))
    it->second["agentIds"] = body["agent_ids"];
  if (body.contains("name")) it->second["name"] = body["name"];
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                           make_issue_body_("teams/" + id, it->second));
  }
  return it->second;
}

bool Store::delete_team(const std::string& id) {
  ensure_teams_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = teams_.find(id);
  if (it == teams_.end()) return false;
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->close_issue(it->second["_github_issue"].get<std::string>());
  }
  teams_.erase(it);
  return true;
}

// ── Tasks ─────────────────────────────────────────────────────────────────────

json Store::create_task(const std::string& team_id, const json& body) {
  ensure_tasks_loaded_();
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

  if (gh_) {
    std::string subj = task["subject"].get<std::string>();
    std::string title = "task: " + (subj.empty() ? id : subj);
    std::string issue_num =
        gh_->create_issue(title, make_issue_body_("tasks/" + id, task), "agamemnon-task");
    if (!issue_num.empty()) task["_github_issue"] = issue_num;
  }

  tasks_[id] = task;
  if (metrics_) {
    metrics_->record_task_created();
  }
  return {{"task", task}};
}

json Store::get_task(const std::string& team_id, const std::string& task_id) {
  // #222: require a non-empty team_id scope; empty team_id is not a valid
  // wildcard — callers must provide the owning team for cross-team safety.
  if (team_id.empty()) return nullptr;
  ensure_tasks_loaded_();
  std::shared_lock<std::shared_mutex> lk(mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return nullptr;
  if (it->second.value("teamId", "") != team_id) return nullptr;
  return it->second;
}

json Store::update_task(const std::string& team_id, const std::string& task_id, const json& body) {
  // Guard against null/non-object payloads from direct (non-route) callers;
  // body.items() throws type_error.306 on a null json. See #209.
  if (!body.is_object()) return nullptr;
  // #222: require a non-empty team_id to prevent cross-team writes.
  if (team_id.empty()) return nullptr;
  ensure_tasks_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return nullptr;
  if (it->second.value("teamId", "") != team_id) return nullptr;
  for (auto& [key, val] : body.items()) {
    if (key != "id" && key != "teamId" && key != "createdAt" && key != "_github_issue")
      it->second[key] = val;
  }
  if (body.contains("status") && body["status"] == "completed" &&
      it->second.value("completedAt", json(nullptr)).is_null()) {
    it->second["completedAt"] = now_iso8601();
  }
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                           make_issue_body_("tasks/" + task_id, it->second));
  }
  if (metrics_ && body.contains("status")) {
    metrics_->record_task_state_change("pending", body["status"].get<std::string>());
  }
  return it->second;
}

json Store::list_tasks_for_team(const std::string& team_id, std::size_t limit, std::size_t offset) {
  ensure_tasks_loaded_();
  std::shared_lock<std::shared_mutex> lk(mutex_);
  // #340: deterministic pagination — collect team tasks, sort by key, then slice.
  std::vector<std::pair<std::string, json>> team_tasks;
  for (auto& [id, task] : tasks_) {
    if (task.value("teamId", "") == team_id) team_tasks.emplace_back(id, task);
  }
  std::sort(team_tasks.begin(), team_tasks.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  const std::size_t total = team_tasks.size();
  json arr = json::array();
  for (std::size_t i = offset; i < team_tasks.size() && arr.size() < limit; ++i) {
    arr.push_back(team_tasks[i].second);
  }
  return {{"tasks", arr}, {"total", total}, {"limit", limit}, {"offset", offset}};
}

json Store::list_all_tasks(std::size_t limit, std::size_t offset) {
  ensure_tasks_loaded_();
  std::shared_lock<std::shared_mutex> lk(mutex_);
  // #340: deterministic pagination — sort by key then slice.
  std::vector<std::pair<std::string, json>> sorted(tasks_.begin(), tasks_.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  json arr = json::array();
  for (std::size_t i = offset; i < sorted.size() && arr.size() < limit; ++i) {
    arr.push_back(sorted[i].second);
  }
  return {{"tasks", arr}, {"total", tasks_.size()}, {"limit", limit}, {"offset", offset}};
}

void Store::mark_task_completed(const std::string& task_id) {
  ensure_tasks_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = tasks_.find(task_id);
  if (it != tasks_.end()) {
    std::string old_status = it->second.value("status", "pending");
    it->second["status"] = "completed";
    it->second["completedAt"] = now_iso8601();
    if (gh_ && it->second.contains("_github_issue")) {
      gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                             make_issue_body_("tasks/" + task_id, it->second));
    }
    if (metrics_) metrics_->record_task_state_change(old_status, "completed");
  }
}

// ── Chaos faults ──────────────────────────────────────────────────────────────

json Store::list_faults(std::size_t limit, std::size_t offset) {
  ensure_faults_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  // #340: deterministic pagination — sort by key then slice.
  std::vector<std::pair<std::string, json>> sorted(faults_.begin(), faults_.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  json arr = json::array();
  for (std::size_t i = offset; i < sorted.size() && arr.size() < limit; ++i) {
    arr.push_back(sorted[i].second);
  }
  return {{"faults", arr}, {"total", faults_.size()}, {"limit", limit}, {"offset", offset}};
}

json Store::create_fault(const std::string& type) {
  ensure_faults_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json fault;
  fault["id"] = id;
  fault["type"] = type;
  fault["active"] = true;
  fault["createdAt"] = now_iso8601();

  if (gh_) {
    std::string issue_num = gh_->create_issue(
        "fault: " + type, make_issue_body_("faults/" + id, fault), "agamemnon-fault");
    if (!issue_num.empty()) fault["_github_issue"] = issue_num;
  }

  faults_[id] = fault;
  return {{"fault", fault}};
}

bool Store::remove_fault(const std::string& id) {
  ensure_faults_loaded_();
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = faults_.find(id);
  if (it == faults_.end()) return false;
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->close_issue(it->second["_github_issue"].get<std::string>());
  }
  faults_.erase(it);
  return true;
}

// ── HMAS typed tasks ──────────────────────────────────────────────────────────

void Store::create_hmas_task(const HmasTask& task) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  hmas_tasks_[task.id] = task;
}

std::optional<HmasTask> Store::get_hmas_task(const std::string& id) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(id);
  if (it == hmas_tasks_.end()) return std::nullopt;
  return it->second;  // value copy — safe to use outside the lock
}

bool Store::update_hmas_task_state_and_record_escalation(const std::string& id, TaskState new_state,
                                                         const EscalationRecord& escalation) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(id);
  if (it == hmas_tasks_.end()) return false;
  it->second.state = new_state;
  it->second.escalations.push_back(escalation);
  return true;
}

bool Store::update_hmas_task_state(const std::string& id, TaskState state) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(id);
  if (it == hmas_tasks_.end()) return false;
  it->second.state = state;
  return true;
}

bool Store::update_hmas_task(const HmasTask& task) {
  std::unique_lock<std::shared_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(task.id);
  if (it == hmas_tasks_.end()) return false;
  it->second = task;
  return true;
}

std::vector<HmasTask> Store::list_hmas_tasks_by_layer(HmasLayer layer) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  std::vector<HmasTask> out;
  for (const auto& [id, task] : hmas_tasks_) {
    if (task.layer == layer) out.push_back(task);
  }
  return out;
}

std::vector<HmasTask> Store::list_hmas_tasks_by_parent(const std::string& parent_id) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  std::vector<HmasTask> out;
  for (const auto& [id, task] : hmas_tasks_) {
    if (task.parent_task_id == parent_id) out.push_back(task);
  }
  return out;
}

std::vector<HmasTask> Store::list_hmas_tasks_by_brief(const std::string& brief_id) {
  std::shared_lock<std::shared_mutex> lk(mutex_);
  std::vector<HmasTask> out;
  for (const auto& [id, task] : hmas_tasks_) {
    if (task.brief_id == brief_id) out.push_back(task);
  }
  return out;
}

}  // namespace projectagamemnon
