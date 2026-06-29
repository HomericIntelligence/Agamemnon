#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

/// Constant-time HMAC-SHA-256 verification of GitHub's X-Hub-Signature-256 header.
/// header_value example: "sha256=ab12...". Empty secret -> always false.
bool verify_github_signature(std::string_view secret, std::string_view header_value,
                             std::string_view raw_body);

/// Normalized issues-event view consumed by Store::apply_github_event.
struct NormalizedEvent {
  std::string action;          // opened | edited | closed | reopened | labeled | unlabeled
  std::string entity_label;    // agamemnon-agent | -team | -task | -fault
  nlohmann::json issue_shape;  // {"number": N, "body": "..."}  (matches parse_issue_entity_)
  std::string updated_at;      // RFC 3339 from payload.issue.updated_at (may be empty)
};

/// Returns nullopt for ignored actions (assigned, milestoned, etc.) or payloads
/// lacking required fields / an agamemnon-* label.
std::optional<NormalizedEvent> normalize_issues_event(const nlohmann::json& payload);

}  // namespace projectagamemnon
