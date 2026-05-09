#include "projectagamemnon/circuit_breaker.hpp"

#include <iostream>

namespace projectagamemnon {

int64_t CircuitBreaker::now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void CircuitBreaker::transition_to_open() noexcept {
  open_since_ms_.store(now_ms(), std::memory_order_relaxed);
  state_.store(static_cast<uint8_t>(State::Open), std::memory_order_release);
  std::cerr << "[circuit_breaker] ERROR: NATS circuit OPEN after " << failure_count_.load()
            << " consecutive failures — messages will be dead-lettered\n";
}

bool CircuitBreaker::allow_attempt() noexcept {
  auto s = static_cast<State>(state_.load(std::memory_order_acquire));
  if (s == State::Closed) return true;

  if (s == State::Open) {
    int64_t elapsed = now_ms() - open_since_ms_.load(std::memory_order_relaxed);
    if (elapsed >= cfg_.probe_interval.count()) {
      state_.store(static_cast<uint8_t>(State::HalfOpen), std::memory_order_release);
      std::cerr << "[circuit_breaker] NATS circuit HALF-OPEN — sending probe\n";
      return true;
    }
    return false;
  }

  // HalfOpen: allow the one probe attempt
  return true;
}

void CircuitBreaker::record_success() noexcept {
  auto s = static_cast<State>(state_.load(std::memory_order_acquire));
  if (s != State::Closed) {
    std::cerr << "[circuit_breaker] NATS circuit CLOSED — connection restored\n";
  }
  failure_count_.store(0, std::memory_order_relaxed);
  state_.store(static_cast<uint8_t>(State::Closed), std::memory_order_release);
}

void CircuitBreaker::record_failure() noexcept {
  int count = failure_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  auto s = static_cast<State>(state_.load(std::memory_order_acquire));

  if (s == State::HalfOpen) {
    transition_to_open();
    return;
  }

  if (s == State::Closed && count >= cfg_.failure_threshold) {
    transition_to_open();
  }
}

std::string CircuitBreaker::state_label() const noexcept {
  switch (static_cast<State>(state_.load(std::memory_order_acquire))) {
    case State::Closed:
      return "closed";
    case State::Open:
      return "open";
    case State::HalfOpen:
      return "half_open";
  }
  return "unknown";
}

}  // namespace projectagamemnon
