#include "agamemnon/auth.hpp"
#include "agamemnon/fleet_issue.hpp"
#include "agamemnon/github_client.hpp"
#include "agamemnon/store.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <openssl/evp.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "httplib.h"
#include <gtest/gtest.h>

// Tests for CurlGitHubClient::with_retry().
//
// All tests inject a no-op sleep function so the suite runs in milliseconds
// without requiring a live network connection or GitHub token.

namespace agamemnon::test {

using Response = CurlGitHubClient::Response;

// Helper: build a no-op sleep function and a counter of how many times it was called.
struct SleepSpy {
  int calls{0};
  std::vector<int> delays;

  std::function<void(int)> fn() {
    return [this](int ms) {
      ++calls;
      delays.push_back(ms);
    };
  }
};

// ── Transient-status classification ──────────────────────────────────────────

TEST(GitHubClientRetryTest, SuccessOnFirstAttemptNoRetry) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "GET", "https://example.com",
      [&]() -> Response {
        ++call_count;
        return {200, R"({"ok":true})", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 200);
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(spy.calls, 0) << "No sleep should occur on first-attempt success";
}

// ── 5xx retry (happy path: fails twice, succeeds on third) ───────────────────

TEST(GitHubClientRetryTest, RetriesOn5xxAndEventuallySucceeds) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "POST", "https://example.com/issues",
      [&]() -> Response {
        ++call_count;
        if (call_count < 3) return {503, "Service Unavailable", ""};
        return {201, R"({"number":42})", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 201);
  EXPECT_EQ(call_count, 3);
  EXPECT_EQ(spy.calls, 2) << "Two sleeps for two retries";
}

// ── 5xx error path: all retries exhausted ────────────────────────────────────

TEST(GitHubClientRetryTest, ReturnsLastResponseAfterAllRetriesExhausted) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "PATCH", "https://example.com/issues/1",
      [&]() -> Response {
        ++call_count;
        return {500, "Internal Server Error", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 500);
  EXPECT_EQ(call_count, CurlGitHubClient::kMaxRetries);
  EXPECT_EQ(spy.calls, CurlGitHubClient::kMaxRetries - 1);
}

// ── Transport-error retry (happy path: fails once, then succeeds) ─────────────

TEST(GitHubClientRetryTest, RetriesOnTransportErrorAndSucceeds) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "GET", "https://example.com",
      [&]() -> Response {
        ++call_count;
        if (call_count == 1) throw std::runtime_error("curl GET failed: Could not resolve host");
        return {200, R"([])", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 200);
  EXPECT_EQ(call_count, 2);
  EXPECT_EQ(spy.calls, 1);
}

// ── Transport-error path: all retries exhausted → throws ─────────────────────

TEST(GitHubClientRetryTest, ThrowsAfterAllTransportRetriesExhausted) {
  SleepSpy spy;
  int call_count = 0;

  EXPECT_THROW(CurlGitHubClient::with_retry(
                   "GET", "https://example.com",
                   [&]() -> Response {
                     ++call_count;
                     throw std::runtime_error("connection refused");
                   },
                   spy.fn()),
               std::runtime_error);

  EXPECT_EQ(call_count, CurlGitHubClient::kMaxRetries);
  EXPECT_EQ(spy.calls, CurlGitHubClient::kMaxRetries - 1);
}

// ── 4xx (other than 429) is NOT retried ──────────────────────────────────────

TEST(GitHubClientRetryTest, DoesNotRetryOn4xx) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "POST", "https://example.com/issues",
      [&]() -> Response {
        ++call_count;
        return {422, "Unprocessable Entity", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 422);
  EXPECT_EQ(call_count, 1) << "4xx (non-429) must not be retried";
  EXPECT_EQ(spy.calls, 0) << "No sleep should occur for non-retryable errors";
}

TEST(GitHubClientRetryTest, DoesNotRetryOn401) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "GET", "https://example.com",
      [&]() -> Response {
        ++call_count;
        return {401, "Unauthorized", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 401);
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(spy.calls, 0);
}

TEST(GitHubClientRetryTest, DoesNotRetryOn404) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "GET", "https://example.com/issues/9999",
      [&]() -> Response {
        ++call_count;
        return {404, "Not Found", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 404);
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(spy.calls, 0);
}

// ── 429 with Retry-After header honored ──────────────────────────────────────

TEST(GitHubClientRetryTest, Honors429WithRetryAfterHeader) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "POST", "https://example.com/issues",
      [&]() -> Response {
        ++call_count;
        if (call_count == 1) return {429, "Too Many Requests", "5"};
        return {201, R"({"number":7})", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 201);
  EXPECT_EQ(call_count, 2);
  ASSERT_EQ(spy.delays.size(), 1u);
  EXPECT_EQ(spy.delays[0], 5000) << "Retry-After:5 should sleep 5000 ms";
}

// ── 429 without Retry-After falls back to exponential backoff ────────────────

TEST(GitHubClientRetryTest, FallsBackToExponentialBackoffOn429WithoutRetryAfter) {
  SleepSpy spy;
  int call_count = 0;

  Response result = CurlGitHubClient::with_retry(
      "GET", "https://example.com",
      [&]() -> Response {
        ++call_count;
        if (call_count < 3) return {429, "Too Many Requests", ""};
        return {200, R"([])", ""};
      },
      spy.fn());

  EXPECT_EQ(result.status, 200);
  ASSERT_EQ(spy.delays.size(), 2u);
  EXPECT_EQ(spy.delays[0], CurlGitHubClient::kBaseRetryMs);
  EXPECT_EQ(spy.delays[1], CurlGitHubClient::kBaseRetryMs * 2);
}

// ── Exponential-backoff schedule verification ─────────────────────────────────

TEST(GitHubClientRetryTest, ExponentialBackoffScheduleIsDoubling) {
  SleepSpy spy;

  // Trigger all kMaxRetries-1 sleeps by failing with 503 until the last attempt succeeds.
  int call_count = 0;
  CurlGitHubClient::with_retry(
      "GET", "https://example.com",
      [&]() -> Response {
        ++call_count;
        if (call_count < CurlGitHubClient::kMaxRetries) return {503, "", ""};
        return {200, "", ""};
      },
      spy.fn());

  ASSERT_EQ(static_cast<int>(spy.delays.size()), CurlGitHubClient::kMaxRetries - 1);
  for (int i = 0; i < static_cast<int>(spy.delays.size()); ++i) {
    int expected = CurlGitHubClient::kBaseRetryMs * (1 << i);
    EXPECT_EQ(spy.delays[i], expected) << "delay[" << i << "] should be " << expected << " ms";
  }
}

}  // namespace agamemnon::test

namespace agamemnon::test {
namespace {
using namespace std::chrono_literals;
const std::string fence_key(64, 'a');
const std::string fence_sha(40, 'b');
const std::string fence_path = "fleet/imports/" + fence_key + ".json";
const std::string contents_path = "/repos/fixture/state/contents/" + fence_path;
const std::string build_fence_path = "fleet/build-admission/current.json";
const std::string build_contents_path = "/repos/fixture/state/contents/" + build_fence_path;

std::string encoded(const std::string& value) {
  std::string result(4 * ((value.size() + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                  reinterpret_cast<const unsigned char*>(value.data()), value.size());
  return result;
}

json contents(const json& document, const std::string& sha = fence_sha) {
  return {{"type", "file"},
          {"path", fence_path},
          {"sha", sha},
          {"encoding", "base64"},
          {"content", encoded(document.dump() + "\n")}};
}

json build_contents(const json& document) {
  auto result = contents(document);
  result["path"] = build_fence_path;
  return result;
}

json issue(int number, std::string body = "fixture") {
  return {{"id", number},
          {"node_id", "I_fixture_" + std::to_string(number)},
          {"number", number},
          {"state", number % 2 ? "open" : "closed"},
          {"body", body}};
}

class LoopbackImportClient : public CurlGitHubClient {
 public:
  explicit LoopbackImportClient(int port)
      : CurlGitHubClient("fixture/state", "fixture-key", port) {}
};
}  // namespace

class GitHubImportTransport : public ::testing::Test {
 protected:
  httplib::Server server;
  std::thread listener;
  std::unique_ptr<LoopbackImportClient> client;
  std::atomic<int> calls{0};
  int port = 0;

  void SetUp() override {
    server.new_task_queue = [] { return new httplib::ThreadPool(2); };
    port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    client = std::make_unique<LoopbackImportClient>(port);
  }
  void start() {
    listener = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
  }
  void TearDown() override {
    server.stop();
    if (listener.joinable()) listener.join();
  }
  void pages(const std::vector<json>& records) {
    server.Get("/repos/fixture/state/issues", [this, records](const auto& req, auto& res) {
      ++calls;
      EXPECT_EQ(req.get_param_value("state"), "all");
      EXPECT_EQ(req.get_param_value("labels"), "agamemnon-hmas-task");
      EXPECT_EQ(req.get_param_value("per_page"), "10");
      const auto page = std::stoi(req.get_param_value("page"));
      json result = json::array();
      for (std::size_t n = (page - 1) * 10; n < records.size() && n < page * 10u; ++n)
        result.push_back(records[n]);
      const auto encoded_page = result.dump();
      EXPECT_LE(encoded_page.size(), 8u * 1024 * 1024);
      res.set_content(encoded_page, "application/json");
    });
  }
};

class GitHubBuildFenceTransport : public GitHubImportTransport {};

TEST_F(GitHubBuildFenceTransport, ConditionalWriteUsesFixedPathAndExactReadback) {
  const json document{{"phase", "creating"}, {"identity", {{"generation", 1}}}};
  std::atomic<int> reads{0};
  server.Put(build_contents_path, [&](const auto& req, auto& res) {
    const int write = ++calls;
    const auto payload = json::parse(req.body);
    EXPECT_EQ(req.get_header_value("Authorization"), "Bearer fixture-key");
    EXPECT_EQ(payload.at("branch"), "fleet/state");
    EXPECT_EQ(payload.at("content"), encoded(document.dump() + "\n"));
    if (write == 1)
      EXPECT_FALSE(payload.contains("sha"));
    else
      EXPECT_EQ(payload.at("sha"), fence_sha);
    res.status = write == 1 ? 201 : 200;
    res.set_content(json{{"content", {{"path", build_fence_path}, {"sha", fence_sha}}},
                         {"commit", {{"sha", std::string(40, 'c')}}}}
                        .dump(),
                    "application/json");
  });
  server.Get(build_contents_path, [&](const auto& req, auto& res) {
    ++reads;
    EXPECT_EQ(req.get_param_value("ref"), "fleet/state");
    res.set_content(build_contents(document).dump(), "application/json");
  });
  start();
  IGitHubClient& transport = *client;
  for (const auto& previous :
       {std::optional<std::string>{}, std::optional<std::string>{fence_sha}}) {
    ImportContext context;
    const auto result = transport.build_write_fence("fleet/state", document, previous, context);
    EXPECT_EQ(result.sha, fence_sha);
    EXPECT_EQ(result.document.dump(), document.dump());
    EXPECT_TRUE(result.document.at("identity").at("generation").is_number_integer());
  }
  ImportContext context;
  const auto retained = transport.build_read_fence("fleet/state", context);
  ASSERT_TRUE(retained.has_value());
  EXPECT_EQ(retained->sha, fence_sha);
  EXPECT_EQ(retained->document.dump(), document.dump());
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(reads, 3);
}

TEST_F(GitHubBuildFenceTransport, AbsenceRequiresAnExactExistingBranch) {
  struct BranchCase {
    const char* name;
    int status;
    json response;
    bool absent;
  };
  const std::vector<BranchCase> cases{
      {"ExistingBranch", 200, {{"name", "main"}, {"commit", {{"sha", fence_sha}}}}, true},
      {"MissingBranch", 404, json::object(), false},
      {"UnavailableBranch", 503, json::object(), false},
      {"WrongBranch", 200, {{"name", "other"}, {"commit", {{"sha", fence_sha}}}}, false},
      {"InvalidCommit", 200, {{"name", "main"}, {"commit", {{"sha", "invalid"}}}}, false}};
  std::atomic<std::size_t> selected{0};
  std::atomic<int> branches{0};
  server.Get(build_contents_path, [&](const auto& req, auto& res) {
    ++calls;
    EXPECT_EQ(req.get_param_value("ref"), "main");
    res.status = 404;
  });
  server.Get("/repos/fixture/state/branches/main", [&](const auto&, auto& res) {
    ++branches;
    const auto& value = cases.at(selected.load());
    res.status = value.status;
    res.set_content(value.response.dump(), "application/json");
  });
  start();
  for (std::size_t index = 0; index < cases.size(); ++index) {
    SCOPED_TRACE(cases[index].name);
    selected = index;
    calls = 0;
    branches = 0;
    ImportContext context;
    if (cases[index].absent)
      EXPECT_EQ(client->build_read_fence("main", context), std::nullopt);
    else
      EXPECT_THROW(client->build_read_fence("main", context), std::runtime_error);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(branches, 1);
  }
}

TEST_F(GitHubBuildFenceTransport, RejectsInvalidBranchesBeforeAnyRequest) {
  server.Get("/repos/fixture/state/.*", [&](const auto&, auto& res) {
    ++calls;
    res.status = 503;
  });
  server.Put("/repos/fixture/state/.*", [&](const auto&, auto& res) {
    ++calls;
    res.status = 503;
  });
  start();
  const std::vector<std::string> invalid{
      "",
      "/main",
      "main/",
      "main//state",
      "main..state",
      ".main",
      "main.",
      "main.lock",
      "main?ref=other",
      "main#fragment",
      "main:other",
      std::string(101, 'a'),
      std::string(100, 'a') + "/" + std::string(100, 'b') + "/" + std::string(54, 'c'),
      std::string("main\0other", 10)};
  for (const auto& branch : invalid) {
    SCOPED_TRACE(branch);
    ImportContext read;
    EXPECT_THROW(client->build_read_fence(branch, read), std::runtime_error);
    ImportContext write;
    EXPECT_THROW(
        client->build_write_fence(branch, json{{"phase", "creating"}}, std::nullopt, write),
        std::runtime_error);
  }
  EXPECT_EQ(calls, 0);
}

TEST_F(GitHubBuildFenceTransport, RejectsWriteAndReadbackFailuresWithoutRetry) {
  const json document{{"phase", "creating"}, {"identity", {{"generation", 1}}}};
  const json acknowledged{{"content", {{"path", build_fence_path}, {"sha", fence_sha}}},
                          {"commit", {{"sha", std::string(40, 'c')}}}};
  const json retained = build_contents(document);
  struct Failure {
    const char* name;
    int write_status;
    json acknowledgment;
    json readback;
    int expected_reads;
  };
  std::vector<Failure> cases;
  for (const auto& [name, status] : std::vector<std::pair<const char*, int>>{
           {"Redirect", 302}, {"Conflict", 409}, {"RateLimit", 429}, {"Unavailable", 503}})
    cases.push_back({name, status, acknowledged, retained, 0});
  auto wrong_path = acknowledged;
  wrong_path["content"]["path"] = fence_path;
  auto invalid_sha = acknowledged;
  invalid_sha["content"]["sha"] = std::string(39, 'b');
  auto invalid_commit = acknowledged;
  invalid_commit["commit"]["sha"] = std::string(40, 'G');
  auto missing_commit = acknowledged;
  missing_commit.erase("commit");
  for (const auto& [name, response] :
       std::vector<std::pair<const char*, json>>{{"WrongAcknowledgmentPath", wrong_path},
                                                 {"InvalidAcknowledgmentSha", invalid_sha},
                                                 {"InvalidCommitSha", invalid_commit},
                                                 {"MissingCommit", missing_commit}})
    cases.push_back({name, 201, response, retained, 0});
  auto different_sha = retained;
  different_sha["sha"] = std::string(40, 'd');
  auto malformed_sha = retained;
  malformed_sha["sha"] = 1;
  auto wrong_read_path = retained;
  wrong_read_path["path"] = fence_path;
  auto changed_document = retained;
  changed_document["content"] = encoded(json{{"phase", "prepared"}}.dump() + "\n");
  auto changed_type = document;
  changed_type["identity"]["generation"] = 1.0;
  auto invalid_encoding = retained;
  invalid_encoding["content"] = "!!!=";
  auto nonobject = retained;
  nonobject["content"] = encoded("[]");
  auto symlink = retained;
  symlink["type"] = "symlink";
  for (const auto& [name, response] : std::vector<std::pair<const char*, json>>{
           {"ReadbackShaMismatch", different_sha},
           {"MalformedReadbackSha", malformed_sha},
           {"WrongReadbackPath", wrong_read_path},
           {"ChangedDocument", changed_document},
           {"ChangedNumericType", build_contents(changed_type)},
           {"InvalidBase64", invalid_encoding},
           {"NonobjectDocument", nonobject},
           {"LinkedContents", symlink}})
    cases.push_back({name, 201, acknowledged, response, 1});
  std::atomic<std::size_t> selected{0};
  std::atomic<int> reads{0};
  std::atomic<int> redirects{0};
  server.Put(build_contents_path, [&](const auto&, auto& res) {
    ++calls;
    const auto& value = cases.at(selected.load());
    res.status = value.write_status;
    res.set_header("Retry-After", "30");
    res.set_header("Location", "http://127.0.0.1:" + std::to_string(port) + "/elsewhere");
    res.set_content(value.acknowledgment.dump(), "application/json");
  });
  server.Get(build_contents_path, [&](const auto&, auto& res) {
    ++reads;
    res.set_content(cases.at(selected.load()).readback.dump(), "application/json");
  });
  server.Get("/elsewhere", [&](const auto&, auto& res) {
    ++redirects;
    res.set_content("{}", "application/json");
  });
  start();
  for (std::size_t index = 0; index < cases.size(); ++index) {
    SCOPED_TRACE(cases[index].name);
    selected = index;
    calls = 0;
    reads = 0;
    ImportContext context{1s};
    EXPECT_THROW(client->build_write_fence("main", document, std::nullopt, context),
                 std::runtime_error);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(reads, cases[index].expected_reads);
    EXPECT_EQ(redirects, 0);
  }
}

TEST_F(GitHubBuildFenceTransport, BoundsDocumentsAndRejectsInvalidExpectedShaBeforeWrite) {
  constexpr std::size_t limit = 16 * 1024;
  const json empty{{"padding", ""}};
  const json document{{"padding", std::string(limit - empty.dump().size() - 1, 'x')}};
  ASSERT_EQ(document.dump().size() + 1, limit);
  std::atomic<int> reads{0};
  std::atomic<bool> enlarged{false};
  server.Put(build_contents_path, [&](const auto& req, auto& res) {
    ++calls;
    EXPECT_EQ(json::parse(req.body).at("content"), encoded(document.dump() + "\n"));
    res.status = 201;
    res.set_content(json{{"content", {{"path", build_fence_path}, {"sha", fence_sha}}},
                         {"commit", {{"sha", std::string(40, 'c')}}}}
                        .dump(),
                    "application/json");
  });
  server.Get(build_contents_path, [&](const auto&, auto& res) {
    ++reads;
    auto response = document;
    if (enlarged) response["padding"] = document.at("padding").get<std::string>() + "x";
    res.set_content(build_contents(response).dump(), "application/json");
  });
  start();
  ImportContext context;
  EXPECT_EQ(client->build_write_fence("main", document, std::nullopt, context).document, document);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(reads, 1);
  auto oversized = document;
  oversized["padding"] = document.at("padding").get<std::string>() + "x";
  EXPECT_THROW(client->build_write_fence("main", oversized, std::nullopt, context),
               std::runtime_error);
  EXPECT_THROW(client->build_write_fence("main", json::array(), std::nullopt, context),
               std::runtime_error);
  for (const auto& sha : {std::string(), std::string(39, 'a'), std::string(41, 'a'),
                          std::string(40, 'A'), std::string(40, 'g')}) {
    SCOPED_TRACE(sha);
    EXPECT_THROW(client->build_write_fence("main", document, sha, context), std::runtime_error);
  }
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(reads, 1);
  enlarged = true;
  ImportContext read;
  EXPECT_THROW(client->build_read_fence("main", read), std::runtime_error);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(reads, 2);
}

TEST(GitHubImportContext, RejectsInvalidBudgetAndEnforcesSharedDeadlineAndBytes) {
  EXPECT_THROW(ImportContext{0ms}, std::runtime_error);
  EXPECT_THROW(ImportContext{-1ms}, std::runtime_error);
  EXPECT_THROW(ImportContext{30001ms}, std::runtime_error);
  ImportContext context{20ms};
  EXPECT_GT(context.remaining_ms(), 0);
  EXPECT_LE(context.remaining_ms(), 20);
  std::this_thread::sleep_for(25ms);
  EXPECT_THROW(context.checkpoint(), std::runtime_error);
  ImportContext bytes;
  EXPECT_NO_THROW(bytes.consume_body(144u * 1024 * 1024));
  EXPECT_THROW(bytes.consume_body(1), std::runtime_error);
}

TEST_F(GitHubImportTransport, Reads108ActualSerializedLargeHmasRecords) {
  auto backing = std::make_shared<MockGitHubClient>();
  Store store{backing};
  std::vector<json> records;
  std::string text;
  while (text.size() < 60000) text += "quoted\" slash\\ control\n multibyte é ";
  for (int n = 1; n <= 108; ++n) {
    HmasTask task{};
    task.id = "cohort-" + std::to_string(n);
    task.layer = HmasLayer::L3_TaskAgent;
    task.state = TaskState::Pending;
    task.subject = "Existing work";
    task.description = text;
    task.repo = "fixture/work";
    store.create_hmas_task(task);
    const auto body = backing->created_issues.at(std::to_string(n)).at("body").get<std::string>();
    ASSERT_GE(body.size(), 60000u);
    ASSERT_NE(body.find(hmas_task_to_json(task).dump(2)), std::string::npos);
    records.push_back(issue(n, body));
    ASSERT_LE(records.back().dump().size(), 512u * 1024);
  }
  pages(records);
  start();
  ImportContext context;
  EXPECT_EQ(client->import_list_issues(context), records);
  EXPECT_EQ(calls, 11);
}

TEST_F(GitHubImportTransport, ExactMultipleRequiresEmptyTerminalPage) {
  std::vector<json> records;
  for (int n = 1; n <= 10; ++n) records.push_back(issue(n));
  pages(records);
  start();
  ImportContext context;
  EXPECT_EQ(client->import_list_issues(context), records);
  EXPECT_EQ(calls, 2);
}

TEST_F(GitHubImportTransport, Supports256AndRejects257WithoutFalseAbsence) {
  std::atomic<int> count{256};
  server.Get("/repos/fixture/state/issues", [&](const auto& req, auto& res) {
    ++calls;
    const int page = std::stoi(req.get_param_value("page"));
    json result = json::array();
    for (int n = (page - 1) * 10 + 1; n <= count && n <= page * 10; ++n) result.push_back(issue(n));
    res.set_content(result.dump(), "application/json");
  });
  start();
  ImportContext first;
  EXPECT_EQ(client->import_list_issues(first).size(), 256u);
  EXPECT_EQ(calls, 26);
  count = 257;
  calls = 0;
  ImportContext second;
  EXPECT_THROW(client->import_list_issues(second), std::runtime_error);
  EXPECT_EQ(calls, 26);
}

TEST_F(GitHubImportTransport, RejectsRepeatedPagesMalformedAndOversizedRecords) {
  std::atomic<int> mode{0};
  server.Get("/repos/fixture/state/issues", [&](const auto&, auto& res) {
    ++calls;
    json result = json::array();
    if (mode == 0)
      for (int n = 1; n <= 10; ++n) result.push_back(issue(n));
    if (mode == 1) result.push_back({{"body", "missing identity"}});
    if (mode == 2) result.push_back(issue(1, std::string(512 * 1024, 'x')));
    if (mode == 3)
      for (int n = 1; n <= 11; ++n) result.push_back(issue(n));
    res.set_content(mode == 4 ? "{" : result.dump(), "application/json");
  });
  start();
  for (int n = 0; n < 5; ++n) {
    mode = n;
    calls = 0;
    ImportContext context;
    EXPECT_THROW(client->import_list_issues(context), std::runtime_error) << n;
    EXPECT_EQ(calls, n == 0 ? 2 : 1);
  }
}

TEST_F(GitHubImportTransport, FixedQueriesBindVariablesAndReturnOnlyData) {
  server.Post("/graphql", [&](const auto& req, auto& res) {
    ++calls;
    EXPECT_EQ(req.get_header_value("Authorization"), "Bearer fixture-key");
    const auto payload = json::parse(req.body);
    const auto query = payload.at("query").template get<std::string>();
    if (calls == 1) {
      EXPECT_EQ(payload.at("variables"),
                (json{{"owner", "fixture"}, {"name", "work"}, {"number", 42}}));
      EXPECT_NE(query.find("repository(owner:$owner,name:$name)"), std::string::npos);
      EXPECT_NE(query.find("__typename"), std::string::npos);
      EXPECT_NE(query.find("nameWithOwner"), std::string::npos);
    } else {
      EXPECT_EQ(payload.at("variables"), (json{{"id", "IC_opaque\"value"}}));
      EXPECT_EQ(query.find("IC_opaque"), std::string::npos);
      EXPECT_NE(query.find("... on IssueComment"), std::string::npos);
      EXPECT_NE(query.find("issue{id}"), std::string::npos);
    }
    res.set_content(json{{"data", {{"fixture", true}}}}.dump(), "application/json");
  });
  start();
  ImportContext context;
  EXPECT_EQ(client->import_work_issue("fixture", "work", 42, context), (json{{"fixture", true}}));
  EXPECT_EQ(client->import_plan_comment("IC_opaque\"value", context), (json{{"fixture", true}}));
  EXPECT_EQ(calls, 2);
  EXPECT_THROW(client->import_work_issue("../bad", "work", 42, context), std::runtime_error);
  EXPECT_THROW(client->import_work_issue("fixture", "work", 0, context), std::runtime_error);
  EXPECT_EQ(calls, 2);
}

TEST_F(GitHubImportTransport, RefusesRedirectRetryAndGraphqlPartialErrors) {
  std::atomic<int> status{302};
  std::atomic<int> redirected{0};
  server.Get("/elsewhere", [&](const auto&, auto& res) {
    ++redirected;
    res.set_content("{}", "application/json");
  });
  server.Post("/graphql", [&](const auto&, auto& res) {
    ++calls;
    res.status = status;
    res.set_header("Location", "http://127.0.0.1:" + std::to_string(port) + "/elsewhere");
    res.set_header("Retry-After", "30");
    res.set_content(R"({"data":{},"errors":[{"message":"PRIVATE-UPSTREAM"}]})", "application/json");
  });
  start();
  for (int code : {302, 429, 503, 200}) {
    status = code;
    ImportContext context{1s};
    try {
      client->import_plan_comment("IC_1", context);
      FAIL();
    } catch (const std::runtime_error& error) {
      EXPECT_EQ(std::string(error.what()).find("PRIVATE-UPSTREAM"), std::string::npos);
    }
  }
  EXPECT_EQ(calls, 4);
  EXPECT_EQ(redirected, 0);
}

TEST_F(GitHubImportTransport, SharesDeadlineAcrossRequestsAndCapsBodyWhileStreaming) {
  const auto mode = std::make_shared<std::atomic<int>>(0);
  server.Post("/graphql", [this, mode](const auto&, auto& res) {
    ++calls;
    if (mode->load() == 0) std::this_thread::sleep_for(100ms);
    res.set_content(mode->load() == 1 ? std::string(8 * 1024 * 1024 + 1, 'x') : R"({"data":{}})",
                    "application/json");
  });
  start();
  ImportContext deadline{150ms};
  EXPECT_NO_THROW(client->import_plan_comment("IC_1", deadline));
  EXPECT_THROW(client->import_plan_comment("IC_1", deadline), std::runtime_error);
  EXPECT_EQ(calls, 2);
  mode->store(1);
  ImportContext body;
  EXPECT_THROW(client->import_plan_comment("IC_1", body), std::runtime_error);
  mode->store(2);
  ImportContext aggregate;
  aggregate.consume_body(144u * 1024 * 1024 - 1);
  EXPECT_THROW(client->import_plan_comment("IC_1", aggregate), std::runtime_error);
  EXPECT_EQ(calls, 4);
}

TEST_F(GitHubImportTransport, MissingFenceRequiresRealBranchAndRejectsUnsafeKeys) {
  std::atomic<int> branch{200};
  std::atomic<int> branch_calls{0};
  server.Get(contents_path, [&](const auto& req, auto& res) {
    ++calls;
    EXPECT_EQ(req.get_param_value("ref"), "main");
    res.status = 404;
  });
  server.Get("/repos/fixture/state/branches/main", [&](const auto&, auto& res) {
    ++branch_calls;
    res.status = branch;
    res.set_content(json{{"name", "main"}, {"commit", {{"sha", fence_sha}}}}.dump(),
                    "application/json");
  });
  start();
  ImportContext first;
  EXPECT_EQ(client->import_read_fence("main", fence_key, first), std::nullopt);
  EXPECT_EQ(branch_calls, 1);
  branch = 404;
  ImportContext second;
  EXPECT_THROW(client->import_read_fence("main", fence_key, second), std::runtime_error);
  EXPECT_EQ(branch_calls, 2);
  EXPECT_THROW(client->import_read_fence("main", "../else", second), std::runtime_error);
  EXPECT_THROW(client->import_read_fence("main?ref=other", fence_key, second), std::runtime_error);
  EXPECT_EQ(calls, 2);
}

TEST_F(GitHubImportTransport, ConditionalWriteRequiresAcknowledgementAndExactReadback) {
  const json document{{"phase", "creating"}, {"token", "private-fixture"}};
  std::atomic<int> reads{0};
  std::atomic<int> mode{0};
  server.Put(contents_path, [&](const auto& req, auto& res) {
    ++calls;
    auto request = json::parse(req.body);
    EXPECT_EQ(request.at("branch"), "main");
    EXPECT_EQ(request.at("content"), encoded(document.dump() + "\n"));
    if (calls == 1)
      EXPECT_FALSE(request.contains("sha"));
    else
      EXPECT_EQ(request.at("sha"), std::string(40, 'c'));
    res.status = calls == 1 ? 201 : 200;
    res.set_content(json{{"content", {{"path", fence_path}, {"sha", fence_sha}}},
                         {"commit", {{"sha", std::string(40, 'd')}}}}
                        .dump(),
                    "application/json");
  });
  server.Get(contents_path, [&](const auto& req, auto& res) {
    ++reads;
    EXPECT_EQ(req.get_param_value("ref"), "main");
    res.set_content(contents(mode == 1 ? json{{"phase", "prepared"}} : document).dump(),
                    "application/json");
  });
  start();
  ImportContext context;
  const auto written =
      client->import_write_fence("main", fence_key, document, std::nullopt, context);
  EXPECT_EQ(written.sha, fence_sha);
  EXPECT_EQ(written.document, document);
  EXPECT_EQ(reads, 1);
  mode = 1;
  EXPECT_THROW(
      client->import_write_fence("main", fence_key, document, std::string(40, 'c'), context),
      std::runtime_error);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(reads, 2);
}

TEST_F(GitHubImportTransport, RejectsConflictAndMalformedContentsAcknowledgement) {
  std::atomic<int> mode{0};
  server.Put(contents_path, [&](const auto&, auto& res) {
    ++calls;
    res.status = mode == 0 ? 409 : 201;
    res.set_content(R"({"content":{"sha":"invalid"},"commit":{}})", "application/json");
  });
  start();
  for (int n : {0, 1}) {
    mode = n;
    ImportContext context;
    EXPECT_THROW(client->import_write_fence("main", fence_key, json{{"phase", "prepared"}},
                                            std::nullopt, context),
                 std::runtime_error);
  }
  EXPECT_EQ(calls, 2);
}

TEST_F(GitHubImportTransport, RejectsMalformedFencePayloadAndDocumentBound) {
  std::atomic<int> mode{0};
  server.Get(contents_path, [&](const auto&, auto& res) {
    ++calls;
    auto result = contents(json{{"phase", "prepared"}});
    if (mode == 0) result["path"] = "fleet/other.json";
    if (mode == 1) result["content"] = "!!!=";
    if (mode == 2) result["content"] = encoded(std::string(16385, 'x'));
    if (mode == 3) result["content"] = encoded("[]");
    if (mode == 4) result["type"] = "symlink";
    res.set_content(result.dump(), "application/json");
  });
  start();
  for (int n = 0; n < 5; ++n) {
    mode = n;
    ImportContext context;
    EXPECT_THROW(client->import_read_fence("main", fence_key, context), std::runtime_error);
  }
  ImportContext context;
  EXPECT_THROW(
      client->import_write_fence("main", fence_key, json{{"large", std::string(16384, 'x')}},
                                 std::nullopt, context),
      std::runtime_error);
  EXPECT_EQ(calls, 5);
}

TEST_F(GitHubImportTransport, LostConditionalWriteIsNeverAcknowledgedOrRetried) {
  std::atomic<bool> committed{false};
  server.Put(contents_path, [&](const auto&, auto& res) {
    ++calls;
    std::this_thread::sleep_for(120ms);
    committed = true;
    res.status = 201;
    res.set_content(json{{"content", {{"path", fence_path}, {"sha", fence_sha}}},
                         {"commit", {{"sha", fence_sha}}}}
                        .dump(),
                    "application/json");
  });
  start();
  ImportContext context{40ms};
  EXPECT_THROW(client->import_write_fence("main", fence_key, json{{"phase", "creating"}},
                                          std::nullopt, context),
               std::runtime_error);
  server.stop();
  listener.join();
  EXPECT_TRUE(committed);
  EXPECT_EQ(calls, 1);
}

TEST_F(GitHubImportTransport, DelayedIssueCreationCompletesAfterTimeoutWithoutRetry) {
  std::atomic<bool> committed{false};
  server.Post("/repos/fixture/state/issues", [&](const auto& req, auto& res) {
    ++calls;
    EXPECT_EQ(json::parse(req.body), (json{{"title", "fixture"},
                                           {"body", "harmless"},
                                           {"labels", json::array({"agamemnon-hmas-task"})}}));
    std::this_thread::sleep_for(120ms);
    committed = true;
    res.status = 201;
    res.set_content(R"({"number":23})", "application/json");
  });
  start();
  ImportContext context{40ms};
  EXPECT_THROW(client->import_create_issue("fixture", "harmless", context), std::runtime_error);
  server.stop();
  listener.join();
  EXPECT_TRUE(committed);
  EXPECT_EQ(calls, 1);
}

TEST_F(GitHubImportTransport, ConfirmedIssueCreationReturnsOnlyPositiveIdentity) {
  std::atomic<int> number{23};
  server.Post("/repos/fixture/state/issues", [&](const auto&, auto& res) {
    ++calls;
    res.status = 201;
    res.set_content(json{{"number", number.load()}}.dump(), "application/json");
  });
  start();
  ImportContext context;
  EXPECT_EQ(client->import_create_issue("fixture", "harmless", context), "23");
  number = 0;
  EXPECT_THROW(client->import_create_issue("fixture", "harmless", context), std::runtime_error);
  EXPECT_EQ(calls, 2);
}

TEST_F(GitHubImportTransport, ConditionalReadbackPreservesNestedNumericTypes) {
  const json document{{"phase", "creating"}, {"identity", {{"issueNumber", 1}}}};
  std::atomic<bool> change_type{false};
  std::atomic<int> reads{0};
  server.Put(contents_path, [&](const auto&, auto& res) {
    ++calls;
    res.status = 201;
    res.set_content(json{{"content", {{"path", fence_path}, {"sha", fence_sha}}},
                         {"commit", {{"sha", fence_sha}}}}
                        .dump(),
                    "application/json");
  });
  server.Get(contents_path, [&](const auto&, auto& res) {
    ++reads;
    auto observed = document;
    if (change_type) observed["identity"]["issueNumber"] = 1.0;
    res.set_content(contents(observed).dump(), "application/json");
  });
  start();
  ImportContext context;
  const auto confirmed =
      client->import_write_fence("main", fence_key, document, std::nullopt, context);
  EXPECT_TRUE(confirmed.document["identity"]["issueNumber"].is_number_integer());
  change_type = true;
  EXPECT_THROW(client->import_write_fence("main", fence_key, document, fence_sha, context),
               std::runtime_error);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(reads, 2);
}

TEST_F(GitHubImportTransport, IgnoresAmbientProxyConfiguration) {
  // Hold a private port without listening; accidental proxy use cannot reach any shared service.
  httplib::Server unavailable_proxy;
  const auto proxy_port = unavailable_proxy.bind_to_any_port("127.0.0.1");
  ASSERT_GT(proxy_port, 0);
  struct Environment {
    std::vector<std::pair<std::string, std::optional<std::string>>> prior;
    void set(const char* name, const std::string& value) {
      const auto* old = std::getenv(name);
      prior.emplace_back(name, old ? std::optional<std::string>(old) : std::nullopt);
      if (setenv(name, value.c_str(), 1) != 0)
        throw std::runtime_error("fixture environment unavailable");
    }
    ~Environment() {
      for (auto it = prior.rbegin(); it != prior.rend(); ++it) {
        if (it->second)
          setenv(it->first.c_str(), it->second->c_str(), 1);
        else
          unsetenv(it->first.c_str());
      }
    }
  } environment;
  const auto proxy = "http://127.0.0.1:" + std::to_string(proxy_port);
  for (const char* name :
       {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"})
    environment.set(name, proxy);
  environment.set("no_proxy", "");
  environment.set("NO_PROXY", "");
  server.Post("/graphql", [&](const auto&, auto& res) {
    ++calls;
    res.set_content(R"({"data":{"direct":true}})", "application/json");
  });
  start();
  ImportContext context{1s};
  EXPECT_EQ(client->import_plan_comment("IC_fixture", context), (json{{"direct", true}}));
  EXPECT_EQ(calls, 1);
}

TEST_F(GitHubImportTransport, StalledChunkedBodyReturnsWithinWholeRequestDeadline) {
  std::atomic<bool> started{false};
  server.Post("/graphql", [&](const auto&, auto& res) {
    ++calls;
    res.set_chunked_content_provider("application/json",
                                     [&](std::size_t offset, httplib::DataSink& sink) {
                                       if (offset == 0) {
                                         started = true;
                                         sink.write("{\"data\":", 8);
                                         return true;
                                       }
                                       std::this_thread::sleep_for(150ms);
                                       sink.write("{}}", 3);
                                       sink.done();
                                       return true;
                                     });
  });
  start();
  const auto begin = std::chrono::steady_clock::now();
  ImportContext context{40ms};
  EXPECT_THROW(client->import_plan_comment("IC_fixture", context), std::runtime_error);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
  EXPECT_TRUE(started);
  EXPECT_EQ(calls, 1);
}

TEST_F(GitHubImportTransport, AggregateLimitCountsActualBodiesAcrossSuccessfulCalls) {
  std::string response = R"({"data":{"padding":")";
  response.append(8 * 1024 * 1024 - response.size() - 3, 'x');
  response += "\"}}";
  ASSERT_EQ(response.size(), 8u * 1024 * 1024);
  server.Post("/graphql", [&](const auto&, auto& res) {
    ++calls;
    res.set_content(response, "application/json");
  });
  start();
  ImportContext context;
  for (int n = 0; n < 18; ++n) {
    SCOPED_TRACE(n);
    ASSERT_NO_THROW(client->import_plan_comment("IC_fixture", context));
  }
  EXPECT_THROW(client->import_plan_comment("IC_fixture", context), std::runtime_error);
  EXPECT_EQ(calls, 19);
}

TEST_F(GitHubImportTransport, UnsupportedResolverRefusesBeforeAnyHttpRequest) {
  class SynchronousResolverClient : public LoopbackImportClient {
   public:
    using LoopbackImportClient::LoopbackImportClient;

   protected:
    bool import_resolver_supports_timeout() const override { return false; }
  } unsupported{port};
  server.Post("/graphql", [&](const auto&, auto& res) {
    ++calls;
    res.set_content(R"({"data":{}})", "application/json");
  });
  start();
  ImportContext supported;
  EXPECT_NO_THROW(client->import_plan_comment("IC_fixture", supported));
  EXPECT_EQ(calls, 1);
  ImportContext refused;
  EXPECT_THROW(unsupported.import_plan_comment("IC_fixture", refused), std::runtime_error);
  EXPECT_EQ(calls, 1);
}

TEST_F(GitHubImportTransport, StoreReconciles108LargeWorkLinkedRecordsAndReplaysAfterRestart) {
  const auto work_response = [](int number) {
    return json{{"repository",
                 {{"id", "R_work"},
                  {"nameWithOwner", "fixture/work"},
                  {"issue",
                   {{"__typename", "Issue"},
                    {"id", "I_work_" + std::to_string(number)},
                    {"number", number},
                    {"url", "https://github.com/fixture/work/issues/" + std::to_string(number)},
                    {"state", "OPEN"},
                    {"title", "Planned fixture work"},
                    {"body", "Implement this harmless fixture plan.\n"},
                    {"updatedAt", "2026-09-13T00:00:00Z"}}}}}};
  };
  auto config = std::make_shared<IssueImportConfiguration>();
  config->state_branch = "main";
  config->repositories =
      json::array({{{"key", "work"}, {"repository", "fixture/work"}, {"repositoryId", "R_work"}}});
  class SeedGitHub : public MockGitHubClient {
   public:
    std::function<json(int)> lookup;
    json import_work_issue(const std::string& owner, const std::string& name, int number,
                           ImportContext& context) override {
      context.checkpoint();
      EXPECT_EQ(owner + "/" + name, "fixture/work");
      return lookup(number);
    }
    std::vector<json> import_list_issues(ImportContext& context) override {
      context.checkpoint();
      EXPECT_TRUE(created_issues.empty());
      return {};
    }
    std::optional<ImportFence> import_read_fence(const std::string&, const std::string&,
                                                 ImportContext& context) override {
      context.checkpoint();
      return std::nullopt;
    }
  };
  std::vector<json> records;
  std::string large;
  while (large.size() < 60000) large += "quoted\" slash\\ control\n multibyte é ";
  // Each producer starts with an explicitly empty fixture namespace; only its
  // actual serialized body is used to seed the later, complete HTTP history.
  for (int n = 1; n <= 108; ++n) {
    auto seed = std::make_shared<SeedGitHub>();
    seed->lookup = work_response;
    Store producer{seed, config};
    HmasTask task{};
    task.id = "linked-cohort-" + std::to_string(n);
    task.layer = HmasLayer::L3_TaskAgent;
    task.state = TaskState::Pending;
    task.subject = "Existing cohort work";
    task.description = large;
    task.repo = "fixture/work";
    task.issue = n;
    task.created_at = "2026-09-13T00:00:00Z";
    producer.create_hmas_task(task);
    const auto body = seed->created_issues.at("1").at("body").get<std::string>();
    ASSERT_GE(body.size(), 60000u);
    ASSERT_NE(body.find(hmas_task_to_json(task).dump(2)), std::string::npos);
    records.push_back(issue(n, body));
    ASSERT_LE(records.back().dump().size(), 512u * 1024);
  }
  const auto original_records = records;
  CanonicalWorkIssue target{"fixture/work",
                            "R_work",
                            "I_work_1001",
                            1001,
                            "https://github.com/fixture/work/issues/1001",
                            true,
                            {},
                            {}};
  const auto owned_path = "fleet/imports/" + import_work_key(target) + ".json";
  const auto owned_contents = "/repos/fixture/state/contents/" + owned_path;
  std::mutex state;
  int lookups = 0, pages_seen = 0, creates = 0, writes = 0;
  std::string remote_content, remote_sha;
  std::vector<std::string> phases;
  server.Post("/graphql", [&](const auto& req, auto& res) {
    std::lock_guard lock(state);
    ++lookups;
    const auto variables = json::parse(req.body).at("variables");
    EXPECT_EQ(variables.at("owner"), "fixture");
    EXPECT_EQ(variables.at("name"), "work");
    const auto number = variables.at("number").template get<int>();
    EXPECT_TRUE((number >= 1 && number <= 108) || number == 1001);
    res.set_content(json{{"data", work_response(number)}}.dump(), "application/json");
  });
  server.Get("/repos/fixture/state/issues", [&](const auto& req, auto& res) {
    std::lock_guard lock(state);
    ++pages_seen;
    EXPECT_EQ(req.get_param_value("state"), "all");
    EXPECT_EQ(req.get_param_value("labels"), "agamemnon-hmas-task");
    EXPECT_EQ(req.get_param_value("per_page"), "10");
    const auto page = std::stoi(req.get_param_value("page"));
    json result = json::array();
    for (std::size_t n = (page - 1) * 10; n < records.size() && n < page * 10u; ++n)
      result.push_back(records[n]);
    const auto body = result.dump();
    EXPECT_LE(body.size(), 8u * 1024 * 1024);
    res.set_content(body, "application/json");
  });
  server.Get("/repos/fixture/state/branches/main", [&](const auto&, auto& res) {
    res.set_content(json{{"name", "main"}, {"commit", {{"sha", fence_sha}}}}.dump(),
                    "application/json");
  });
  server.Get(owned_contents, [&](const auto& req, auto& res) {
    std::lock_guard lock(state);
    EXPECT_EQ(req.get_param_value("ref"), "main");
    if (remote_sha.empty()) {
      res.status = 404;
      return;
    }
    res.set_content(json{{"path", owned_path},
                         {"type", "file"},
                         {"encoding", "base64"},
                         {"sha", remote_sha},
                         {"content", remote_content}}
                        .dump(),
                    "application/json");
  });
  server.Put(owned_contents, [&](const auto& req, auto& res) {
    std::lock_guard lock(state);
    const auto payload = json::parse(req.body);
    EXPECT_EQ(payload.at("branch"), "main");
    if (remote_sha.empty())
      EXPECT_FALSE(payload.contains("sha"));
    else
      EXPECT_EQ(payload.at("sha"), remote_sha);
    remote_content = payload.at("content").template get<std::string>();
    std::string decoded(remote_content.size(), '\0');
    auto length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                  reinterpret_cast<const unsigned char*>(remote_content.data()),
                                  remote_content.size());
    ASSERT_GT(length, 0);
    length -= remote_content.ends_with("==") ? 2 : remote_content.ends_with("=") ? 1 : 0;
    decoded.resize(length);
    const auto document = json::parse(decoded);
    phases.push_back(document.at("phase").template get<std::string>());
    ++writes;
    remote_sha = std::string(39, 'e') + std::to_string(writes);
    res.status = writes == 1 ? 201 : 200;
    res.set_content(json{{"content", {{"path", owned_path}, {"sha", remote_sha}}},
                         {"commit", {{"sha", fence_sha}}}}
                        .dump(),
                    "application/json");
  });
  server.Post("/repos/fixture/state/issues", [&](const auto& req, auto& res) {
    std::lock_guard lock(state);
    ++creates;
    EXPECT_EQ(phases, (std::vector<std::string>{"prepared", "creating"}));
    const auto payload = json::parse(req.body);
    EXPECT_EQ(payload.at("labels"), json::array({"agamemnon-hmas-task"}));
    auto created = issue(1000, payload.at("body").template get<std::string>());
    created["state"] = "open";
    records.push_back(created);
    res.status = 201;
    res.set_content(R"({"number":1000})", "application/json");
  });
  start();
  auto transport = std::make_shared<LoopbackImportClient>(port);
  AuthMiddleware auth{"fixture-key"};
  Store store{transport, config};
  FleetIssueService service{store, config, auth};
  const auto inspection = service.inspect("work", 1001);
  ASSERT_EQ(inspection.status, 200);
  const json request{{"schema", "hi/agamemnon/issue-import/v1"},
                     {"repositoryKey", "work"},
                     {"repositoryId", inspection.body.at("repositoryId")},
                     {"issueId", inspection.body.at("issueId")},
                     {"issueNumber", 1001},
                     {"plan", inspection.body.at("plan")}};
  const auto imported = service.import_request(request);
  ASSERT_EQ(imported.status, 201);
  ASSERT_EQ(store.list_hmas_tasks_by_layer(HmasLayer::L3_TaskAgent).size(), 109u);
  Store restarted{transport, config};
  FleetIssueService replay_service{restarted, config, auth};
  const auto replay = replay_service.import_request(request);
  ASSERT_EQ(replay.status, 200);
  EXPECT_EQ(replay.body, imported.body);
  EXPECT_EQ(restarted.list_hmas_tasks_by_layer(HmasLayer::L3_TaskAgent).size(), 109u);
  std::lock_guard lock(state);
  EXPECT_EQ(creates, 1);
  EXPECT_EQ(writes, 3);
  EXPECT_EQ(phases, (std::vector<std::string>{"prepared", "creating", "linked"}));
  EXPECT_EQ(pages_seen, 22);
  EXPECT_EQ(lookups, 219);
  ASSERT_EQ(records.size(), 109u);
  EXPECT_TRUE(std::equal(original_records.begin(), original_records.end(), records.begin()));
}
}  // namespace agamemnon::test
