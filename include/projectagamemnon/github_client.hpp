#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace projectagamemnon {

using json = nlohmann::json;

/// Abstract interface for GitHub Issues API operations used by Store.
class IGitHubClient {
 public:
  virtual ~IGitHubClient() = default;

  /// Returns all open issue bodies with the given label.
  virtual std::vector<json> list_issues(std::string_view label) = 0;

  /// Creates a new issue; returns the issue number as a string.
  virtual std::string create_issue(std::string_view title, std::string_view body,
                                   std::string_view label) = 0;

  /// Replaces the body of an existing issue (identified by number string).
  virtual void update_issue_body(std::string_view issue_number, std::string_view body) = 0;

  /// Closes an issue (soft-delete).
  virtual void close_issue(std::string_view issue_number) = 0;
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
    auto it = seed_issues.find(std::string(label));
    if (it == seed_issues.end()) return {};
    return it->second;
  }

  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override {
    calls.push_back({"create_issue", std::string(title), std::string(body), std::string(label)});
    std::string num = std::to_string(++next_issue_number_);
    created_issues[num] = {{"title", std::string(title)},
                           {"body", std::string(body)},
                           {"label", std::string(label)}};
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

  std::vector<Call> calls;
  std::unordered_map<std::string, json> created_issues;
  std::unordered_map<std::string, std::string> updated_bodies;
  std::vector<std::string> closed_issues;

 private:
  int next_issue_number_{0};
};

/// Real GitHub client using libcurl to call the GitHub REST API v3.
/// Requires GITHUB_TOKEN env var and a repo in "owner/repo" format.
class CurlGitHubClient : public IGitHubClient {
 public:
  CurlGitHubClient(std::string repo, std::string token);
  ~CurlGitHubClient() override;

  std::vector<json> list_issues(std::string_view label) override;
  std::string create_issue(std::string_view title, std::string_view body,
                           std::string_view label) override;
  void update_issue_body(std::string_view issue_number, std::string_view body) override;
  void close_issue(std::string_view issue_number) override;

 private:
  std::string repo_;
  std::string token_;

  struct Response {
    long status{0};
    std::string body;
  };

  Response do_get(const std::string& url) const;
  Response do_post(const std::string& url, const std::string& payload) const;
  Response do_patch(const std::string& url, const std::string& payload) const;
};

}  // namespace projectagamemnon
