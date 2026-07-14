#include "agamemnon/github_client.hpp"

#include <stdexcept>
#include <string>
#include <vector>

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
