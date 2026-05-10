#include "projectagamemnon/rate_limiter.hpp"

#include <algorithm>

namespace projectagamemnon {

RateLimiter::RateLimiter(double tokens_per_second, double burst_capacity)
    : tokens_per_second_(tokens_per_second), burst_capacity_(burst_capacity) {}

void RateLimiter::refill(Bucket& bucket, std::chrono::steady_clock::time_point now) const {
  auto elapsed = std::chrono::duration<double>(now - bucket.last_seen).count();
  bucket.tokens = std::min(burst_capacity_, bucket.tokens + elapsed * tokens_per_second_);
  bucket.last_seen = now;
}

void RateLimiter::evict_stale(std::chrono::steady_clock::time_point now) {
  // Called under mutex_ — no need to re-lock.
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    if ((now - it->second.last_seen) > kEvictAfter) {
      it = buckets_.erase(it);
    } else {
      ++it;
    }
  }
}

bool RateLimiter::allow(const std::string& client_ip) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();

  auto [it, inserted] = buckets_.emplace(client_ip, Bucket{burst_capacity_, now});
  Bucket& bucket = it->second;

  if (!inserted) {
    refill(bucket, now);
  }

  if (++request_count_since_evict_ >= kEvictCheckInterval) {
    request_count_since_evict_ = 0;
    evict_stale(now);
    // Iterator `it`/`bucket` may be invalidated after eviction if the IP was
    // just evicted, but that can't happen: we just refreshed last_seen above.
  }

  if (bucket.tokens >= 1.0) {
    bucket.tokens -= 1.0;
    return true;
  }
  return false;
}

double RateLimiter::retry_after_seconds(const std::string& client_ip) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();

  auto it = buckets_.find(client_ip);
  if (it == buckets_.end()) {
    return 0.0;
  }

  Bucket bucket = it->second;
  refill(bucket, now);

  if (bucket.tokens >= 1.0) {
    return 0.0;
  }

  double tokens_needed = 1.0 - bucket.tokens;
  return tokens_needed / tokens_per_second_;
}

}  // namespace projectagamemnon
