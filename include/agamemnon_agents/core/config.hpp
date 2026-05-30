#pragma once

#include <chrono>
#include <cstddef>

namespace keystone {
namespace core {

/**
 * @brief Global configuration constants for ProjectKeystone HMAS
 *
 * FIX m3: Extract magic numbers to named constants with documentation.
 * These values can be made runtime-configurable in future versions.
 */
struct Config {
  // ========================================================================
  // Agent Configuration
  // ========================================================================

  /**
   * @brief Maximum number of agents that can be registered in MessageBus
   *
   * FIX P2-10: Prevents DoS via agent registration flooding.
   * Limits memory consumption to ~10GB (assuming 1MB per agent avg).
   *
   * Default: 10,000 agents
   */
  static constexpr size_t MAX_AGENTS = 10000;

  /**
   * @brief Maximum total messages per agent inbox across all priorities
   *
   * Prevents memory exhaustion under high load. When exceeded, backpressure
   * is applied (messages rejected until queue drains to QUEUE_LOW_WATERMARK).
   *
   * Default: 10,000 messages (~10-50MB depending on message size)
   */
  static constexpr size_t AGENT_MAX_QUEUE_SIZE = 10000;

  /**
   * @brief Queue size threshold for clearing backpressure (hysteresis)
   *
   * Backpressure is removed when queue drops below this percentage of MAX.
   * Prevents oscillation at the threshold boundary.
   *
   * Default: 80% of MAX_QUEUE_SIZE
   */
  static constexpr double AGENT_QUEUE_LOW_WATERMARK_PERCENT = 0.8;

  /**
   * @brief Interval for forced low-priority message checks
   *
   * To prevent starvation, NORMAL/LOW priority queues are checked every
   * N milliseconds regardless of HIGH priority queue state.
   *
   * Default: 100ms (guarantees max 100ms latency for low-priority messages)
   */
  static constexpr std::chrono::milliseconds AGENT_LOW_PRIORITY_CHECK_INTERVAL{100};

  // ========================================================================
  // Metrics Configuration
  // ========================================================================

  /**
   * @brief Maximum latency timestamp entries before cleanup
   *
   * When exceeded, time-based cleanup removes entries older than EXPIRY_TIME.
   *
   * Default: 10,000 entries
   */
  static constexpr size_t METRICS_MAX_TIMESTAMP_ENTRIES = 10000;

  /**
   * @brief Age threshold for removing old latency timestamps
   *
   * Entries older than this are removed during cleanup to prevent unbounded
   * memory growth.
   *
   * Default: 60 seconds
   */
  static constexpr std::chrono::seconds METRICS_TIMESTAMP_EXPIRY{60};

  /**
   * @brief Queue depth warning threshold
   *
   * Logs WARNING when agent queue exceeds this size.
   *
   * Default: 1,000 messages
   */
  static constexpr size_t METRICS_QUEUE_DEPTH_WARNING = 1000;

  /**
   * @brief Queue depth critical threshold
   *
   * Logs CRITICAL when agent queue exceeds this size.
   *
   * Default: 10,000 messages (same as AGENT_MAX_QUEUE_SIZE)
   */
  static constexpr size_t METRICS_QUEUE_DEPTH_CRITICAL = 10000;

  // ========================================================================
  // Task / Coordination Configuration
  // ========================================================================

  /**
   * @brief Default task execution timeout (25 minutes in milliseconds)
   *
   * Used as fallback when no explicit deadline is specified in a task spec.
   * 25 minutes allows for complex multi-level coordination chains while
   * preventing indefinite hangs.
   */
  static constexpr int DEFAULT_TASK_TIMEOUT_MS = 25 * 60 * 1000;

  // ========================================================================
  // HTTP Server Configuration (PrometheusExporter)
  // ========================================================================

  /**
   * @brief HTTP request buffer size
   *
   * Maximum size of HTTP request that can be read in one call.
   * Requests larger than this are truncated.
   *
   * Default: 1024 bytes (sufficient for GET /metrics requests)
   */
  static constexpr size_t HTTP_REQUEST_BUFFER_SIZE = 1024;

  /**
   * @brief HTTP socket read timeout
   *
   * Prevents slowloris attacks by limiting time spent waiting for data.
   *
   * Default: 5 seconds
   */
  static constexpr std::chrono::seconds HTTP_READ_TIMEOUT{5};

  /**
   * @brief Maximum concurrent HTTP connections
   *
   * Listen backlog size for the HTTP server.
   *
   * Default: 10 connections
   */
  static constexpr int HTTP_MAX_PENDING_CONNECTIONS = 10;
};

}  // namespace core
}  // namespace keystone
