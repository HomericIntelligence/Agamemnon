#include "core/metrics.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

#include "concurrency/logger.hpp"  // Phase D: For queue depth alerting
#include "core/config.hpp"         // FIX m3: Centralized configuration

namespace keystone {
namespace core {

Metrics::Metrics() : start_time_(std::chrono::steady_clock::now()) {}

Metrics& Metrics::getInstance() {
  static Metrics instance;
  return instance;
}

void Metrics::recordMessageSent(const std::string& msg_id, Priority priority) {
  messages_sent_.fetch_add(1, std::memory_order_relaxed);

  // FIX: Track priority distribution using enum (type-safe)
  switch (priority) {
    case Priority::HIGH:
      high_priority_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case Priority::NORMAL:
      normal_priority_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case Priority::LOW:
      low_priority_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      // Shouldn't happen with enum, but handle gracefully
      normal_priority_count_.fetch_add(1, std::memory_order_relaxed);
      break;
  }

  // Record timestamp for latency tracking
  {
    std::lock_guard<std::mutex> lock(timestamps_mutex_);
    message_timestamps_[msg_id] = {std::chrono::steady_clock::now()};

    // SECURITY FIX: Prevent memory leak with guaranteed cleanup
    if (message_timestamps_.size() > Config::METRICS_MAX_TIMESTAMP_ENTRIES) {
      cleanupOldTimestamps();

      // If time-based cleanup didn't reduce size enough, force removal of
      // oldest entries
      if (message_timestamps_.size() > Config::METRICS_MAX_TIMESTAMP_ENTRIES) {
        // Calculate how many entries to remove (10% of limit)
        size_t entries_to_remove =
            message_timestamps_.size() - Config::METRICS_MAX_TIMESTAMP_ENTRIES;

        // Sort entries by timestamp and remove oldest
        std::vector<std::pair<std::string, LatencyRecord>> sorted_entries(
            message_timestamps_.begin(), message_timestamps_.end());

        std::sort(sorted_entries.begin(), sorted_entries.end(), [](const auto& a, const auto& b) {
          return a.second.send_time < b.second.send_time;
        });

        // Remove oldest entries
        for (size_t i = 0; i < entries_to_remove && i < sorted_entries.size(); ++i) {
          message_timestamps_.erase(sorted_entries[i].first);
        }
      }
    }
  }
}

void Metrics::recordMessageProcessed(const std::string& msg_id) {
  messages_processed_.fetch_add(1, std::memory_order_relaxed);

  // Calculate latency if we have send timestamp
  {
    std::lock_guard<std::mutex> lock(timestamps_mutex_);
    auto it = message_timestamps_.find(msg_id);
    if (it != message_timestamps_.end()) {
      auto now = std::chrono::steady_clock::now();
      auto latency_us =
          std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.send_time).count();

      total_latency_us_.fetch_add(latency_us, std::memory_order_relaxed);
      latency_sample_count_.fetch_add(1, std::memory_order_relaxed);

      // Clean up timestamp
      message_timestamps_.erase(it);
    }
  }
}

void Metrics::recordQueueDepth(const std::string& agent_id, size_t depth) {
  {
    std::lock_guard<std::mutex> lock(queue_depths_mutex_);
    agent_queue_depths_[agent_id] = depth;
  }

  // Update max depth
  size_t current_max = max_queue_depth_.load(std::memory_order_relaxed);
  while (depth > current_max) {
    if (max_queue_depth_.compare_exchange_weak(current_max, depth, std::memory_order_relaxed)) {
      break;
    }
  }

  // Phase D: Alert on queue depth thresholds
  if (depth > Config::METRICS_QUEUE_DEPTH_CRITICAL) {
    concurrency::Logger::critical("Agent {} queue CRITICAL: {} messages (threshold: {})", agent_id,
                                  depth, Config::METRICS_QUEUE_DEPTH_CRITICAL);
  } else if (depth > Config::METRICS_QUEUE_DEPTH_WARNING) {
    concurrency::Logger::warn("Agent {} queue high: {} messages (threshold: {})", agent_id, depth,
                              Config::METRICS_QUEUE_DEPTH_WARNING);
  }
}

void Metrics::recordWorkerActivity(int32_t worker_id, bool is_busy) {
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_busy_states_[worker_id] = is_busy;
  }

  // Sample worker utilization
  total_worker_samples_.fetch_add(1, std::memory_order_relaxed);
  if (is_busy) {
    total_busy_samples_.fetch_add(1, std::memory_order_relaxed);
  }
}

uint64_t Metrics::getTotalMessagesSent() const {
  return messages_sent_.load(std::memory_order_relaxed);
}

uint64_t Metrics::getTotalMessagesProcessed() const {
  return messages_processed_.load(std::memory_order_relaxed);
}

double Metrics::getMessagesPerSecond() const {
  auto now = std::chrono::steady_clock::now();
  auto elapsed_s = std::chrono::duration<double>(now - start_time_).count();

  if (elapsed_s < 0.001) {
    return 0.0;
  }

  uint64_t total = messages_processed_.load(std::memory_order_relaxed);
  return static_cast<double>(total) / elapsed_s;
}

std::optional<double> Metrics::getAverageLatencyUs() const {
  uint64_t count = latency_sample_count_.load(std::memory_order_relaxed);
  if (count == 0) {
    return std::nullopt;
  }

  uint64_t total = total_latency_us_.load(std::memory_order_relaxed);
  return static_cast<double>(total) / static_cast<double>(count);
}

size_t Metrics::getMaxQueueDepth() const {
  return max_queue_depth_.load(std::memory_order_relaxed);
}

double Metrics::getWorkerUtilization() const {
  uint64_t samples = total_worker_samples_.load(std::memory_order_relaxed);
  if (samples == 0) {
    return 0.0;
  }

  uint64_t busy = total_busy_samples_.load(std::memory_order_relaxed);
  return (static_cast<double>(busy) / static_cast<double>(samples)) * 100.0;
}

Metrics::PriorityStats Metrics::getPriorityStats() const {
  return {high_priority_count_.load(std::memory_order_relaxed),
          normal_priority_count_.load(std::memory_order_relaxed),
          low_priority_count_.load(std::memory_order_relaxed)};
}

void Metrics::setInFlightCount(int64_t count) {
  in_flight_count_.store(count, std::memory_order_relaxed);
}

int64_t Metrics::getInFlightCount() const {
  return in_flight_count_.load(std::memory_order_relaxed);
}

void Metrics::recordDeadlineMiss(const std::string& /* msg_id */, int64_t late_by_ms) {
  deadline_misses_.fetch_add(1, std::memory_order_relaxed);
  total_deadline_miss_ms_.fetch_add(late_by_ms, std::memory_order_relaxed);
}

uint64_t Metrics::getTotalDeadlineMisses() const {
  return deadline_misses_.load(std::memory_order_relaxed);
}

std::optional<double> Metrics::getAverageDeadlineMissMs() const {
  uint64_t misses = deadline_misses_.load(std::memory_order_relaxed);
  if (misses == 0) {
    return std::nullopt;
  }

  int64_t total_ms = total_deadline_miss_ms_.load(std::memory_order_relaxed);
  return static_cast<double>(total_ms) / static_cast<double>(misses);
}

void Metrics::reset() {
  messages_sent_.store(0, std::memory_order_relaxed);
  messages_processed_.store(0, std::memory_order_relaxed);
  high_priority_count_.store(0, std::memory_order_relaxed);
  normal_priority_count_.store(0, std::memory_order_relaxed);
  low_priority_count_.store(0, std::memory_order_relaxed);
  total_latency_us_.store(0, std::memory_order_relaxed);
  latency_sample_count_.store(0, std::memory_order_relaxed);
  max_queue_depth_.store(0, std::memory_order_relaxed);
  total_busy_samples_.store(0, std::memory_order_relaxed);
  total_worker_samples_.store(0, std::memory_order_relaxed);
  deadline_misses_.store(0, std::memory_order_relaxed);
  total_deadline_miss_ms_.store(0, std::memory_order_relaxed);
  in_flight_count_.store(0, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(timestamps_mutex_);
    message_timestamps_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(queue_depths_mutex_);
    agent_queue_depths_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_busy_states_.clear();
  }

  start_time_ = std::chrono::steady_clock::now();
}

std::string Metrics::generateReport() const {
  std::stringstream ss;
  ss << std::fixed << std::setprecision(2);

  ss << "\n=== HMAS Performance Metrics ===\n\n";

  // Throughput
  ss << "Throughput:\n";
  ss << "  Messages Sent:      " << getTotalMessagesSent() << "\n";
  ss << "  Messages Processed: " << getTotalMessagesProcessed() << "\n";
  ss << "  Messages/Second:    " << getMessagesPerSecond() << "\n\n";

  // Latency
  auto latency = getAverageLatencyUs();
  ss << "Latency:\n";
  if (latency.has_value()) {
    ss << "  Average Latency:    " << *latency << " μs\n";
  } else {
    ss << "  Average Latency:    No data\n";
  }
  ss << "\n";

  // Priority distribution
  auto priority_stats = getPriorityStats();
  uint64_t total_priority =
      priority_stats.high_count + priority_stats.normal_count + priority_stats.low_count;
  ss << "Priority Distribution:\n";
  ss << "  HIGH:    " << priority_stats.high_count;
  if (total_priority > 0) {
    ss << " (" << (100.0 * priority_stats.high_count / total_priority) << "%)";
  }
  ss << "\n";
  ss << "  NORMAL:  " << priority_stats.normal_count;
  if (total_priority > 0) {
    ss << " (" << (100.0 * priority_stats.normal_count / total_priority) << "%)";
  }
  ss << "\n";
  ss << "  LOW:     " << priority_stats.low_count;
  if (total_priority > 0) {
    ss << " (" << (100.0 * priority_stats.low_count / total_priority) << "%)";
  }
  ss << "\n\n";

  // Queue depths
  ss << "Queue Management:\n";
  ss << "  Max Queue Depth:    " << getMaxQueueDepth() << "\n\n";

  // Worker utilization
  ss << "Worker Utilization:\n";
  ss << "  Utilization:        " << getWorkerUtilization() << "%\n\n";

  // Deadline tracking
  ss << "Deadline Tracking:\n";
  ss << "  Deadline Misses:    " << getTotalDeadlineMisses() << "\n";
  auto avg_miss = getAverageDeadlineMissMs();
  if (avg_miss.has_value()) {
    ss << "  Avg Late By:        " << *avg_miss << " ms\n";
  } else {
    ss << "  Avg Late By:        No data\n";
  }

  ss << "\n================================\n";

  return ss.str();
}

void Metrics::cleanupOldTimestamps() {
  // FIX M3: Time-based expiration instead of O(n log n) sort
  // Remove entries older than Config::METRICS_TIMESTAMP_EXPIRY
  // Note: Caller must hold timestamps_mutex_

  if (message_timestamps_.empty()) {
    return;
  }

  auto now = std::chrono::steady_clock::now();

  // Iterate and erase old entries (more efficient than sorting)
  for (auto it = message_timestamps_.begin(); it != message_timestamps_.end();) {
    if (now - it->second.send_time > Config::METRICS_TIMESTAMP_EXPIRY) {
      it = message_timestamps_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace core
}  // namespace keystone
