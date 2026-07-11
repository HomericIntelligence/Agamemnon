#include "agamemnon/github_webhook.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <vector>

namespace agamemnon {

namespace {

constexpr std::string_view kSha256Prefix = "sha256=";
constexpr std::size_t kSha256HexLength = 64;

// Hex-decode a string into a buffer. Returns false if not valid hex.
bool hex_decode(std::string_view hex, std::vector<unsigned char>& output) {
  if (hex.size() % 2 != 0) return false;
  output.clear();
  output.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    int high = std::isxdigit(hex[i]) ? std::stoi(std::string(1, hex[i]), nullptr, 16) : -1;
    int low = std::isxdigit(hex[i + 1]) ? std::stoi(std::string(1, hex[i + 1]), nullptr, 16) : -1;
    if (high == -1 || low == -1) return false;
    output.push_back(static_cast<unsigned char>((high << 4) | low));
  }
  return true;
}

}  // namespace

bool verify_github_signature(std::string_view secret, std::string_view header_value,
                             std::string_view raw_body) {
  // Empty secret always fails.
  if (secret.empty()) return false;

  // Header must be "sha256=<64-hex-chars>"
  if (header_value.size() != kSha256Prefix.size() + kSha256HexLength) return false;
  if (!header_value.starts_with(kSha256Prefix)) return false;

  std::string_view hex_sig = header_value.substr(kSha256Prefix.size());

  // Decode the hex signature.
  std::vector<unsigned char> expected_sig;
  if (!hex_decode(hex_sig, expected_sig)) return false;

  // Compute HMAC-SHA-256 using EVP_hmac (works on both OpenSSL 1.1 and 3.x).
  unsigned int sig_len = 0;
  std::array<unsigned char, EVP_MAX_MD_SIZE> computed_sig;

  const EVP_MD* md = EVP_sha256();
  if (!md) return false;

  if (!HMAC(md, secret.data(), static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char*>(raw_body.data()), raw_body.size(),
            computed_sig.data(), &sig_len)) {
    return false;
  }

  if (sig_len != expected_sig.size()) return false;

  // Constant-time comparison.
  return CRYPTO_memcmp(computed_sig.data(), expected_sig.data(), sig_len) == 0;
}

std::optional<NormalizedEvent> normalize_issues_event(const nlohmann::json& payload) {
  using json = nlohmann::json;

  // Extract action and filter.
  if (!payload.contains("action") || !payload["action"].is_string()) {
    return std::nullopt;
  }
  std::string action = payload["action"].get<std::string>();

  // Allowlist of actions we care about.
  static const std::array<std::string_view, 6> kAllowedActions = {
      "opened", "edited", "closed", "reopened", "labeled", "unlabeled"};

  if (std::find(kAllowedActions.begin(), kAllowedActions.end(), action) == kAllowedActions.end()) {
    return std::nullopt;
  }

  // Extract issue.
  if (!payload.contains("issue") || !payload["issue"].is_object()) {
    return std::nullopt;
  }
  const json& issue = payload["issue"];

  if (!issue.contains("number") || !issue["number"].is_number_integer()) {
    return std::nullopt;
  }
  if (!issue.contains("body") || !issue["body"].is_string()) {
    return std::nullopt;
  }

  // Look for the first agamemnon-* label.
  std::string entity_label;
  if (issue.contains("labels") && issue["labels"].is_array()) {
    for (const auto& label_obj : issue["labels"]) {
      if (!label_obj.is_object() || !label_obj.contains("name") || !label_obj["name"].is_string()) {
        continue;
      }
      std::string label_name = label_obj["name"].get<std::string>();
      if (label_name.starts_with("agamemnon-")) {
        entity_label = label_name;
        break;
      }
    }
  }

  if (entity_label.empty()) {
    return std::nullopt;
  }

  // Extract updated_at (RFC 3339 from GitHub).
  std::string updated_at;
  if (issue.contains("updated_at") && issue["updated_at"].is_string()) {
    updated_at = issue["updated_at"].get<std::string>();
  }

  // Build issue_shape: the minimal shape needed by parse_issue_entity_.
  json issue_shape = json::object();
  issue_shape["number"] = issue["number"].get<int>();
  issue_shape["body"] = issue["body"].get<std::string>();
  if (issue.contains("state") && issue["state"].is_string()) {
    issue_shape["state"] = issue["state"].get<std::string>();
  }

  return NormalizedEvent{
      .action = action,
      .entity_label = entity_label,
      .issue_shape = issue_shape,
      .updated_at = updated_at,
  };
}

}  // namespace agamemnon
