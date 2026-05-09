#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace projectagamemnon {

/// Bounded ring buffer for NATS messages that failed all publish retries.
///
/// Thread-safe. Evicts the oldest entry when capacity is exceeded so memory
/// usage is bounded (default 256 entries).
class DeadLetterQueue {
 public:
  struct Entry {
    std::string subject;
    std::string payload;
    int attempts{0};
    int64_t timestamp_ms{0};  // milliseconds since epoch (steady_clock)
  };

  explicit DeadLetterQueue(std::size_t capacity = 256) : capacity_(capacity) {}

  /// Enqueue a failed message. Evicts the oldest entry if at capacity.
  void push(std::string subject, std::string payload, int attempts);

  /// Remove and return all entries (drains the queue).
  std::vector<Entry> drain();

  /// Discard all entries without returning them.
  void clear();

  std::size_t size() const;
  bool empty() const;

 private:
  const std::size_t capacity_;
  mutable std::mutex mu_;
  std::deque<Entry> queue_;
};

}  // namespace projectagamemnon
