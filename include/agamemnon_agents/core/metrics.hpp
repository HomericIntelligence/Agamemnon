#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "core/config.hpp"   // FIX m3: Centralized configuration
#include "core/message.hpp"  // For Priority enum

namespace keystone {
namespace core {

/**
 * @brief Thread-safe metrics collection for HMAS performance monitoring
 *
 * Tracks:
 * - Message throughput (messages/second)
 * - Message latency (send to process time)
 * - Queue depths per agent
 * - Worker utilization
 * - Priority distribution
 *
 * All operations are lock-free or use minimal locking for thread safety.
 */
class Metrics {
 public:
  /**
   * @brief Get singleton instance
   */
  static Metrics& getInstance();

  // Prevent copying
  Metrics(const Metrics&) = delete;
  Metrics& operator=(const Metrics&) = delete;

  /**
   * @brief Record a message sent
   * @param msg_id Message identifier
   * @param priority Message priority (FIX: now uses Priority enum for type
   * safety)
   */
  void recordMessageSent(const std::string& msg_id, Priority priority);

  /**
   * @brief Record a message processed
   * @param msg_id Message identifier
   */
  void recordMessageProcessed(const std::string& msg_id);

  /**
   * @brief Record queue depth for an agent
   * @param agent_id Agent identifier
   * @param depth Current queue depth (high + normal + low)
   *
   * Phase D: Logs warnings when depth exceeds thresholds:
   * - WARNING: depth > 1000 messages
   * - CRITICAL: depth > 10000 messages
   */
  void recordQueueDepth(const std::string& agent_id, size_t depth);

  /**
   * @brief Record worker activity
   * @param worker_id Worker identifier
   * @param is_busy True if worker is busy, false if idle
   */
  void recordWorkerActivity(int worker_id, bool is_busy);

  /**
   * @brief Get total messages sent
   */
  uint64_t getTotalMessagesSent() const;

  /**
   * @brief Get total messages processed
   */
  uint64_t getTotalMessagesProcessed() const;

  /**
   * @brief Get messages per second (average over last interval)
   */
  double getMessagesPerSecond() const;

  /**
   * @brief Get average message latency in microseconds
   */
  std::optional<double> getAverageLatencyUs() const;

  /**
   * @brief Get maximum queue depth across all agents
   */
  size_t getMaxQueueDepth() const;

  /**
   * @brief Get worker utilization percentage
   */
  double getWorkerUtilization() const;

  /**
   * @brief Get priority distribution (HIGH/NORMAL/LOW counts)
   */
  struct PriorityStats {
    uint64_t high_count;
    uint64_t normal_count;
    uint64_t low_count;
  };
  PriorityStats getPriorityStats() const;

  /**
   * @brief Set the current number of in-flight task claims.
   *
   * Called by the TaskClaimer (or its C++ bridge) to report how many
   * advance_dag_tracked tasks are currently executing. The value is snapshotted
   * on each call and exposed as a Prometheus gauge.
   *
   * @param count Number of tasks currently in-flight.
   */
  void setInFlightCount(int64_t count);

  /**
   * @brief Get the current in-flight task claim count.
   * @return Last value set by setInFlightCount(), or 0 if never set.
   */
  int64_t getInFlightCount() const;

  /**
   * @brief Record a deadline miss
   * @param msg_id Message identifier that missed deadline
   * @param late_by_ms How late the message was processed (in milliseconds)
   */
  void recordDeadlineMiss(const std::string& msg_id, int64_t late_by_ms);

  /**
   * @brief Get total deadline misses
   */
  uint64_t getTotalDeadlineMisses() const;

  /**
   * @brief Get average deadline miss time in milliseconds
   */
  std::optional<double> getAverageDeadlineMissMs() const;

  /**
   * @brief Reset all metrics
   */
  void reset();

  /**
   * @brief Generate human-readable metrics report
   */
  std::string generateReport() const;

 private:
  Metrics();
  ~Metrics() = default;

  /**
   * @brief Clean up old timestamps to prevent memory leak
   *
   * Called when timestamp map exceeds Config::METRICS_MAX_TIMESTAMP_ENTRIES.
   * Removes entries older than Config::METRICS_TIMESTAMP_EXPIRY.
   */
  void cleanupOldTimestamps();

  // Message counters (atomic for lock-free increment)
  std::atomic<uint64_t> messages_sent_{0};
  std::atomic<uint64_t> messages_processed_{0};
  std::atomic<uint64_t> high_priority_count_{0};
  std::atomic<uint64_t> normal_priority_count_{0};
  std::atomic<uint64_t> low_priority_count_{0};

  // Latency tracking
  struct LatencyRecord {
    std::chrono::steady_clock::time_point send_time;
  };
  std::map<std::string, LatencyRecord> message_timestamps_;
  std::mutex timestamps_mutex_;

  std::atomic<uint64_t> total_latency_us_{0};
  std::atomic<uint64_t> latency_sample_count_{0};

  // Queue depth tracking
  std::map<std::string, size_t> agent_queue_depths_;
  std::mutex queue_depths_mutex_;
  std::atomic<size_t> max_queue_depth_{0};

  // Worker utilization
  std::map<int, bool> worker_busy_states_;
  std::mutex worker_mutex_;
  std::atomic<uint64_t> total_busy_samples_{0};
  std::atomic<uint64_t> total_worker_samples_{0};

  // Deadline tracking
  std::atomic<uint64_t> deadline_misses_{0};
  std::atomic<int64_t> total_deadline_miss_ms_{0};

  // In-flight task claim count (reported by TaskClaimer)
  std::atomic<int64_t> in_flight_count_{0};

  // Throughput calculation
  std::chrono::steady_clock::time_point start_time_;
};

}  // namespace core
}  // namespace keystone
