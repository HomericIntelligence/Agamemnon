#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

/// GitHub REST API client for Issues-backed persistence.
///
/// Designed for graceful degradation: if GITHUB_TOKEN / GITHUB_OWNER / GITHUB_REPO
/// are absent, is_enabled() returns false and all operations are no-ops returning
/// sentinel values.  Failures never throw; they log to stderr and return sentinels.
class GitHubClient {
 public:
  struct Config {
    std::string token;
    std::string owner;
    std::string repo;
  };

  /// Construct from config.  If any field is empty, the client is disabled.
  explicit GitHubClient(Config cfg);

  /// Read config from environment (GITHUB_TOKEN, GITHUB_OWNER, GITHUB_REPO).
  static Config config_from_env();

  bool is_enabled() const { return enabled_; }

  /// Create a GitHub issue.  Returns the issue number on success, -1 on failure.
  int create_issue(const std::string& title,
                   const std::string& body,
                   const std::vector<std::string>& labels);

  /// Update an existing issue.  Returns true on success.
  bool update_issue(int number,
                    const std::string& title,
                    const std::string& body,
                    const std::vector<std::string>& labels,
                    const std::string& state);  // "open" | "closed"

  /// List issues filtered by a label.  Returns empty array on failure or disabled.
  nlohmann::json list_issues(const std::string& label,
                             const std::string& state = "all");

 private:
  Config cfg_;
  bool enabled_ = false;
  std::mutex http_mutex_;

  nlohmann::json do_get(const std::string& path);
  nlohmann::json do_post(const std::string& path, const nlohmann::json& body);
  nlohmann::json do_patch(const std::string& path, const nlohmann::json& body);
};

}  // namespace projectagamemnon
