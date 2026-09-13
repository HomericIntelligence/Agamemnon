#include "agamemnon/store.hpp"

#include "agamemnon/fleet_issue.hpp"
#include "agamemnon/fleet_research.hpp"
#include "agamemnon/metrics.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace agamemnon {

namespace {
bool has_research_intake(const HmasTask& task) {
  return task.delivery.is_object() && task.delivery.contains("researchIntake");
}
const char* import_kind(const HmasTask& task) {
  if (!task.delivery.is_object()) return "";
  const bool research = task.delivery.contains("researchIntake");
  const bool issue = task.delivery.contains("issueIntake");
  if (research && issue) throw std::invalid_argument("Ambiguous import provenance");
  return research ? "researchIntake" : issue ? "issueIntake" : "";
}

bool research_leaf(const HmasTask& task) {
  return task.layer == HmasLayer::L3_TaskAgent && task.parent_task_id.empty() &&
         task.brief_id.empty() && task.module.empty() && task.child_task_ids.empty() &&
         task.blocked_by.empty();
}

bool same_import_identity(const HmasTask& original, const HmasTask& updated, bool replay = false) {
  const std::string kind = import_kind(original);
  if (kind.empty() || kind != import_kind(updated) || !research_leaf(original) ||
      !research_leaf(updated) || original.id != updated.id || original.repo != updated.repo ||
      original.issue != updated.issue)
    return false;
  auto before = original.delivery[kind];
  auto after = updated.delivery[kind];
  if (replay && kind == "issueIntake" && before.is_object() && after.is_object()) {
    before.erase("observedAt");
    after.erase("observedAt");
  }
  return before.dump() == after.dump();
}
std::string lower_work_name(std::string value) {
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return value;
}

void validate_import_entity(const json& raw) {
  const auto delivery = raw.value("delivery", json());
  const auto id = raw.value("id", json());
  const bool direct = delivery.is_object() && delivery.contains("issueIntake");
  const bool research = delivery.is_object() && delivery.contains("researchIntake");
  const bool typed_id =
      id.is_string() &&
      std::regex_match(id.get<std::string>(), std::regex("(issue|research)-[a-f0-9]{64}"));
  if (!direct && !research && !typed_id) return;
  if (direct == research || !id.is_string() || !raw.value("repo", json()).is_string() ||
      !raw.value("issue", json()).is_number_integer() || raw["issue"] <= 0 ||
      raw["issue"] > std::numeric_limits<int>::max() ||
      raw.value("layer", json()) != "L3_TaskAgent")
    throw std::invalid_argument("Malformed retained import identity");
  for (const auto* field : {"brief_id", "parent_task_id", "module"})
    if (!raw.contains(field) || raw[field] != "")
      throw std::invalid_argument("Malformed retained import leaf");
  for (const auto* field : {"child_task_ids", "blocked_by"})
    if (!raw.value(field, json()).is_array() || !raw[field].empty())
      throw std::invalid_argument("Malformed retained import leaf");
  if (direct)
    validate_issue_intake_provenance(delivery["issueIntake"], id.get<std::string>(),
                                     raw["repo"].get<std::string>(), raw["issue"].get<int>());
  else
    validate_research_intake_provenance(delivery["researchIntake"], id.get<std::string>(),
                                        raw["repo"].get<std::string>(), raw["issue"].get<int>());
}

bool backing_number(const std::string& value) {
  int number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  return error == std::errc{} && end == value.data() + value.size() && number > 0 &&
         std::to_string(number) == value;
}
}  // namespace

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

json Store::parse_issue_entity_(const json& issue, bool reject_duplicate_keys) {
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
    if (reject_duplicate_keys) {
      std::vector<std::set<std::string>> keys;
      return json::parse(body.substr(start, end - start),
                         [&keys](int, json::parse_event_t event, json& value) {
                           if (event == json::parse_event_t::object_start) keys.emplace_back();
                           if (event == json::parse_event_t::key &&
                               !keys.back().insert(value.get<std::string>()).second)
                             throw std::invalid_argument("Duplicate HMAS body member");
                           if (event == json::parse_event_t::object_end) keys.pop_back();
                           return true;
                         });
    }
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
      std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
      std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
      std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
      std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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

void Store::ensure_hmas_tasks_loaded_() {
  if (hmas_tasks_loaded_.load(std::memory_order_acquire)) return;
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  if (hmas_tasks_loaded_.load(std::memory_order_acquire)) return;
  if (gh_) {
    std::unordered_map<std::string, HmasTask> tasks;
    std::unordered_map<std::string, std::string> numbers;
    std::unordered_map<std::string, std::vector<std::string>> briefs;
    for (const auto& issue : gh_->list_issues_including_closed("agamemnon-hmas-task")) {
      auto entity = parse_issue_entity_(issue, true);
      if (!entity.is_object() || !entity.contains("id") || !issue.contains("number"))
        throw std::runtime_error("Malformed HMAS backing issue; reconciliation required");
      validate_import_entity(entity);
      auto task = hmas_task_from_json(entity);
      if (task.id.empty() || !tasks.emplace(task.id, task).second)
        throw std::runtime_error("Duplicate or empty HMAS task identity");
      numbers[task.id] = std::to_string(issue.at("number").get<int>());
      if (!task.brief_id.empty()) briefs[task.brief_id].push_back(task.id);
    }
    hmas_tasks_ = std::move(tasks);
    hmas_task_issue_numbers_ = std::move(numbers);
    hmas_tasks_by_brief_ = std::move(briefs);
  }
  hmas_tasks_loaded_.store(true, std::memory_order_release);
}

void Store::persist_hmas_task_(const HmasTask& task) {
  validate_import_entity(hmas_task_to_json(task));
  if (!hmas_tasks_loaded_.load(std::memory_order_acquire))
    throw std::runtime_error("HMAS state requires reconciliation");
  const auto existing = hmas_tasks_.find(task.id);
  if (std::string(import_kind(task)) != "" ||
      (existing != hmas_tasks_.end() && std::string(import_kind(existing->second)) != "")) {
    if (existing == hmas_tasks_.end() || !same_import_identity(existing->second, task))
      throw std::invalid_argument("Imported identity is immutable");
  }
  if (!gh_) return;
  if (existing == hmas_tasks_.end() || existing->second.repo != task.repo ||
      existing->second.issue != task.issue ||
      (std::string(import_kind(task)).empty() &&
       existing->second.delivery.dump() != task.delivery.dump()))
    guard_work_acquisition_(task, existing != hmas_tasks_.end());
  auto number = hmas_task_issue_numbers_.find(task.id);
  if (number == hmas_task_issue_numbers_.end())
    throw std::runtime_error("HMAS task has no durable backing issue: " + task.id);
  try {
    gh_->update_issue_body(number->second,
                           make_issue_body_("hmas-tasks/" + task.id, hmas_task_to_json(task)));
  } catch (...) {
    hmas_tasks_loaded_.store(false, std::memory_order_release);
    throw;
  }
}

std::optional<std::string> Store::guard_work_acquisition_(const HmasTask& task,
                                                          bool existing_identity) {
  if (!gh_ || task.issue == 0) return std::nullopt;
  if (task.issue < 0 || !import_configuration_)
    throw std::runtime_error("work_identity_reconciliation_required");
  ImportContext context;
  const auto work =
      resolve_work_issue(*import_configuration_, *gh_, task.repo, task.issue, context);
  const auto key = import_work_key(work);
  // A missing/disabled route never removes this Store guard. Any retained
  // phase is owned by the import operation; generic writes cannot adopt it.
  if (gh_->import_read_fence(import_configuration_->state_branch, key, context))
    throw std::runtime_error("work_identity_reserved");
  for (const auto& issue : gh_->import_list_issues(context)) {
    context.checkpoint();
    const auto raw = parse_issue_entity_(issue, true);
    if (!raw.is_object() || !raw.value("id", json()).is_string() ||
        !raw.value("repo", json()).is_string() || !raw.value("issue", json()).is_number_integer() ||
        raw["issue"] < 0 || raw["issue"] > std::numeric_limits<int>::max())
      throw std::runtime_error("work_identity_reconciliation_required");
    validate_import_entity(raw);
    if (raw["issue"] == 0) continue;
    const auto other =
        resolve_work_issue(*import_configuration_, *gh_, raw["repo"].get<std::string>(),
                           raw["issue"].get<int>(), context);
    if (other.repository_id == work.repository_id && other.issue_id == work.issue_id &&
        !(existing_identity && raw["id"].get<std::string>() == task.id))
      throw std::runtime_error("work_issue_already_imported");
  }
  return key;
}

void Store::ensure_briefs_loaded_() {
  if (briefs_loaded_.load(std::memory_order_acquire)) return;
  if (gh_) {
    std::call_once(briefs_once_, [this]() {
      std::vector<json> issues;
      try {
        issues = gh_->list_issues("agamemnon-brief");
      } catch (const std::exception& e) {
        std::cerr << "[agamemnon] hydration error (briefs): " << e.what() << "\n";
        briefs_loaded_.store(true, std::memory_order_release);
        return;
      }
      std::unique_lock<std::shared_timed_mutex> lk(mutex_);
      for (auto& issue : issues) {
        json entity = parse_issue_entity_(issue);
        if (entity.is_null() || !entity.contains("id")) {
          std::cerr << "[agamemnon] skipping malformed brief issue\n";
          continue;
        }
        try {
          TaskBrief b = task_brief_from_json(entity);
          if (issue.contains("number"))
            brief_issue_numbers_[b.id] = std::to_string(issue["number"].get<int>());
          task_briefs_[b.id] = std::move(b);
        } catch (const std::exception& e) {
          std::cerr << "[agamemnon] failed to deserialize brief: " << e.what() << "\n";
        }
      }
      briefs_loaded_.store(true, std::memory_order_release);
    });
  } else {
    briefs_loaded_.store(true, std::memory_order_release);
  }
}

std::unordered_map<std::string, json>* Store::pick_map_(std::string_view label) {
  if (label == "agamemnon-agent") {
    ensure_agents_loaded_();
    return &agents_;
  }
  if (label == "agamemnon-team") {
    ensure_teams_loaded_();
    return &teams_;
  }
  if (label == "agamemnon-task") {
    ensure_tasks_loaded_();
    return &tasks_;
  }
  if (label == "agamemnon-fault") {
    ensure_faults_loaded_();
    return &faults_;
  }
  return nullptr;
}

bool Store::apply_github_event(std::string_view entity_label, std::string_view action,
                               const json& issue_shape, std::string_view updated_at) {
  auto* map = pick_map_(entity_label);
  if (!map) return false;
  json entity = parse_issue_entity_(issue_shape);
  if (entity.is_null() || !entity.contains("id")) return false;
  const std::string id = entity["id"].get<std::string>();
  if (issue_shape.contains("number"))
    entity["_github_issue"] = std::to_string(issue_shape["number"].get<int>());

  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = map->find(id);

  // closed/reopened are terminal state transitions — always apply (no LWW skip).
  if (action == "closed") {
    if (it == map->end()) return false;
    it->second["status"] = "closed";
    it->second["closedAt"] = std::string(updated_at);
    it->second["updatedAt"] = std::string(updated_at);
    if (metrics_) metrics_->record_inbound_sync("closed");
    return true;
  }
  if (action == "reopened") {
    if (it == map->end()) {
      (*map)[id] = std::move(entity);
    } else {
      it->second["status"] = "active";
      it->second["updatedAt"] = std::string(updated_at);
    }
    if (metrics_) metrics_->record_inbound_sync("reopened");
    return true;
  }

  // edited / opened / labeled / unlabeled: LWW via updatedAt.
  if (it != map->end() && !updated_at.empty()) {
    const std::string local_ts = it->second.value("updatedAt", std::string{});
    if (!local_ts.empty() && local_ts >= std::string(updated_at)) {
      if (metrics_) metrics_->record_inbound_sync("skipped_stale");
      return false;
    }
  }
  entity["updatedAt"] = std::string(updated_at);
  (*map)[id] = std::move(entity);
  if (metrics_) metrics_->record_inbound_sync("applied");
  return true;
}

std::size_t Store::reconcile_from_github() {
  if (!gh_) return 0;
  static constexpr std::array<std::string_view, 4> kLabels{"agamemnon-agent", "agamemnon-team",
                                                           "agamemnon-task", "agamemnon-fault"};
  std::size_t changed = 0;
  for (auto label : kLabels) {
    std::vector<json> issues;
    try {
      issues = gh_->list_issues(label);
    } catch (const std::exception& e) {
      std::cerr << "[agamemnon] reconcile error (" << label << "): " << e.what() << "\n";
      continue;
    }
    for (auto& issue : issues) {
      std::string ts = issue.value("updated_at", "");
      if (apply_github_event(label, "edited", issue, ts)) ++changed;
    }
  }
  return changed;
}

// ── Agents ────────────────────────────────────────────────────────────────────

json Store::create_agent(const json& body) {
  ensure_agents_loaded_();  // hydrate before acquiring mutex_ (#161)
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  agent["updatedAt"] = agent["createdAt"];

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
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  return it->second;
}

json Store::get_agent_by_name(const std::string& name) {
  ensure_agents_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  for (auto& [id, agent] : agents_) {
    if (agent.value("name", "") == name) return agent;
  }
  return nullptr;
}

json Store::list_agents(std::size_t limit, std::size_t offset) {
  ensure_agents_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = agents_.find(id);
  if (it == agents_.end()) return nullptr;
  for (auto& [key, val] : fields.items()) {
    if (key != "id" && key != "createdAt" && key != "_github_issue" && key != "updatedAt")
      it->second[key] = val;
  }
  it->second["updatedAt"] = now_iso8601();
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                           make_issue_body_("agents/" + id, it->second));
  }
  return it->second;
}

bool Store::delete_agent(const std::string& id) {
  ensure_agents_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json team;
  team["id"] = id;
  team["name"] = body.value("name", "unnamed-team");
  team["agentIds"] =
      body.contains("agent_ids") ? body["agent_ids"] : body.value("agentIds", json::array());
  team["createdAt"] = now_iso8601();
  team["updatedAt"] = team["createdAt"];

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
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = teams_.find(id);
  if (it == teams_.end()) return nullptr;
  return it->second;
}

json Store::list_teams(std::size_t limit, std::size_t offset) {
  ensure_teams_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = teams_.find(id);
  if (it == teams_.end()) return nullptr;
  if (body.contains("agentIds"))
    it->second["agentIds"] = body["agentIds"];
  else if (body.contains("agent_ids"))
    it->second["agentIds"] = body["agent_ids"];
  if (body.contains("name")) it->second["name"] = body["name"];
  it->second["updatedAt"] = now_iso8601();
  if (gh_ && it->second.contains("_github_issue")) {
    gh_->update_issue_body(it->second["_github_issue"].get<std::string>(),
                           make_issue_body_("teams/" + id, it->second));
  }
  return it->second;
}

bool Store::delete_team(const std::string& id) {
  ensure_teams_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  task["updatedAt"] = task["createdAt"];
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
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return nullptr;
  if (it->second.value("teamId", "") != team_id) return nullptr;
  for (auto& [key, val] : body.items()) {
    if (key != "id" && key != "teamId" && key != "createdAt" && key != "_github_issue" &&
        key != "updatedAt")
      it->second[key] = val;
  }
  if (body.contains("status") && body["status"] == "completed" &&
      it->second.value("completedAt", json(nullptr)).is_null()) {
    it->second["completedAt"] = now_iso8601();
  }
  it->second["updatedAt"] = now_iso8601();
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
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  std::string id = generate_uuid();
  json fault;
  fault["id"] = id;
  fault["type"] = type;
  fault["active"] = true;
  fault["createdAt"] = now_iso8601();
  fault["updatedAt"] = fault["createdAt"];

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
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
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
  if (std::string(import_kind(task)) != "")
    throw std::invalid_argument("Use canonical intake import");
  ensure_hmas_tasks_loaded_();  // also guards write-first race
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  if (hmas_tasks_.contains(task.id)) throw std::runtime_error("HMAS task already exists");
  if (!task.fleet_claim.is_null())
    throw std::runtime_error("Use the canonical Fleet claim operation");
  guard_work_acquisition_(task);
  if (gh_) {
    const std::string title = "hmas-task: " + task.id;
    try {
      std::string issue_num = gh_->create_issue(
          title, make_issue_body_("hmas-tasks/" + task.id, hmas_task_to_json(task)),
          "agamemnon-hmas-task");
      if (issue_num.empty())
        throw std::runtime_error("GitHub did not acknowledge HMAS task creation");
      hmas_task_issue_numbers_[task.id] = issue_num;
    } catch (...) {
      hmas_tasks_loaded_.store(false, std::memory_order_release);
      throw;
    }
  }
  hmas_tasks_[task.id] = task;
  if (!task.brief_id.empty()) {
    hmas_tasks_by_brief_[task.brief_id].push_back(task.id);
  }
}

std::pair<HmasTask, bool> Store::import_research_task(const HmasTask& proposed) {
  if (!gh_ || !import_configuration_)
    throw std::runtime_error("Research import requires shared persistence configuration");
  if (!has_research_intake(proposed)) throw std::invalid_argument("Invalid research provenance");
  ImportContext context;
  const auto work =
      resolve_work_issue(*import_configuration_, *gh_, proposed.repo, proposed.issue, context);
  return import_task_(proposed, work, context);
}

std::pair<HmasTask, bool> Store::import_issue_task(const HmasTask& proposed,
                                                   const CanonicalWorkIssue& work,
                                                   ImportContext& context) {
  if (std::string(import_kind(proposed)) != "issueIntake")
    throw std::invalid_argument("Invalid issue provenance");
  return import_task_(proposed, work, context);
}

std::pair<HmasTask, bool> Store::import_task_(const HmasTask& proposed,
                                              const CanonicalWorkIssue& work,
                                              ImportContext& context) {
  if (!gh_ || !import_configuration_)
    throw std::runtime_error("Import requires shared persistence configuration");
  validate_issue_import_configuration(*import_configuration_);
  const std::string kind = import_kind(proposed);
  validate_import_entity(hmas_task_to_json(proposed));
  if (kind.empty() || !research_leaf(proposed) || proposed.id.empty() || proposed.repo.empty() ||
      proposed.issue <= 0 || proposed.state != TaskState::Pending ||
      !proposed.assigned_lead_id.empty() || !proposed.fleet_claim.is_null() ||
      !proposed.fleet_resolution.is_null() ||
      lower_work_name(proposed.repo) != lower_work_name(work.repository) ||
      proposed.issue != work.number)
    throw std::invalid_argument("Invalid proposed import leaf");
  context.checkpoint();
  std::unique_lock<std::shared_timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_for(std::chrono::milliseconds(context.remaining_ms())))
    throw std::runtime_error("import_store_busy");
  context.checkpoint();
  context.checkpoint();
  // An import never uses a warm cache as evidence that a durable identity is absent.
  hmas_tasks_loaded_.store(false, std::memory_order_release);
  const auto issues = gh_->import_list_issues(context);
  std::unordered_map<std::string, HmasTask> tasks;
  std::unordered_map<std::string, std::string> numbers;
  std::unordered_map<std::string, std::vector<std::string>> briefs;
  std::unordered_map<std::string, std::string> issue_owners;
  for (const auto& issue : issues) {
    context.checkpoint();
    try {
      const auto entity = parse_issue_entity_(issue, true);
      if (!entity.is_object() || !entity.contains("id") || !issue.is_object() ||
          !issue.value("number", json()).is_number_integer() || issue["number"] <= 0 ||
          issue["number"] > std::numeric_limits<int>::max())
        throw std::invalid_argument("Malformed HMAS backing record");
      const auto number = std::to_string(issue["number"].get<int>());
      validate_import_entity(entity);
      if (!backing_number(number) || issue["number"] != std::stoi(number))
        throw std::invalid_argument("Invalid HMAS backing number");
      if (!entity.value("repo", json()).is_string() ||
          !entity.value("issue", json()).is_number_integer() || entity["issue"] < 0 ||
          entity["issue"] > std::numeric_limits<int>::max())
        throw std::invalid_argument("Malformed raw work identity");
      if (entity.value("id", json()) == proposed.id) {
        const auto canonical = hmas_task_to_json(proposed);
        // Compare schema-typed identity before the legacy reader can default
        // missing fields, discard malformed lists, or narrow an issue number.
        for (const auto* field : {"id", "layer", "repo", "issue", "parent_task_id", "brief_id",
                                  "module", "child_task_ids", "blocked_by"})
          if (!entity.contains(field) || entity[field].dump() != canonical[field].dump())
            throw std::invalid_argument("Invalid raw import identity");
      }
      auto task = hmas_task_from_json(entity);
      if (task.id.empty() || !tasks.emplace(task.id, task).second ||
          !issue_owners.emplace(number, task.id).second)
        throw std::invalid_argument("Ambiguous HMAS backing identity");
      numbers[task.id] = number;
      if (!task.brief_id.empty()) briefs[task.brief_id].push_back(task.id);
      bool same_work = task.issue == work.number &&
                       lower_work_name(task.repo) == lower_work_name(work.repository);
      if (task.issue > 0 && !same_work) {
        const auto other =
            resolve_work_issue(*import_configuration_, *gh_, task.repo, task.issue, context);
        same_work = other.repository_id == work.repository_id && other.issue_id == work.issue_id;
      }
      if (same_work && task.id != proposed.id)
        throw std::invalid_argument("work_issue_already_imported");
      if (task.id == proposed.id) {
        for (const auto* field : {"state", "delivery"})
          if (!entity.contains(field))
            throw std::invalid_argument("Incomplete research backing record");
        if (issue.value("state", json()) != "open" || !same_work ||
            !same_import_identity(task, proposed, true))
          throw std::invalid_argument("Import backing record requires reconciliation");
      }
    } catch (const std::invalid_argument& error) {
      if (std::string(error.what()) == "work_issue_already_imported") throw;
      throw std::invalid_argument("Import backing record requires reconciliation");
    } catch (const std::exception&) {
      throw std::runtime_error("Import backing identity unavailable");
    }
  }
  bool created = false;
  if (!tasks.contains(proposed.id)) {
    if (!work.open) throw std::invalid_argument("Work issue is closed");
    const auto key = import_work_key(work);
    const auto& branch = import_configuration_->state_branch;
    const json identity{{"forge", "github"},
                        {"repositoryId", work.repository_id},
                        {"issueId", work.issue_id},
                        {"repository", work.repository},
                        {"number", work.number}};
    auto fence = gh_->import_read_fence(branch, key, context);
    bool prepared_here = false;
    auto write = [&](const json& document, const std::optional<std::string>& expected) {
      context.checkpoint();
      const auto confirmed = gh_->import_write_fence(branch, key, document, expected, context);
      if (confirmed.sha.empty() || confirmed.document.dump() != document.dump())
        throw std::runtime_error("Import conditional write unconfirmed");
      return confirmed;
    };
    if (!fence) {
      fence = write({{"schema", "hi/agamemnon/import-attempt/v1"},
                     {"work", identity},
                     {"taskId", proposed.id},
                     {"kind", kind},
                     {"provenance", proposed.delivery[kind]},
                     {"phase", "prepared"},
                     {"attemptId", generate_uuid()},
                     {"backingIssue", nullptr}},
                    std::nullopt);
      prepared_here = true;
    }
    const auto& retained = fence->document;
    if (!retained.is_object() || retained.size() != 8 ||
        retained.value("schema", json()) != "hi/agamemnon/import-attempt/v1" ||
        retained.value("work", json()).dump() != identity.dump() ||
        !retained.contains("backingIssue") || !retained.value("attemptId", json()).is_string() ||
        !std::regex_match(
            retained["attemptId"].get<std::string>(),
            std::regex("[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}")))
      throw std::runtime_error("Import fence requires reconciliation");
    if (retained.value("taskId", json()) != proposed.id || retained.value("kind", json()) != kind)
      throw std::invalid_argument("work_issue_already_imported");
    auto original = proposed;
    original.delivery[kind] = retained.value("provenance", json());
    try {
      validate_import_entity(hmas_task_to_json(original));
    } catch (const std::exception&) {
      throw std::runtime_error("Import fence requires reconciliation");
    }
    if (!same_import_identity(original, proposed, true))
      throw std::invalid_argument("Import intent changed");
    // Only this invocation's confirmed preparation can proceed. A retained
    // prepared record can include a lost write acknowledgement; observing it
    // does not authorize a new grant. V1 has no recovery/reset transition.
    if (!prepared_here || retained.value("phase", json()) != "prepared" ||
        !retained.value("backingIssue", json()).is_null())
      throw std::runtime_error("Import creation requires reconciliation");
    auto creating = retained;
    creating["phase"] = "creating";
    fence = write(creating, fence->sha);
    // Do not retry this mutation. Even an empty or invalid acknowledgement is uncertain.
    const auto number = gh_->import_create_issue(
        "hmas-task: " + proposed.id,
        make_issue_body_("hmas-tasks/" + proposed.id, hmas_task_to_json(original)), context);
    if (!backing_number(number) || issue_owners.contains(number))
      throw std::runtime_error("Research creation was not acknowledged");
    tasks.emplace(proposed.id, original);
    numbers[proposed.id] = number;
    created = true;
    auto linked = creating;
    linked["phase"] = "linked";
    linked["backingIssue"] = number;
    try {
      (void)write(linked, fence->sha);
    } catch (const std::exception&) { /* The acknowledged task remains canonical. */
    }
  }
  const auto result = tasks.at(proposed.id);
  hmas_tasks_ = std::move(tasks);
  hmas_task_issue_numbers_ = std::move(numbers);
  hmas_tasks_by_brief_ = std::move(briefs);
  hmas_tasks_loaded_.store(true, std::memory_order_release);
  return {result, created};
}

std::optional<HmasTask> Store::get_hmas_task(const std::string& id) {
  ensure_hmas_tasks_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(id);
  if (it == hmas_tasks_.end()) return std::nullopt;
  return it->second;  // value copy — safe to use outside the lock
}

bool Store::update_hmas_task_state_and_record_escalation(const std::string& id, TaskState new_state,
                                                         const EscalationRecord& escalation) {
  ensure_hmas_tasks_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(id);
  if (it == hmas_tasks_.end()) return false;
  if (!it->second.fleet_claim.is_null())
    throw std::runtime_error("Fleet-owned task requires a generation-fenced transition");
  auto updated = it->second;
  updated.state = new_state;
  updated.escalations.push_back(escalation);
  persist_hmas_task_(updated);
  it->second = std::move(updated);
  return true;
}

bool Store::update_hmas_task_state(const std::string& id, TaskState state) {
  ensure_hmas_tasks_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(id);
  if (it == hmas_tasks_.end()) return false;
  if (!it->second.fleet_claim.is_null())
    throw std::runtime_error("Fleet-owned task requires a generation-fenced transition");
  auto updated = it->second;
  updated.state = state;
  persist_hmas_task_(updated);
  it->second = std::move(updated);
  return true;
}

bool Store::update_hmas_task(const HmasTask& task) {
  ensure_hmas_tasks_loaded_();  // review fix: run hydration even on write-first paths
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(task.id);
  if (it == hmas_tasks_.end()) return false;
  if (!it->second.fleet_claim.is_null())
    throw std::runtime_error("Fleet-owned task requires a generation-fenced transition");
  persist_hmas_task_(task);
  const std::string old_brief = it->second.brief_id;
  it->second = task;
  if (old_brief != task.brief_id) {
    if (!old_brief.empty()) {
      auto& old_bucket = hmas_tasks_by_brief_[old_brief];
      old_bucket.erase(std::remove(old_bucket.begin(), old_bucket.end(), task.id),
                       old_bucket.end());
      if (old_bucket.empty()) hmas_tasks_by_brief_.erase(old_brief);
    }
    if (!task.brief_id.empty()) {
      hmas_tasks_by_brief_[task.brief_id].push_back(task.id);
    }
  }
  return true;
}

bool Store::append_hmas_children(const HmasTask& expected, const std::vector<HmasTask>& children) {
  ensure_hmas_tasks_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  const auto found = hmas_tasks_.find(expected.id);
  if (found == hmas_tasks_.end() || !found->second.fleet_claim.is_null() ||
      hmas_task_to_json(found->second) != hmas_task_to_json(expected))
    return false;
  auto parent = expected;
  guard_work_acquisition_(parent, true);
  std::set<std::string> child_ids;
  std::set<std::string> work_identities;
  for (const auto& child : children) {
    if (child.id.empty() || !child_ids.insert(child.id).second || hmas_tasks_.contains(child.id) ||
        !child.fleet_claim.is_null() || std::string(import_kind(child)) != "")
      throw std::invalid_argument("Invalid split child identity");
    const auto work = guard_work_acquisition_(child);
    if (work && !work_identities.insert(*work).second)
      throw std::invalid_argument("Duplicate split work identity");
    parent.child_task_ids.push_back(child.id);
  }
  // This is not a multi-issue transaction. A failed child create leaves known
  // parent links and prevents Fleet leaf admission until explicit reconciliation.
  persist_hmas_task_(parent);
  found->second = parent;
  for (const auto& child : children) {
    if (gh_) {
      try {
        const auto number =
            gh_->create_issue("hmas-task: " + child.id,
                              make_issue_body_("hmas-tasks/" + child.id, hmas_task_to_json(child)),
                              "agamemnon-hmas-task");
        if (number.empty()) throw std::runtime_error("GitHub did not acknowledge split child");
        hmas_task_issue_numbers_[child.id] = number;
      } catch (...) {
        hmas_tasks_loaded_.store(false, std::memory_order_release);
        throw;
      }
    }
    hmas_tasks_[child.id] = child;
    if (!child.brief_id.empty()) hmas_tasks_by_brief_[child.brief_id].push_back(child.id);
  }
  return true;
}

bool Store::reserve_hmas_fleet_claim(const std::string& id, const json& claim) {
  if (!gh_) throw std::runtime_error("Fleet claims require GitHub persistence");
  ensure_hmas_tasks_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  if (!hmas_tasks_loaded_.load(std::memory_order_acquire))
    throw std::runtime_error("HMAS state requires reconciliation");
  auto found = hmas_tasks_.find(id);
  if (found == hmas_tasks_.end()) return false;
  const auto& task = found->second;
  if (!task.fleet_claim.is_null())
    return task.fleet_claim == claim && task.state != TaskState::Completed &&
           task.state != TaskState::Failed;
  if (task.layer != HmasLayer::L3_TaskAgent || !task.child_task_ids.empty() ||
      task.state != TaskState::Pending || !task.assigned_lead_id.empty())
    return false;
  for (const auto& dependency : task.blocked_by) {
    auto blocker = hmas_tasks_.find(dependency);
    if (blocker == hmas_tasks_.end() || blocker->second.state != TaskState::Completed) return false;
  }
  if (!claim.is_object() || claim.value("schema", "") != "hi/fleet/claim/v1" ||
      !claim.contains("generation") || !claim["generation"].is_number_integer() ||
      claim["generation"].get<std::int64_t>() < 1)
    throw std::invalid_argument("Invalid Fleet claim schema");
  auto updated = task;
  updated.fleet_claim = claim;
  updated.assigned_lead_id = claim.at("agentId").get<std::string>();
  updated.state = TaskState::Delegated;
  persist_hmas_task_(updated);
  found->second = std::move(updated);
  return true;
}

bool Store::update_hmas_delivery(const std::string& id, const json& expected,
                                 const json& delivery) {
  if (!gh_) throw std::runtime_error("Durable delivery requires GitHub persistence");
  ensure_hmas_tasks_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = hmas_tasks_.find(id);
  if (it == hmas_tasks_.end() || it->second.delivery != expected) return false;
  auto updated = it->second;
  updated.delivery = delivery;
  persist_hmas_task_(updated);
  it->second = std::move(updated);
  return true;
}

bool Store::observe_hmas_fleet_start(const std::string& id, const json& claim) {
  ensure_hmas_tasks_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  if (!hmas_tasks_loaded_.load(std::memory_order_acquire))
    throw std::runtime_error("HMAS state requires reconciliation");
  auto found = hmas_tasks_.find(id);
  if (found == hmas_tasks_.end() || found->second.fleet_claim.is_null() ||
      found->second.fleet_claim != claim)
    return false;
  if (found->second.state == TaskState::InProgress) return true;
  if (found->second.state != TaskState::Delegated) return false;
  auto updated = found->second;
  updated.state = TaskState::InProgress;
  persist_hmas_task_(updated);
  found->second = std::move(updated);
  return true;
}

std::optional<HmasTask> Store::resolve_hmas_fleet_task(const std::string& id, const json& claim,
                                                       const json& decision) {
  ensure_hmas_tasks_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  if (!hmas_tasks_loaded_.load(std::memory_order_acquire))
    throw std::runtime_error("HMAS state requires reconciliation");
  auto found = hmas_tasks_.find(id);
  if (found == hmas_tasks_.end() || found->second.fleet_claim.is_null() ||
      found->second.fleet_claim != claim)
    return std::nullopt;
  const auto& task = found->second;
  if (!task.fleet_resolution.is_null())
    return task.fleet_resolution == decision ? std::optional<HmasTask>(task) : std::nullopt;
  const auto outcome = decision.at("outcome").get<std::string>();
  if ((outcome != "completed" && outcome != "failed") ||
      (task.state != TaskState::InProgress && task.state != TaskState::Delegated) ||
      (outcome == "completed" && task.state != TaskState::InProgress))
    return std::nullopt;
  auto updated = task;
  updated.fleet_resolution = decision;
  updated.state = outcome == "completed" ? TaskState::Completed : TaskState::Failed;
  updated.completed_at = now_iso8601();
  persist_hmas_task_(updated);
  found->second = updated;
  return updated;
}

std::vector<HmasTask> Store::list_hmas_tasks_by_layer(HmasLayer layer) {
  ensure_hmas_tasks_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  std::vector<HmasTask> out;
  for (const auto& [id, task] : hmas_tasks_) {
    if (task.layer == layer) out.push_back(task);
  }
  return out;
}

std::vector<HmasTask> Store::list_hmas_tasks_by_parent(const std::string& parent_id) {
  ensure_hmas_tasks_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  std::vector<HmasTask> out;
  for (const auto& [id, task] : hmas_tasks_) {
    if (task.parent_task_id == parent_id) out.push_back(task);
  }
  return out;
}

std::vector<HmasTask> Store::list_hmas_tasks_by_brief(const std::string& brief_id) {
  ensure_hmas_tasks_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  std::vector<HmasTask> out;
  auto idx_it = hmas_tasks_by_brief_.find(brief_id);
  if (idx_it == hmas_tasks_by_brief_.end()) return out;
  out.reserve(idx_it->second.size());
  for (const auto& task_id : idx_it->second) {
    auto t_it = hmas_tasks_.find(task_id);
    if (t_it != hmas_tasks_.end()) out.push_back(t_it->second);
  }
  return out;
}

// ── TaskBriefs ────────────────────────────────────────────────────────────────

void Store::create_task_brief(const TaskBrief& brief) {
  ensure_briefs_loaded_();
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  if (gh_) {
    // Truncate by code points, not bytes, to avoid splitting UTF-8 sequences
    std::string truncated_title;
    truncated_title.reserve(80);
    std::size_t code_points = 0;
    for (std::size_t i = 0; i < brief.title.size() && code_points < 80;) {
      unsigned char c = static_cast<unsigned char>(brief.title[i]);
      std::size_t adv = (c < 0x80) ? 1u : (c < 0xC0) ? 1u : (c < 0xE0) ? 2u : (c < 0xF0) ? 3u : 4u;
      if (i + adv > brief.title.size()) break;
      truncated_title.append(brief.title, i, adv);
      i += adv;
      ++code_points;
    }
    std::string issue_num = gh_->create_issue(
        "brief: " + truncated_title,
        make_issue_body_("briefs/" + brief.id, task_brief_to_json(brief)), "agamemnon-brief");
    if (!issue_num.empty()) brief_issue_numbers_[brief.id] = issue_num;
  }
  task_briefs_[brief.id] = brief;
}

void Store::ensure_durable_task_brief(const TaskBrief& brief) {
  if (!gh_) throw std::runtime_error("Durable brief requires GitHub persistence");
  std::unique_lock<std::shared_timed_mutex> lk(mutex_);
  std::string issue_number;
  // Always enumerate: this also reconciles a lost create response on retry.
  for (const auto& issue : gh_->list_issues_including_closed("agamemnon-brief")) {
    auto entity = parse_issue_entity_(issue);
    if (!entity.is_object()) throw std::runtime_error("Invalid durable brief record");
    if (entity.value("id", "") != brief.id) continue;
    if (!issue_number.empty() || entity != task_brief_to_json(brief))
      throw std::invalid_argument("Conflicting durable brief registration");
    issue_number = std::to_string(issue.at("number").get<int>());
  }
  if (issue_number.empty()) {
    issue_number = gh_->create_issue(
        "brief: " + brief.id, make_issue_body_("briefs/" + brief.id, task_brief_to_json(brief)),
        "agamemnon-brief");
    if (issue_number.empty()) throw std::runtime_error("GitHub did not acknowledge brief creation");
  }
  brief_issue_numbers_[brief.id] = issue_number;
  task_briefs_[brief.id] = brief;
}

std::optional<TaskBrief> Store::get_task_brief(const std::string& id) {
  ensure_briefs_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  auto it = task_briefs_.find(id);
  if (it == task_briefs_.end()) return std::nullopt;
  return it->second;
}

std::vector<TaskBrief> Store::list_task_briefs() {
  ensure_briefs_loaded_();
  std::shared_lock<std::shared_timed_mutex> lk(mutex_);
  std::vector<TaskBrief> out;
  out.reserve(task_briefs_.size());
  for (const auto& [_, b] : task_briefs_) out.push_back(b);
  return out;
}

}  // namespace agamemnon
