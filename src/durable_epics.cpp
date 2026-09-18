#include "agamemnon/nats_publisher.hpp"
#include "agamemnon/orchestrator.hpp"
#include "agamemnon/store.hpp"

#include <chrono>
#include <openssl/sha.h>
#include <regex>
#include <stdexcept>

namespace agamemnon {
namespace {
std::string canonical_repo(std::string repo) {
  // GitHub owner/repository identity is ASCII case-insensitive. Do not fold
  // workflow text, keys, or any unrelated registration fields.
  for (char& character : repo)
    if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
  return repo;
}
std::string identity(const std::string& input) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto byte : digest) {
    out += hex[byte >> 4];
    out += hex[byte & 15];
  }
  return out;
}
std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void Orchestrator::reconcile_parent_wakeups() {
  if (!store_.github_client())
    throw std::runtime_error("Parent wakeup requires GitHub persistence");
  for (int layer = 1; layer <= 3; ++layer) {
    for (const auto& child : store_.list_hmas_tasks_by_layer(static_cast<HmasLayer>(layer))) {
      if (child.state != TaskState::Completed || child.parent_task_id.empty()) continue;
      auto parent = store_.get_hmas_task(child.parent_task_id);
      if (!parent || !parent->fleet_claim.is_null() || !parent->assigned_lead_id.empty() ||
          (parent->state != TaskState::Decomposing && parent->state != TaskState::Delegated))
        continue;
      // Scope wakeups to a tree rooted in the explicitly durable epic path.
      const auto tree = store_.list_hmas_tasks_by_brief(parent->brief_id);
      const bool durable_tree = std::any_of(tree.begin(), tree.end(), [](const HmasTask& task) {
        return task.parent_task_id.empty() && task.delivery.contains("registration");
      });
      if (!durable_tree) continue;
      const auto message_id =
          "parent-wake-" + identity(child.id + "|" + parent->id + "|" + child.completed_at);
      json wake = {{"schema", "hi/v1"},
                   {"msg_id", message_id},
                   {"operation", "child_completed"},
                   {"task_id", parent->id},
                   {"brief_id", parent->brief_id},
                   {"completed_child_id", child.id},
                   {"canonical_state", "Completed"}};
      publish_checkpoint(
          child, "parentWake",
          mesh_dispatch_subject("pipeline", mesh_role_name(parent->layer), parent->id), wake,
          parent);
    }
  }
}

void Orchestrator::publish_checkpoint(HmasTask task, const std::string& key,
                                      const std::string& subject, const json& envelope,
                                      const std::optional<HmasTask>& parent) {
  const json intent = {{"subject", subject}, {"envelope", envelope}};
  auto delivery = task.delivery;
  if (delivery.contains(key)) {
    if (delivery[key].at("intent") != intent)
      throw std::invalid_argument("Conflicting durable publication");
    if (delivery[key].at("phase") == "published") return;
  } else {
    delivery[key] = {{"intent", intent}, {"phase", "pending"}, {"attemptedAt", now_ms()}};
    if (!store_.update_hmas_delivery(task.id, task.delivery, delivery))
      throw std::runtime_error("Durable publication changed concurrently");
    task.delivery = delivery;
  }
  const auto publish = [&] {
    const auto elapsed = now_ms() - delivery[key].at("attemptedAt").get<std::int64_t>();
    // Check after any persistence or lock wait. Broker deduplication is finite;
    // an older uncertain intent needs reconciliation instead of another send.
    if (elapsed < 0 || elapsed >= 60000)
      throw std::invalid_argument("publication_uncertain_requires_reconciliation");
    if (!nats_.publish_durable(subject, envelope.dump(), envelope.at("msg_id").get<std::string>()))
      throw std::runtime_error("JetStream did not acknowledge publication");
  };
  if (parent) {
    if (!store_.publish_hmas_parent_wakeup(task, *parent, publish)) return;
  } else {
    publish();
  }
  delivery[key]["phase"] = "published";
  if (!store_.update_hmas_delivery(task.id, task.delivery, delivery))
    throw std::runtime_error("Durable publication changed concurrently");
}

std::string Orchestrator::register_epic_durable(const std::string& subject,
                                                const std::string& payload) {
  if (!store_.github_client()) throw std::runtime_error("Durable epic requires GitHub persistence");
  json msg;
  try {
    msg = json::parse(payload);
  } catch (const json::exception&) {
    throw std::invalid_argument("invalid_epic_json");
  }
  try {
    auto epic = msg.at("epic");
    auto repo = epic.at("repo").get<std::string>();
    const auto key = epic.at("key").get<std::string>();
    const auto issue = epic.at("issue").get<int>();
    const auto team = msg.value("team_id", "mesh");
    if (msg.at("schema") != "hi/v1" || msg.at("msg_id").get<std::string>().empty() ||
        !std::regex_match(repo, std::regex("[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")) || issue <= 0 ||
        !std::regex_match(key, std::regex("[a-z0-9-]+")) ||
        !std::regex_match(team, std::regex("[A-Za-z0-9_-]+")) ||
        subject != "hi.pipeline.epic." + key + ".registered" || !msg.at("children").is_array())
      throw std::invalid_argument("invalid_epic_envelope");
    repo = canonical_repo(repo);
    epic["repo"] = repo;
    auto children = msg.at("children").get<std::vector<int>>();
    for (int child : children)
      if (child <= 0) throw std::invalid_argument("invalid_child_issue");
    std::sort(children.begin(), children.end());
    if (std::adjacent_find(children.begin(), children.end()) != children.end())
      throw std::invalid_argument("duplicate_child_issue");
    const json registration = {{"schema", "hi/epic-registration/v1"},
                               {"epic", epic},
                               {"children", children},
                               {"workflow", msg.at("workflow").get<std::string>()},
                               {"team_id", team}};
    const auto digest = identity(repo + "#" + std::to_string(issue));
    const auto task_id = "epic-root-" + digest;
    // Earlier local builds hashed the original spelling. Never create a
    // replacement graph while those durable IDs/outbox references still exist.
    for (const auto& prior : store_.list_hmas_tasks_by_layer(HmasLayer::L0_ChiefArchitect)) {
      if (prior.id == task_id || !prior.parent_task_id.empty() ||
          !prior.delivery.contains("registration"))
        continue;
      const auto& previous = prior.delivery.at("registration");
      if (!previous.is_object() || !previous.contains("epic") || !previous["epic"].is_object() ||
          !previous["epic"].contains("repo") || !previous["epic"]["repo"].is_string() ||
          !previous["epic"].contains("issue") || !previous["epic"]["issue"].is_number_integer())
        throw std::runtime_error("epic_identity_requires_reconciliation");
      if (previous["epic"]["issue"].get<int>() == issue &&
          canonical_repo(previous["epic"]["repo"].get<std::string>()) == repo)
        throw std::invalid_argument("epic_identity_migration_required");
    }
    TaskBrief brief;
    brief.id = "epic-" + digest;
    brief.title = "[epic] " + key;
    brief.description = "Decompose epic " + repo + "#" + std::to_string(issue);
    brief.repos = {repo};
    store_.ensure_durable_task_brief(brief);
    auto existing = store_.get_hmas_task(task_id);
    HmasTask root;
    if (existing) {
      root = *existing;
      if (root.delivery.value("registration", json(nullptr)) != registration)
        throw std::invalid_argument("epic_registration_conflict");
    } else {
      root.id = task_id;
      root.brief_id = brief.id;
      root.layer = HmasLayer::L0_ChiefArchitect;
      root.state = TaskState::Decomposing;
      root.subject = brief.title;
      root.description = brief.description;
      root.repo = repo;
      root.issue = issue;
      root.created_at = now_iso8601();
      root.delivery["registration"] = registration;
      store_.create_hmas_task(root);
    }
    json dispatch = {{"schema", "hi/v1"},
                     {"msg_id", "epic-dispatch-" + digest},
                     {"operation", "decompose"},
                     {"task_id", root.id},
                     {"brief_id", brief.id},
                     {"team_id", team},
                     {"epic", epic},
                     {"children", children},
                     {"workflow", registration["workflow"]}};
    publish_checkpoint(root, "dispatch",
                       mesh_dispatch_subject("pipeline", "chief-architect", root.id), dispatch);
    return brief.id;
  } catch (const json::exception&) {
    throw std::invalid_argument("invalid_epic_envelope");
  }
}
}  // namespace agamemnon
