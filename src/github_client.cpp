#include "projectagamemnon/github_client.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <iostream>

#include "httplib.h"

namespace projectagamemnon {

namespace {
constexpr const char* kGitHubHost = "api.github.com";
constexpr int kGitHubPort = 443;
constexpr int kPerPage = 100;
}  // namespace

GitHubClient::GitHubClient(Config cfg) : cfg_(std::move(cfg)) {
  enabled_ = !cfg_.token.empty() && !cfg_.owner.empty() && !cfg_.repo.empty();
}

// static
GitHubClient::Config GitHubClient::config_from_env() {
  Config cfg;
  if (const char* v = std::getenv("GITHUB_TOKEN")) cfg.token = v;
  if (const char* v = std::getenv("GITHUB_OWNER")) cfg.owner = v;
  if (const char* v = std::getenv("GITHUB_REPO")) cfg.repo = v;
  return cfg;
}

// ── Private HTTP helpers ─────────────────────────────────────────────────────

nlohmann::json GitHubClient::do_get(const std::string& path) {
  std::lock_guard<std::mutex> lk(http_mutex_);
  httplib::SSLClient cli(kGitHubHost, kGitHubPort);
  cli.set_follow_location(true);

  httplib::Headers headers = {
      {"Authorization", "Bearer " + cfg_.token},
      {"Accept", "application/vnd.github+json"},
      {"X-GitHub-Api-Version", "2022-11-28"},
      {"User-Agent", "ProjectAgamemnon"},
  };

  auto res = cli.Get(path, headers);
  if (!res || res->status < 200 || res->status >= 300) {
    int status = res ? res->status : -1;
    std::cerr << "[agamemnon/github] GET " << path << " failed: HTTP " << status << "\n";
    return nlohmann::json::array();
  }
  try {
    return nlohmann::json::parse(res->body);
  } catch (...) {
    std::cerr << "[agamemnon/github] GET " << path << " response parse error\n";
    return nlohmann::json::array();
  }
}

nlohmann::json GitHubClient::do_post(const std::string& path, const nlohmann::json& body) {
  std::lock_guard<std::mutex> lk(http_mutex_);
  httplib::SSLClient cli(kGitHubHost, kGitHubPort);
  cli.set_follow_location(true);

  httplib::Headers headers = {
      {"Authorization", "Bearer " + cfg_.token},
      {"Accept", "application/vnd.github+json"},
      {"X-GitHub-Api-Version", "2022-11-28"},
      {"User-Agent", "ProjectAgamemnon"},
  };

  std::string payload = body.dump();
  auto res = cli.Post(path, headers, payload, "application/json");
  if (!res || res->status < 200 || res->status >= 300) {
    int status = res ? res->status : -1;
    std::cerr << "[agamemnon/github] POST " << path << " failed: HTTP " << status << "\n";
    return nullptr;
  }
  try {
    return nlohmann::json::parse(res->body);
  } catch (...) {
    std::cerr << "[agamemnon/github] POST " << path << " response parse error\n";
    return nullptr;
  }
}

nlohmann::json GitHubClient::do_patch(const std::string& path, const nlohmann::json& body) {
  std::lock_guard<std::mutex> lk(http_mutex_);
  httplib::SSLClient cli(kGitHubHost, kGitHubPort);
  cli.set_follow_location(true);

  httplib::Headers headers = {
      {"Authorization", "Bearer " + cfg_.token},
      {"Accept", "application/vnd.github+json"},
      {"X-GitHub-Api-Version", "2022-11-28"},
      {"User-Agent", "ProjectAgamemnon"},
  };

  std::string payload = body.dump();
  auto res = cli.Patch(path, headers, payload, "application/json");
  if (!res || res->status < 200 || res->status >= 300) {
    int status = res ? res->status : -1;
    std::cerr << "[agamemnon/github] PATCH " << path << " failed: HTTP " << status << "\n";
    return nullptr;
  }
  try {
    return nlohmann::json::parse(res->body);
  } catch (...) {
    std::cerr << "[agamemnon/github] PATCH " << path << " response parse error\n";
    return nullptr;
  }
}

// ── Public API ───────────────────────────────────────────────────────────────

int GitHubClient::create_issue(const std::string& title,
                               const std::string& body,
                               const std::vector<std::string>& labels) {
  if (!enabled_) return -1;

  nlohmann::json payload;
  payload["title"] = title;
  payload["body"] = body;
  payload["labels"] = labels;

  std::string path = "/repos/" + cfg_.owner + "/" + cfg_.repo + "/issues";
  auto resp = do_post(path, payload);
  if (resp.is_null() || !resp.contains("number")) return -1;
  return resp["number"].get<int>();
}

bool GitHubClient::update_issue(int number,
                                const std::string& title,
                                const std::string& body,
                                const std::vector<std::string>& labels,
                                const std::string& state) {
  if (!enabled_) return false;

  nlohmann::json payload;
  payload["title"] = title;
  payload["body"] = body;
  payload["labels"] = labels;
  payload["state"] = state;

  std::string path =
      "/repos/" + cfg_.owner + "/" + cfg_.repo + "/issues/" + std::to_string(number);
  auto resp = do_patch(path, payload);
  return !resp.is_null();
}

nlohmann::json GitHubClient::list_issues(const std::string& label, const std::string& state) {
  if (!enabled_) return nlohmann::json::array();

  nlohmann::json result = nlohmann::json::array();
  int page = 1;

  while (true) {
    std::string path = "/repos/" + cfg_.owner + "/" + cfg_.repo + "/issues?state=" + state +
                       "&labels=" + label + "&per_page=" + std::to_string(kPerPage) +
                       "&page=" + std::to_string(page);

    auto page_result = do_get(path);
    if (!page_result.is_array() || page_result.empty()) break;

    for (auto& issue : page_result) result.push_back(issue);

    if (static_cast<int>(page_result.size()) < kPerPage) break;
    ++page;
  }

  return result;
}

}  // namespace projectagamemnon
