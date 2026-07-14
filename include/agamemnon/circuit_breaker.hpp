#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace agamemnon {

/// Three-state circuit breaker guarding NATS publish operations.
///
/// State transitions:
///   Closed  → Open      : failure_count reaches threshold
///   Open    → HalfOpen  : probe_interval has elapsed since opening
///   HalfOpen → Closed   : probe publish succeeds (failure_count reset)
///   HalfOpen → Open     : probe publish fails (timer reset)
class CircuitBreaker {
 public:
  enum class State : uint8_t { Closed, Open, HalfOpen };

  struct Config {
    int failure_threshold{5};
    std::chrono::milliseconds probe_interval{30'000};
  };

  CircuitBreaker() = default;
  explicit CircuitBreaker(Config cfg) : cfg_(cfg) {}

  /// Returns true if the caller should attempt a publish.
  /// Transitions Open→HalfOpen when probe_interval has elapsed.
  bool allow_attempt() noexcept;

  /// Call after a successful publish.
  void record_success() noexcept;

  /// Call after a failed publish attempt.
  void record_failure() noexcept;

  [[nodiscard]] State state() const noexcept {
    return static_cast<State>(state_.load(std::memory_order_acquire));
  }

  [[nodiscard]] int failure_count() const noexcept {
    return failure_count_.load(std::memory_order_relaxed);
  }

  /// Human-readable state label for health endpoints.
  [[nodiscard]] std::string state_label() const noexcept;

 private:
  Config cfg_;
  std::atomic<uint8_t> state_{static_cast<uint8_t>(State::Closed)};
  std::atomic<int> failure_count_{0};
  std::atomic<int64_t> open_since_ms_{0};  // milliseconds since epoch when last opened

  void transition_to_open() noexcept;
  static int64_t now_ms() noexcept;
};

}  // namespace agamemnon
