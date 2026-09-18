#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace agamemnon {

using json = nlohmann::json;

/// Validate an existing state branch name without external access.
void validate_github_state_branch(const std::string& branch);

/// One import's shared monotonic deadline and received-body budget.
class ImportContext {
 public:
  explicit ImportContext(std::chrono::milliseconds budget = std::chrono::seconds(30));
  void checkpoint() const;
  long remaining_ms() const;
  void consume_body(std::size_t bytes);

 private:
  std::chrono::steady_clock::time_point deadline_;
  std::size_t received_{0};
};

struct ImportFence {
  std::string sha;
  json document;
};

/// Abstract interface for GitHub Issues API operations used by Store.
class IGitHubClient {
 public:
  virtual ~IGitHubClient() = default;

  // Import methods deliberately do not fall back to unbounded generic operations.
  virtual json import_work_issue(const std::string&, const std::string&, int, ImportContext&) {
    throw std::runtime_error("bounded GitHub import is unsupported");
  }
  virtual json import_plan_comment(const std::string&, ImportContext&) {
    throw std::runtime_error("bounded GitHub import is unsupported");
  }
  virtual std::vector<json> import_list_issues(ImportContext&) {
    throw std::runtime_error("bounded GitHub import is unsupported");
  }
  virtual std::optional<ImportFence> import_read_fence(const std::string&, const std::string&,
                                                       ImportContext&) {
    throw std::runtime_error("bounded GitHub import is unsupported");
  }
  virtual ImportFence import_write_fence(const std::string&, const std::string&, const json&,
                                         const std::optional<std::string>&, ImportContext&) {
    throw std::runtime_error("bounded GitHub import is unsupported");
  }
  virtual std::string import_create_issue(std::string_view, std::string_view, ImportContext&) {
    throw std::runtime_error("bounded GitHub import is unsupported");
  }
  virtual std::optional<ImportFence> build_read_fence(const std::string&, ImportContext&) {
    throw std::runtime_error("durable build admission is unsupported");
  }
  virtual ImportFence build_write_fence(const std::string&, const json&,
                                        const std::optional<std::string>&, ImportContext&) {
    throw std::runtime_error("durable build admission is unsupported");
  }

  /// Returns all open issue bodies with the given label.
  virtual std::vector<json> list_issues(std::string_view label) = 0;

  /// Fleet ownership cannot disappear when a backing issue is closed.
  /// Clients without complete enumeration must fail closed.
  virtual std::vector<json> list_issues_including_closed(std::string_view) {
    throw std::runtime_error("complete GitHub issue enumeration is unsupported");
  }

  /// Creates a new issue; returns the issue number as a string.
  virtual std::string create_issue(std::string_view title, std::string_view body,
                                   std::string_view label) = 0;

  /// Replaces the body of an existing issue (identified by number string).
  virtual void update_issue_body(std::string_view issue_number, std::string_view body) = 0;

  /// Closes an issue (soft-delete).
  virtual void close_issue(std::string_view issue_number) = 0;
  /// Returns GraphQL data; transport and GraphQL errors must throw.
  virtual json graphql(const std::string&, const json&) {
    throw std::runtime_error("GitHub GraphQL is unsupported");
  }
};

/// In-memory stub for unit tests — zero network, zero GitHub tokens needed.
class MockGitHubClient : public IGitHubClient {
 public:
  struct Call {
    std::string method;
    std::string arg1;
    std::string arg2;
    std::string arg3;
  };

  std::vector<json> list_issues(std::string_view label) override {
    calls.push_back({"list_issues", std::string(label), {}, {}});
    if (!fail_list_on_label.empty() && fail_list_on_label == std::string(label)) {
      throw std::runtime_error("simulated GitHub list failure for label: " + std::string(label));
    }
    auto it = seed_issues.find(std::string(label));
    if (it == seed_issues.end()) return {};
    return it->second;
  }

  std::vector<json> list_issues_including_closed(std::string_view label) override {
    return list_issues(label);
  }

  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    calls.push_back({"create_issue", std::string(title), std::string(body), std::string(label)});
    std::string num = std::to_string(++next_issue_number_);
    created_issues[num] = {
        {"title", std::string(title)}, {"body", std::string(body)}, {"label", std::string(label)}};
    return num;
  }

  void update_issue_body(std::string_view issue_number, std::string_view body) override {
    calls.push_back({"update_issue_body", std::string(issue_number), std::string(body), {}});
    auto it = created_issues.find(std::string(issue_number));
    if (it != created_issues.end()) it->second["body"] = std::string(body);
    updated_bodies[std::string(issue_number)] = std::string(body);
  }

  void close_issue(std::string_view issue_number) override {
    calls.push_back({"close_issue", std::string(issue_number), {}, {}});
    closed_issues.push_back(std::string(issue_number));
  }

  /// Seed data: map label -> list of full issue JSON objects (with "body" field).
  std::unordered_map<std::string, std::vector<json>> seed_issues;

  /// When non-empty, list_issues() throws std::runtime_error for this label (simulates GitHub 404).
  std::string fail_list_on_label;

  std::vector<Call> calls;
  std::unordered_map<std::string, json> created_issues;
  std::unordered_map<std::string, std::string> updated_bodies;
  std::vector<std::string> closed_issues;

 private:
  int next_issue_number_{0};
};

/// Real GitHub client using libcurl to call the GitHub REST API v3.
/// Requires GITHUB_TOKEN env var and a repo in "owner/repo" format.
///
/// Retry contract: do_get / do_patch automatically retry up to
/// kMaxRetries times with exponential backoff (1 s -> 2 s -> 4 s) on:
///   - transport errors (CURLcode != CURLE_OK)
///   - HTTP 5xx responses
///   - HTTP 429 responses (honors the Retry-After header when present)
///
/// 4xx responses other than 429 are NOT retried; they indicate client bugs.
/// Callers may observe up to ~7 seconds of total elapsed time per call in the
/// worst case (3 retries x (1 + 2 + 4) s backoff ceiling).
/// POST never retries at the transport layer: an uncertain create/mutation
/// must first reconcile its durable identity at the owning caller.
class CurlGitHubClient : public IGitHubClient {
 public:
  CurlGitHubClient(std::string repo, std::string token);
  ~CurlGitHubClient() override;

  std::vector<json> list_issues(std::string_view label) override;
  std::vector<json> list_issues_including_closed(std::string_view label) override;
  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override;
  void update_issue_body(std::string_view issue_number, std::string_view body) override;
  void close_issue(std::string_view issue_number) override;
  json graphql(const std::string& query, const json& variables) override;
  json import_work_issue(const std::string& owner, const std::string& name, int number,
                         ImportContext& context) override;
  json import_plan_comment(const std::string& id, ImportContext& context) override;
  std::vector<json> import_list_issues(ImportContext& context) override;
  std::optional<ImportFence> import_read_fence(const std::string& branch, const std::string& key,
                                               ImportContext& context) override;
  ImportFence import_write_fence(const std::string& branch, const std::string& key,
                                 const json& document,
                                 const std::optional<std::string>& expected_sha,
                                 ImportContext& context) override;
  std::string import_create_issue(std::string_view title, std::string_view body,
                                  ImportContext& context) override;
  std::optional<ImportFence> build_read_fence(const std::string& branch,
                                              ImportContext& context) override;
  ImportFence build_write_fence(const std::string& branch, const json& document,
                                const std::optional<std::string>& expected_sha,
                                ImportContext& context) override;

  // Retry / backoff constants (exposed for testing).
  static constexpr int kMaxRetries = 3;
  static constexpr int kBaseRetryMs = 1000;  // 1 s -> 2 s -> 4 s

  /// Parsed result of a single HTTP attempt (also used as the retry unit).
  struct Response {
    long status{0};
    std::string body;
    std::string retry_after;  // raw value of Retry-After response header, if any
  };

  /// Execute op with retry / exponential-backoff on transient failures.
  /// sleep_fn is called instead of std::this_thread::sleep_for (injectable for tests).
  /// label is used only for log messages (e.g. "GET", "POST").
  static Response with_retry(const std::string& label, const std::string& url,
                             std::function<Response()> op,
                             std::function<void(int /*ms*/)> sleep_fn = {});

 private:
  std::string repo_;
  std::string token_;
  int import_loopback_port_{0};
  std::vector<json> list_issues_(std::string_view label, std::string_view state);
  Response import_request_(const char* method, const std::string& path, const std::string& payload,
                           ImportContext& context) const;
  json import_graphql_(const char* query, const json& variables, ImportContext& context) const;
  std::optional<ImportFence> read_fence_(const std::string& branch, const std::string& path,
                                         ImportContext& context);
  ImportFence write_fence_(const std::string& branch, const std::string& path,
                           const std::string& message, const json& document,
                           const std::optional<std::string>& expected_sha, ImportContext& context);

 protected:
  // Only derived transport tests can select a literal loopback server.
  CurlGitHubClient(std::string repo, std::string token, int loopback_port);
  virtual bool import_resolver_supports_timeout() const;
  // Transport seam lets persistence contracts exercise real response handling
  // without network access or credentials.
  virtual Response do_get(const std::string& url) const;
  virtual Response do_post(const std::string& url, const std::string& payload) const;
  virtual Response do_post_once(const std::string& url, const std::string& payload) const;
  virtual Response do_patch(const std::string& url, const std::string& payload) const;
};

}  // namespace agamemnon
