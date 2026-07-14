#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace agamemnon {

/// Token-bucket rate limiter with per-client-IP tracking.
///
/// Each client IP gets its own bucket. Tokens refill at `tokens_per_second`
/// up to `burst_capacity`. Each allowed request consumes one token.
/// Buckets unseen for >5 minutes are evicted to bound memory growth.
class RateLimiter {
 public:
  /// @param tokens_per_second  Sustained request rate allowed per client IP.
  /// @param burst_capacity     Maximum tokens (burst allowance) per client IP.
  RateLimiter(double tokens_per_second, double burst_capacity);

  /// Returns true if the request from `client_ip` is within the rate limit.
  /// Thread-safe.
  bool allow(const std::string& client_ip);

  /// Returns how many seconds until the next token is available for `client_ip`.
  /// Returns 0 if a token is already available.
  /// Thread-safe.
  double retry_after_seconds(const std::string& client_ip);

 private:
  struct Bucket {
    double tokens;
    std::chrono::steady_clock::time_point last_seen;
  };

  void refill(Bucket& bucket, std::chrono::steady_clock::time_point now) const;
  void evict_stale(std::chrono::steady_clock::time_point now);

  double tokens_per_second_;
  double burst_capacity_;

  std::mutex mutex_;
  std::unordered_map<std::string, Bucket> buckets_;

  static constexpr auto kEvictAfter = std::chrono::minutes{5};
  static constexpr int kEvictCheckInterval = 100;
  int request_count_since_evict_{0};
};

}  // namespace agamemnon
