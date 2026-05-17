#include "projectagamemnon/dead_letter_queue.hpp"

#include <iostream>

namespace projectagamemnon {

static int64_t now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void DeadLetterQueue::push(std::string subject, std::string payload, int attempts,
                           std::string level, std::string service) {
  std::lock_guard<std::mutex> lock(mu_);
  if (queue_.size() >= capacity_) {
    std::cerr << "[dlq] WARNING: dead-letter queue full (capacity=" << capacity_
              << "), evicting oldest entry\n";
    queue_.pop_front();
  }
  queue_.push_back({std::move(subject), std::move(payload), attempts, now_ms(),
                    std::move(level), std::move(service)});
}

std::vector<DeadLetterQueue::Entry> DeadLetterQueue::drain() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Entry> result(std::make_move_iterator(queue_.begin()),
                            std::make_move_iterator(queue_.end()));
  queue_.clear();
  return result;
}

void DeadLetterQueue::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  queue_.clear();
}

std::size_t DeadLetterQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.size();
}

bool DeadLetterQueue::empty() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.empty();
}

}  // namespace projectagamemnon
