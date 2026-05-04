#include "projectagamemnon/github_client.hpp"

#include <curl/curl.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace projectagamemnon {

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* buf = static_cast<std::string*>(userdata);
  buf->append(ptr, size * nmemb);
  return size * nmemb;
}

}  // namespace

// ── CurlGitHubClient ─────────────────────────────────────────────────────────

CurlGitHubClient::CurlGitHubClient(std::string repo, std::string token)
    : repo_(std::move(repo)), token_(std::move(token)) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlGitHubClient::~CurlGitHubClient() { curl_global_cleanup(); }

CurlGitHubClient::Response CurlGitHubClient::do_get(const std::string& url) const {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl_easy_init failed");

  Response resp;
  struct curl_slist* headers = nullptr;
  std::string auth_header = "Authorization: Bearer " + token_;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
  headers = curl_slist_append(headers, auth_header.c_str());
  headers = curl_slist_append(headers, "User-Agent: ProjectAgamemnon/1.0");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    throw std::runtime_error(std::string("curl GET failed: ") + curl_easy_strerror(rc));

  return resp;
}

CurlGitHubClient::Response CurlGitHubClient::do_post(const std::string& url,
                                                      const std::string& payload) const {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl_easy_init failed");

  Response resp;
  struct curl_slist* headers = nullptr;
  std::string auth_header = "Authorization: Bearer " + token_;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
  headers = curl_slist_append(headers, auth_header.c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "User-Agent: ProjectAgamemnon/1.0");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    throw std::runtime_error(std::string("curl POST failed: ") + curl_easy_strerror(rc));

  return resp;
}

CurlGitHubClient::Response CurlGitHubClient::do_patch(const std::string& url,
                                                       const std::string& payload) const {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl_easy_init failed");

  Response resp;
  struct curl_slist* headers = nullptr;
  std::string auth_header = "Authorization: Bearer " + token_;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
  headers = curl_slist_append(headers, auth_header.c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "User-Agent: ProjectAgamemnon/1.0");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    throw std::runtime_error(std::string("curl PATCH failed: ") + curl_easy_strerror(rc));

  return resp;
}

std::vector<json> CurlGitHubClient::list_issues(std::string_view label) {
  std::vector<json> results;
  int page = 1;
  while (true) {
    std::string url = "https://api.github.com/repos/" + repo_ + "/issues?state=open&labels=" +
                      std::string(label) + "&per_page=100&page=" + std::to_string(page);
    Response resp;
    try {
      resp = do_get(url);
    } catch (const std::exception& e) {
      std::cerr << "[agamemnon] GitHub list_issues error: " << e.what() << "\n";
      break;
    }

    if (resp.status != 200) {
      std::cerr << "[agamemnon] GitHub list_issues HTTP " << resp.status << "\n";
      break;
    }

    json arr;
    try {
      arr = json::parse(resp.body);
    } catch (...) {
      std::cerr << "[agamemnon] GitHub list_issues: malformed JSON response\n";
      break;
    }

    if (!arr.is_array() || arr.empty()) break;

    for (auto& issue : arr) results.push_back(issue);

    if (static_cast<int>(arr.size()) < 100) break;
    ++page;
  }
  return results;
}

std::string CurlGitHubClient::create_issue(std::string_view title, std::string_view body,
                                            std::string_view label) {
  std::string url = "https://api.github.com/repos/" + repo_ + "/issues";
  json payload = {{"title", std::string(title)},
                  {"body", std::string(body)},
                  {"labels", json::array({std::string(label)})}};

  Response resp = do_post(url, payload.dump());
  if (resp.status != 201) {
    std::cerr << "[agamemnon] GitHub create_issue HTTP " << resp.status << ": " << resp.body
              << "\n";
    return "";
  }

  try {
    auto result = json::parse(resp.body);
    return std::to_string(result["number"].get<int>());
  } catch (...) {
    std::cerr << "[agamemnon] GitHub create_issue: malformed response\n";
    return "";
  }
}

void CurlGitHubClient::update_issue_body(std::string_view issue_number, std::string_view body) {
  std::string url =
      "https://api.github.com/repos/" + repo_ + "/issues/" + std::string(issue_number);
  json payload = {{"body", std::string(body)}};

  Response resp = do_patch(url, payload.dump());
  if (resp.status != 200) {
    std::cerr << "[agamemnon] GitHub update_issue_body HTTP " << resp.status << "\n";
  }
}

void CurlGitHubClient::close_issue(std::string_view issue_number) {
  std::string url =
      "https://api.github.com/repos/" + repo_ + "/issues/" + std::string(issue_number);
  json payload = {{"state", "closed"}};

  Response resp = do_patch(url, payload.dump());
  if (resp.status != 200) {
    std::cerr << "[agamemnon] GitHub close_issue HTTP " << resp.status << "\n";
  }
}

}  // namespace projectagamemnon
