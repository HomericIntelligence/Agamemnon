#include "agamemnon/github_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <curl/curl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace agamemnon {

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* buf = static_cast<std::string*>(userdata);
  buf->append(ptr, size * nmemb);
  return size * nmemb;
}

/// Header callback — captures the value of the Retry-After header.
size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* retry_after = static_cast<std::string*>(userdata);
  std::string line(buffer, size * nitems);

  // Header lines look like "Retry-After: 30\r\n"
  static constexpr std::string_view kPrefix = "retry-after:";
  std::string lower = line;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower.rfind(kPrefix, 0) == 0) {
    std::string val = line.substr(kPrefix.size());
    // Strip leading/trailing whitespace and CRLF.
    auto start = val.find_first_not_of(" \t\r\n");
    auto end = val.find_last_not_of(" \t\r\n");
    if (start != std::string::npos) {
      *retry_after = val.substr(start, end - start + 1);
    }
  }
  return size * nitems;
}

/// Returns true when the HTTP status should be retried.
bool is_transient_status(long status) noexcept {
  // 429 Too Many Requests and all 5xx Server Errors are transient.
  return status == 429 || (status >= 500 && status < 600);
}

}  // namespace

// ── CurlGitHubClient ─────────────────────────────────────────────────────────

CurlGitHubClient::CurlGitHubClient(std::string repo, std::string token)
    : repo_(std::move(repo)), token_(std::move(token)) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlGitHubClient::~CurlGitHubClient() { curl_global_cleanup(); }

// ── with_retry ────────────────────────────────────────────────────────────────

// static
CurlGitHubClient::Response CurlGitHubClient::with_retry(const std::string& label,
                                                        const std::string& url,
                                                        std::function<Response()> op,
                                                        std::function<void(int)> sleep_fn) {
  // Default sleep implementation: real wall-clock sleep.
  if (!sleep_fn) {
    sleep_fn = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
  }

  int delay_ms = kBaseRetryMs;
  for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
    Response resp;
    bool transport_error = false;
    std::string transport_what;

    try {
      resp = op();
    } catch (const std::exception& e) {
      transport_error = true;
      transport_what = e.what();
    }

    if (transport_error) {
      if (attempt == kMaxRetries) {
        throw std::runtime_error(transport_what);
      }
      std::cerr << "[agamemnon] GitHub " << label << " " << url << " transport error (attempt "
                << attempt << "/" << kMaxRetries << "): " << transport_what << " — retrying in "
                << delay_ms << " ms\n";
      sleep_fn(delay_ms);
      delay_ms *= 2;
      continue;
    }

    if (!is_transient_status(resp.status)) {
      // Success or a non-retryable error (4xx other than 429).
      return resp;
    }

    if (attempt == kMaxRetries) {
      std::cerr << "[agamemnon] GitHub " << label << " " << url << " HTTP " << resp.status
                << " — all retries exhausted\n";
      return resp;
    }

    // Determine sleep duration: honor Retry-After if present, else exponential backoff.
    int sleep_ms = delay_ms;
    if (!resp.retry_after.empty()) {
      try {
        int secs = std::stoi(resp.retry_after);
        if (secs > 0) {
          sleep_ms = secs * 1000;
        }
      } catch (...) {
        // Ignore malformed Retry-After; fall back to backoff.
      }
    }

    std::cerr << "[agamemnon] GitHub " << label << " " << url << " HTTP " << resp.status
              << " (attempt " << attempt << "/" << kMaxRetries << ") — retrying in " << sleep_ms
              << " ms\n";
    sleep_fn(sleep_ms);
    delay_ms *= 2;
  }

  // Unreachable, but satisfies compiler.
  return {};
}

// ── do_get ────────────────────────────────────────────────────────────────────

CurlGitHubClient::Response CurlGitHubClient::do_get(const std::string& url) const {
  return CurlGitHubClient::with_retry("GET", url, [&]() -> Response {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    Response resp;
    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + token_;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "User-Agent: Agamemnon/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.retry_after);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
      throw std::runtime_error(std::string("curl GET failed: ") + curl_easy_strerror(rc));

    return resp;
  });
}

// ── do_post ───────────────────────────────────────────────────────────────────

CurlGitHubClient::Response CurlGitHubClient::do_post(const std::string& url,
                                                     const std::string& payload) const {
  // Creation and GraphQL mutations can commit before their response is lost.
  // Reconcile durable identity at the caller before any subsequent attempt.
  return do_post_once(url, payload);
}

CurlGitHubClient::Response CurlGitHubClient::do_post_once(const std::string& url,
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
  headers = curl_slist_append(headers, "User-Agent: Agamemnon/1.0");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.retry_after);

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    throw std::runtime_error(std::string("curl POST failed: ") + curl_easy_strerror(rc));

  return resp;
}

// ── do_patch ──────────────────────────────────────────────────────────────────

CurlGitHubClient::Response CurlGitHubClient::do_patch(const std::string& url,
                                                      const std::string& payload) const {
  return CurlGitHubClient::with_retry("PATCH", url, [&]() -> Response {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    Response resp;
    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + token_;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: Agamemnon/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.retry_after);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
      throw std::runtime_error(std::string("curl PATCH failed: ") + curl_easy_strerror(rc));

    return resp;
  });
}

// ── Public API ────────────────────────────────────────────────────────────────

std::vector<json> CurlGitHubClient::list_issues(std::string_view label) {
  return list_issues_(label, "open");
}

std::vector<json> CurlGitHubClient::list_issues_including_closed(std::string_view label) {
  return list_issues_(label, "all");
}

std::vector<json> CurlGitHubClient::list_issues_(std::string_view label, std::string_view state) {
  std::vector<json> results;
  int page = 1;
  while (true) {
    std::string url = "https://api.github.com/repos/" + repo_ +
                      "/issues?state=" + std::string(state) + "&labels=" + std::string(label) +
                      "&per_page=100&page=" + std::to_string(page);
    Response resp = do_get(url);

    if (resp.status != 200) {
      throw std::runtime_error("GitHub list_issues HTTP " + std::to_string(resp.status));
    }

    json arr;
    try {
      arr = json::parse(resp.body);
    } catch (...) {
      throw std::runtime_error("GitHub list_issues: malformed JSON response");
    }

    if (!arr.is_array()) throw std::runtime_error("GitHub list_issues: expected array");
    if (arr.empty()) break;

    for (auto& issue : arr) results.push_back(issue);

    if (static_cast<int>(arr.size()) < 100) break;
    ++page;
  }
  return results;
}

json CurlGitHubClient::graphql(const std::string& query, const json& variables) {
  const auto response = do_post("https://api.github.com/graphql",
                                json{{"query", query}, {"variables", variables}}.dump());
  if (response.status != 200) throw std::runtime_error("GitHub GraphQL transport failed");
  const auto document = json::parse(response.body, nullptr, false);
  if (!document.is_object() || !document.contains("data") || !document["data"].is_object() ||
      (document.contains("errors") && !document["errors"].empty()))
    throw std::runtime_error("GitHub GraphQL response was not acknowledged");
  return document["data"];
}

std::string CurlGitHubClient::create_issue(std::string_view title, std::string_view body,
                                           std::string_view label) {
  std::string url = "https://api.github.com/repos/" + repo_ + "/issues";
  json payload = {{"title", std::string(title)},
                  {"body", std::string(body)},
                  {"labels", json::array({std::string(label)})}};

  Response resp = do_post(url, payload.dump());
  if (resp.status != 201) {
    throw std::runtime_error("GitHub create_issue HTTP " + std::to_string(resp.status));
  }

  try {
    auto result = json::parse(resp.body);
    int number = result.at("number").get<int>();
    if (number <= 0) throw std::runtime_error("invalid issue number");
    return std::to_string(number);
  } catch (...) {
    throw std::runtime_error("GitHub create_issue: malformed response");
  }
}

void CurlGitHubClient::update_issue_body(std::string_view issue_number, std::string_view body) {
  std::string url =
      "https://api.github.com/repos/" + repo_ + "/issues/" + std::string(issue_number);
  json payload = {{"body", std::string(body)}};

  Response resp = do_patch(url, payload.dump());
  if (resp.status != 200) {
    throw std::runtime_error("GitHub update_issue_body HTTP " + std::to_string(resp.status));
  }
}

void CurlGitHubClient::close_issue(std::string_view issue_number) {
  std::string url =
      "https://api.github.com/repos/" + repo_ + "/issues/" + std::string(issue_number);
  json payload = {{"state", "closed"}};

  Response resp = do_patch(url, payload.dump());
  if (resp.status != 200) {
    throw std::runtime_error("GitHub close_issue HTTP " + std::to_string(resp.status));
  }
}

}  // namespace agamemnon
