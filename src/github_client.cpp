#include "agamemnon/github_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <curl/curl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <set>
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

namespace agamemnon {
namespace {
constexpr std::size_t import_response_limit = 8 * 1024 * 1024;
constexpr std::size_t import_total_limit = 144 * 1024 * 1024;
constexpr std::size_t import_record_limit = 512 * 1024;
constexpr std::size_t fence_limit = 16 * 1024;

void require_import(bool condition) {
  if (!condition) throw std::runtime_error("GitHub import unavailable or uncertain");
}

bool lower_hex(std::string_view text, std::size_t size) {
  return text.size() == size && std::all_of(text.begin(), text.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool repository_part(std::string_view text) {
  return !text.empty() && text.size() <= 100 && text != "." && text != ".." &&
         std::all_of(text.begin(), text.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
         });
}

void require_repository(const std::string& repository) {
  const auto slash = repository.find('/');
  require_import(slash != std::string::npos &&
                 repository_part(std::string_view(repository).substr(0, slash)) &&
                 repository_part(std::string_view(repository).substr(slash + 1)));
}

void require_branch(const std::string& branch) {
  require_import(!branch.empty() && branch.size() <= 255 && branch.find("..") == std::string::npos);
  std::size_t start = 0;
  do {
    const auto end = branch.find('/', start);
    const auto part =
        std::string_view(branch).substr(start, end == std::string::npos ? end : end - start);
    require_import(repository_part(part) && part.front() != '.' && part.back() != '.' &&
                   !part.ends_with(".lock"));
    if (end == std::string::npos) break;
    start = end + 1;
  } while (true);
}

void require_fence(const std::string& branch, const std::string& key) {
  require_import(lower_hex(key, 64));
  require_branch(branch);
}

std::string url_segment(std::string_view text) {
  const char* hex = "0123456789ABCDEF";
  std::string result;
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.')
      result += c;
    else {
      result += '%';
      result += hex[c >> 4];
      result += hex[c & 15];
    }
  }
  return result;
}

json import_json(const std::string& body, ImportContext& context) {
  context.checkpoint();
  std::vector<std::set<std::string>> keys;
  try {
    auto result = json::parse(body, [&](int depth, json::parse_event_t event, json& value) {
      context.checkpoint();
      require_import(depth <= 64);
      if (event == json::parse_event_t::object_start) keys.emplace_back();
      if (event == json::parse_event_t::key)
        require_import(keys.back().insert(value.get<std::string>()).second);
      if (event == json::parse_event_t::object_end) keys.pop_back();
      return true;
    });
    context.checkpoint();
    return result;
  } catch (...) {
    throw std::runtime_error("GitHub import response invalid");
  }
}

std::string base64_encode(const std::string& body) {
  std::string result(4 * ((body.size() + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                  reinterpret_cast<const unsigned char*>(body.data()),
                  static_cast<int>(body.size()));
  return result;
}

std::string base64_decode(const std::string& body) {
  // GitHub wraps base64 in line breaks. No other non-alphabet bytes are accepted.
  require_import(body.size() <= 2 * fence_limit);
  std::string compact;
  for (char c : body)
    if (c != '\n' && c != '\r') compact += c;
  require_import(!compact.empty() && compact.size() % 4 == 0 &&
                 compact.size() <= 4 * ((fence_limit + 2) / 3));
  std::string result(compact.size() / 4 * 3, '\0');
  const auto count =
      EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                      reinterpret_cast<const unsigned char*>(compact.data()), compact.size());
  require_import(count >= 0);
  const auto padding = compact.ends_with("==") ? 2 : compact.ends_with("=") ? 1 : 0;
  require_import(count >= padding);
  result.resize(count - padding);
  require_import(result.size() <= fence_limit && base64_encode(result) == compact);
  return result;
}

struct ImportTransfer {
  ImportContext& context;
  std::string body;
  bool failed = false;
};

size_t import_body(char* bytes, size_t size, size_t count, void* pointer) noexcept {
  auto& transfer = *static_cast<ImportTransfer*>(pointer);
  try {
    require_import(size == 0 || count <= std::numeric_limits<std::size_t>::max() / size);
    const auto length = size * count;
    require_import(length <= import_response_limit - transfer.body.size());
    transfer.context.consume_body(length);
    transfer.body.append(bytes, length);
    return length;
  } catch (...) {
    transfer.failed = true;
    return 0;
  }
}

size_t import_header(char* bytes, size_t size, size_t count, void* pointer) noexcept {
  auto& transfer = *static_cast<ImportTransfer*>(pointer);
  try {
    transfer.context.checkpoint();
    require_import(size == 0 || count <= std::numeric_limits<std::size_t>::max() / size);
    std::string line(bytes, size * count);
    std::transform(line.begin(), line.end(), line.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (line.starts_with("content-encoding:")) {
      const auto value = line.substr(17);
      const auto first = value.find_first_not_of(" \t\r\n");
      const auto last = value.find_last_not_of(" \t\r\n");
      require_import(first != std::string::npos &&
                     value.substr(first, last - first + 1) == "identity");
    }
    return size * count;
  } catch (...) {
    transfer.failed = true;
    return 0;
  }
}

int import_progress(void* pointer, curl_off_t, curl_off_t received, curl_off_t,
                    curl_off_t) noexcept {
  auto& transfer = *static_cast<ImportTransfer*>(pointer);
  try {
    transfer.context.checkpoint();
    require_import(received <= static_cast<curl_off_t>(import_response_limit));
    return 0;
  } catch (...) {
    transfer.failed = true;
    return 1;
  }
}
}  // namespace

void validate_github_state_branch(const std::string& branch) { require_branch(branch); }

ImportContext::ImportContext(std::chrono::milliseconds budget) {
  require_import(budget.count() > 0 && budget <= std::chrono::seconds(30));
  deadline_ = std::chrono::steady_clock::now() + budget;
}

void ImportContext::checkpoint() const {
  require_import(std::chrono::steady_clock::now() < deadline_);
}

long ImportContext::remaining_ms() const {
  checkpoint();
  // A positive curl timeout is mandatory: zero would disable its deadline.
  return std::max<long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline_ - std::chrono::steady_clock::now())
                               .count());
}

void ImportContext::consume_body(std::size_t bytes) {
  checkpoint();
  require_import(bytes <= import_total_limit - received_);
  received_ += bytes;
}

CurlGitHubClient::CurlGitHubClient(std::string repo, std::string token, int loopback_port)
    : CurlGitHubClient(std::move(repo), std::move(token)) {
  require_import(loopback_port > 0 && loopback_port <= 65535);
  import_loopback_port_ = loopback_port;
}

bool CurlGitHubClient::import_resolver_supports_timeout() const {
  const auto* version = curl_version_info(CURLVERSION_NOW);
  return version && (version->features & CURL_VERSION_ASYNCHDNS) != 0;
}

CurlGitHubClient::Response CurlGitHubClient::import_request_(const char* method,
                                                             const std::string& path,
                                                             const std::string& payload,
                                                             ImportContext& context) const {
  context.checkpoint();
  // NOSIGNAL cannot bound a synchronous resolver's DNS lookup.
  require_import(import_resolver_supports_timeout());
  require_repository(repo_);
  require_import(!token_.empty() && token_.size() <= 4096 &&
                 std::none_of(token_.begin(), token_.end(),
                              [](unsigned char c) { return c < 33 || c == 127; }));
  require_import(payload.size() <= import_response_limit);
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  require_import(curl != nullptr);
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
  for (const auto& value :
       {std::string("Accept: application/vnd.github+json"),
        std::string("X-GitHub-Api-Version: 2022-11-28"),
        std::string("Content-Type: application/json"), std::string("User-Agent: Agamemnon/1.0"),
        "Authorization: Bearer " + token_}) {
    auto* next = curl_slist_append(headers.get(), value.c_str());
    require_import(next != nullptr);
    headers.release();
    headers.reset(next);
  }
  const std::string origin = import_loopback_port_ == 0
                                 ? "https://api.github.com"
                                 : "http://127.0.0.1:" + std::to_string(import_loopback_port_);
  const auto url = origin + path;
  ImportTransfer transfer{context, {}, false};
  const auto set = [&](auto option, auto value) {
    require_import(curl_easy_setopt(curl.get(), option, value) == CURLE_OK);
  };
  set(CURLOPT_URL, url.c_str());
  set(CURLOPT_HTTPHEADER, headers.get());
  set(CURLOPT_CUSTOMREQUEST, method);
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_TIMEOUT_MS, context.remaining_ms());
  set(CURLOPT_CONNECTTIMEOUT_MS, std::min(1000L, context.remaining_ms()));
  set(CURLOPT_FOLLOWLOCATION, 0L);
  set(CURLOPT_MAXREDIRS, 0L);
  set(CURLOPT_PROXY, "");
  set(CURLOPT_NOPROXY, "*");
  set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
  set(CURLOPT_SSL_VERIFYPEER, 1L);
  set(CURLOPT_SSL_VERIFYHOST, 2L);
  set(CURLOPT_ACCEPT_ENCODING, "identity");
  set(CURLOPT_HTTP_CONTENT_DECODING, 0L);
  set(CURLOPT_WRITEFUNCTION, &import_body);
  set(CURLOPT_WRITEDATA, &transfer);
  set(CURLOPT_HEADERFUNCTION, &import_header);
  set(CURLOPT_HEADERDATA, &transfer);
  set(CURLOPT_XFERINFOFUNCTION, &import_progress);
  set(CURLOPT_XFERINFODATA, &transfer);
  set(CURLOPT_NOPROGRESS, 0L);
  if (std::string_view(method) != "GET") {
    set(CURLOPT_POSTFIELDS, payload.data());
    set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
  }
  const auto code = curl_easy_perform(curl.get());
  context.checkpoint();
  require_import(code == CURLE_OK && !transfer.failed);
  long status = 0;
  require_import(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK);
  return {status, std::move(transfer.body), {}};
}

json CurlGitHubClient::import_graphql_(const char* query, const json& variables,
                                       ImportContext& context) const {
  const auto response = import_request_(
      "POST", "/graphql", json{{"query", query}, {"variables", variables}}.dump(), context);
  require_import(response.status == 200);
  const auto document = import_json(response.body, context);
  require_import(document.is_object() && document.value("data", json()).is_object() &&
                 (!document.contains("errors") || document["errors"].empty()));
  return document["data"];
}

json CurlGitHubClient::import_work_issue(const std::string& owner, const std::string& name,
                                         int number, ImportContext& context) {
  require_import(repository_part(owner) && repository_part(name) && number > 0);
  return import_graphql_(
      "query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name){id "
      "nameWithOwner issue(number:$number){__typename id number url state title body updatedAt}}}",
      {{"owner", owner}, {"name", name}, {"number", number}}, context);
}

json CurlGitHubClient::import_plan_comment(const std::string& id, ImportContext& context) {
  require_import(!id.empty() && id.size() <= 128);
  return import_graphql_(
      "query($id:ID!){node(id:$id){__typename ... on IssueComment{id body updatedAt issue{id}}}}",
      {{"id", id}}, context);
}

std::vector<json> CurlGitHubClient::import_list_issues(ImportContext& context) {
  std::vector<json> results;
  std::set<std::string> ids;
  std::set<int> numbers;
  for (int page = 1; page <= 27; ++page) {
    const auto response = import_request_(
        "GET",
        "/repos/" + repo_ +
            "/issues?state=all&labels=agamemnon-hmas-task&per_page=10&page=" + std::to_string(page),
        {}, context);
    require_import(response.status == 200);
    const auto records = import_json(response.body, context);
    require_import(records.is_array() && records.size() <= 10 &&
                   results.size() + records.size() <= 256);
    for (const auto& record : records) {
      context.checkpoint();
      require_import(
          record.is_object() && record.dump().size() <= import_record_limit &&
          record.value("node_id", json()).is_string() &&
          !record["node_id"].get_ref<const std::string&>().empty() &&
          record.value("number", json()).is_number_integer() && record["number"] > 0 &&
          record["number"] <= std::numeric_limits<int>::max() &&
          record.value("body", json()).is_string() &&
          (record.value("state", json()) == "open" || record.value("state", json()) == "closed") &&
          !record.contains("pull_request"));
      require_import(ids.insert(record["node_id"].get<std::string>()).second &&
                     numbers.insert(record["number"].get<int>()).second);
      results.push_back(record);
    }
    if (records.size() < 10) {
      context.checkpoint();
      return results;
    }
  }
  throw std::runtime_error("GitHub import enumeration incomplete");
}

std::optional<ImportFence> CurlGitHubClient::import_read_fence(const std::string& branch,
                                                               const std::string& key,
                                                               ImportContext& context) {
  require_fence(branch, key);
  return read_fence_(branch, "fleet/imports/" + key + ".json", context);
}

std::optional<ImportFence> CurlGitHubClient::build_read_fence(const std::string& branch,
                                                              ImportContext& context) {
  require_branch(branch);
  return read_fence_(branch, "fleet/build-admission/current.json", context);
}

std::optional<ImportFence> CurlGitHubClient::read_fence_(const std::string& branch,
                                                         const std::string& path,
                                                         ImportContext& context) {
  const auto response = import_request_(
      "GET", "/repos/" + repo_ + "/contents/" + path + "?ref=" + url_segment(branch), {}, context);
  if (response.status == 404) {
    const auto check =
        import_request_("GET", "/repos/" + repo_ + "/branches/" + url_segment(branch), {}, context);
    require_import(check.status == 200);
    const auto document = import_json(check.body, context);
    require_import(document.is_object() && document.value("name", json()) == branch &&
                   document.value("commit", json()).is_object() &&
                   document["commit"].value("sha", json()).is_string() &&
                   lower_hex(document["commit"]["sha"].get<std::string>(), 40));
    return std::nullopt;
  }
  require_import(response.status == 200);
  const auto data = import_json(response.body, context);
  require_import(data.is_object() && data.value("type", json()) == "file" &&
                 data.value("path", json()) == path && data.value("encoding", json()) == "base64" &&
                 data.value("sha", json()).is_string() &&
                 lower_hex(data["sha"].get<std::string>(), 40) &&
                 data.value("content", json()).is_string());
  const auto document = import_json(base64_decode(data["content"].get<std::string>()), context);
  require_import(document.is_object());
  return ImportFence{data["sha"].get<std::string>(), document};
}

ImportFence CurlGitHubClient::import_write_fence(const std::string& branch, const std::string& key,
                                                 const json& document,
                                                 const std::optional<std::string>& expected_sha,
                                                 ImportContext& context) {
  require_fence(branch, key);
  return write_fence_(branch, "fleet/imports/" + key + ".json", "Record Fleet import attempt",
                      document, expected_sha, context);
}

ImportFence CurlGitHubClient::build_write_fence(const std::string& branch, const json& document,
                                                const std::optional<std::string>& expected_sha,
                                                ImportContext& context) {
  require_branch(branch);
  return write_fence_(branch, "fleet/build-admission/current.json", "Record Fleet build attempt",
                      document, expected_sha, context);
}

ImportFence CurlGitHubClient::write_fence_(const std::string& branch, const std::string& path,
                                           const std::string& message, const json& document,
                                           const std::optional<std::string>& expected_sha,
                                           ImportContext& context) {
  require_import(document.is_object() && (!expected_sha || lower_hex(*expected_sha, 40)));
  const auto text = document.dump() + "\n";
  require_import(text.size() <= fence_limit);
  json payload{{"message", message}, {"branch", branch}, {"content", base64_encode(text)}};
  if (expected_sha) payload["sha"] = *expected_sha;
  const auto response =
      import_request_("PUT", "/repos/" + repo_ + "/contents/" + path, payload.dump(), context);
  require_import(response.status == 200 || response.status == 201);
  const auto ack = import_json(response.body, context);
  require_import(ack.is_object() && ack.value("content", json()).is_object() &&
                 ack["content"].value("path", json()) == path &&
                 ack["content"].value("sha", json()).is_string() &&
                 lower_hex(ack["content"]["sha"].get<std::string>(), 40) &&
                 ack.value("commit", json()).is_object() &&
                 ack["commit"].value("sha", json()).is_string() &&
                 lower_hex(ack["commit"]["sha"].get<std::string>(), 40));
  const auto observed = read_fence_(branch, path, context);
  require_import(observed && observed->sha == ack["content"]["sha"].get<std::string>() &&
                 observed->document.dump() + "\n" == text);
  return *observed;
}

std::string CurlGitHubClient::import_create_issue(std::string_view title, std::string_view body,
                                                  ImportContext& context) {
  require_import(!title.empty() && title.size() <= 256 && body.size() <= import_record_limit);
  const auto response = import_request_(
      "POST", "/repos/" + repo_ + "/issues",
      json{{"title", title}, {"body", body}, {"labels", json::array({"agamemnon-hmas-task"})}}
          .dump(),
      context);
  require_import(response.status == 201);
  const auto document = import_json(response.body, context);
  require_import(document.is_object() && document.value("number", json()).is_number_integer() &&
                 document["number"] > 0 && document["number"] <= std::numeric_limits<int>::max());
  return std::to_string(document["number"].get<int>());
}
}  // namespace agamemnon
