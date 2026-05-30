#pragma once

// ── ADR-015 PORT NOTE ────────────────────────────────────────────────────────
// This is the ProjectAgamemnon port of Keystone's concurrency Logger. The
// original Keystone implementation was a thin wrapper over spdlog/fmt. To keep
// Agamemnon's HMAS agent runtime *standalone* (no new spdlog/fmt dependency,
// per the ADR-015 extraction constraints) this port replaces the spdlog
// backend with a tiny self-contained logger that preserves the public API
// (`Logger::{trace,debug,info,warn,error,critical}`, `LogContext`,
// `CorrelationScope`) and the brace-style `{}` format placeholders used by the
// ported agents and scheduler. It writes to stderr and is thread-safe.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace keystone {
namespace concurrency {

/**
 * @brief Severity levels for the standalone logger.
 *
 * Mirrors the subset of spdlog levels used by the ported runtime. Kept as a
 * dedicated enum so the public Logger API does not leak any third-party type.
 */
enum class LogLevel : int {
  trace = 0,
  debug = 1,
  info = 2,
  warn = 3,
  err = 4,
  critical = 5,
  off = 6,
};

/**
 * @brief Generate a UUID4-format correlation ID
 *
 * Uses thread-local random state for efficiency. Not cryptographically secure
 * but suitable for log correlation across a single event lifecycle.
 */
std::string generateCorrelationId();

/**
 * @brief LogContext - Thread-local context for distributed logging
 *
 * Provides thread-local context information (agent_id, worker_id, session_id,
 * correlation_id) that is automatically included in all log messages from that
 * thread.
 */
class LogContext {
 public:
  static void set(const std::string& agent_id, int32_t worker_id, const std::string& session_id);
  static void clear();
  static std::string getAgentId();
  static int32_t getWorkerId();
  static std::string getSessionId();
  static void setCorrelationId(const std::string& correlation_id);
  static void clearCorrelationId();
  static std::string getCorrelationId();
  static std::string getContextString();

 private:
  struct Context {
    std::string agent_id;
    int32_t worker_id = -1;
    std::string session_id;
    std::string correlation_id;
  };

  static thread_local Context context_;
};

/**
 * @brief RAII guard that sets a correlation ID for the duration of a scope.
 *
 * Non-moveable by design: restoring the previous correlation ID must happen at
 * a deterministic point. To carry an ID across async boundaries, capture a
 * std::string copy of id() and re-set it on the remote thread.
 */
class CorrelationScope {
 public:
  explicit CorrelationScope();
  explicit CorrelationScope(std::string correlation_id);

  CorrelationScope(const CorrelationScope&) = delete;
  CorrelationScope& operator=(const CorrelationScope&) = delete;
  CorrelationScope(CorrelationScope&&) = delete;
  CorrelationScope& operator=(CorrelationScope&&) = delete;

  ~CorrelationScope();

  const std::string& id() const noexcept { return current_id_; }

 private:
  std::string previous_id_;
  std::string current_id_;
};

namespace detail {

/**
 * @brief Minimal brace-style formatter supporting the `{}` placeholders used by
 * the ported runtime. Sequential `{}` are replaced left-to-right by the
 * stringified arguments. Surplus arguments are ignored; surplus placeholders
 * are left intact. This is intentionally a tiny subset of fmt — only what the
 * ported HMAS code relies on.
 */
inline void formatAppend(std::string& out, const std::string& fmt) { out += fmt; }

template <typename T, typename... Rest>
inline void formatAppend(std::string& out, const std::string& fmt, T&& value, Rest&&... rest) {
  const std::string placeholder = "{}";
  auto pos = fmt.find(placeholder);
  if (pos == std::string::npos) {
    out += fmt;
    return;
  }
  out += fmt.substr(0, pos);
  std::ostringstream oss;
  oss << std::forward<T>(value);
  out += oss.str();
  formatAppend(out, fmt.substr(pos + placeholder.size()), std::forward<Rest>(rest)...);
}

// Zero-argument overload: a format string with no substitution arguments is
// emitted verbatim. Providing this as a non-template overload (rather than
// relying on an empty `Args...` pack instantiation) keeps the variadic
// template below from ever instantiating with an empty pack.
inline std::string format(const std::string& fmt) { return fmt; }

template <typename T, typename... Args>
inline std::string format(const std::string& fmt, T&& value, Args&&... args) {
  std::string out;
  formatAppend(out, fmt, std::forward<T>(value), std::forward<Args>(args)...);
  return out;
}

}  // namespace detail

/**
 * @brief Logger - self-contained structured logger with context injection.
 *
 * Drop-in replacement for the Keystone spdlog-backed Logger: same static
 * methods and the same `{}` placeholder syntax, but with no third-party
 * dependency. Output goes to stderr, prefixed with the thread-local context.
 */
class Logger {
 public:
  static void init(LogLevel level = LogLevel::info);
  static void shutdown();
  static void setLevel(LogLevel level);

  // Each severity method has a non-template zero-argument overload (the format
  // string is emitted verbatim) plus a variadic overload that requires at least
  // one substitution argument. Splitting them this way means the variadic
  // template is never instantiated with an empty parameter pack.
  static void trace(const std::string& fmt) { log(LogLevel::trace, fmt); }

  template <typename T, typename... Args>
  static void trace(const std::string& fmt, T&& value, Args&&... args) {
    log(LogLevel::trace, fmt, std::forward<T>(value), std::forward<Args>(args)...);
  }

  static void debug(const std::string& fmt) { log(LogLevel::debug, fmt); }

  template <typename T, typename... Args>
  static void debug(const std::string& fmt, T&& value, Args&&... args) {
    log(LogLevel::debug, fmt, std::forward<T>(value), std::forward<Args>(args)...);
  }

  static void info(const std::string& fmt) { log(LogLevel::info, fmt); }

  template <typename T, typename... Args>
  static void info(const std::string& fmt, T&& value, Args&&... args) {
    log(LogLevel::info, fmt, std::forward<T>(value), std::forward<Args>(args)...);
  }

  static void warn(const std::string& fmt) { log(LogLevel::warn, fmt); }

  template <typename T, typename... Args>
  static void warn(const std::string& fmt, T&& value, Args&&... args) {
    log(LogLevel::warn, fmt, std::forward<T>(value), std::forward<Args>(args)...);
  }

  static void error(const std::string& fmt) { log(LogLevel::err, fmt); }

  template <typename T, typename... Args>
  static void error(const std::string& fmt, T&& value, Args&&... args) {
    log(LogLevel::err, fmt, std::forward<T>(value), std::forward<Args>(args)...);
  }

  static void critical(const std::string& fmt) { log(LogLevel::critical, fmt); }

  template <typename T, typename... Args>
  static void critical(const std::string& fmt, T&& value, Args&&... args) {
    log(LogLevel::critical, fmt, std::forward<T>(value), std::forward<Args>(args)...);
  }

 private:
  static LogLevel level_;
  static std::mutex mutex_;

  static void emit(LogLevel level, const std::string& message);

  static void log(LogLevel level, const std::string& fmt) {
    if (static_cast<int>(level) < static_cast<int>(level_)) {
      return;
    }
    std::string context = LogContext::getContextString();
    emit(level, context + " " + detail::format(fmt));
  }

  template <typename T, typename... Args>
  static void log(LogLevel level, const std::string& fmt, T&& value, Args&&... args) {
    if (static_cast<int>(level) < static_cast<int>(level_)) {
      return;
    }
    std::string context = LogContext::getContextString();
    std::string body = detail::format(fmt, std::forward<T>(value), std::forward<Args>(args)...);
    emit(level, context + " " + body);
  }
};

}  // namespace concurrency
}  // namespace keystone
