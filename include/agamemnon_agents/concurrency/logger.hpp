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

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

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
 * @brief Stringify a single value through an ostringstream.
 *
 * Factored out so the variadic formatter can turn its whole argument list into
 * a sequence of already-stringified pieces in one non-recursive step. This
 * keeps each argument's use a direct, unconditional read (no pack-forwarding
 * recursion for the unused-variable analysis to misread).
 */
template <typename T>
inline std::string stringify(const T& value) {
  std::ostringstream oss;
  oss << value;
  return oss.str();
}

/**
 * @brief Substitute the next `{}` placeholder in `fmt`, starting at `searchPos`.
 *
 * Appends the text up to (and the replacement for) the next placeholder onto
 * `out` and returns the position just past it, so the caller can resume the
 * scan for the following argument. If no placeholder remains the rest of `fmt`
 * is flushed and `std::string::npos` is returned to signal "no more slots".
 */
inline std::string::size_type substituteNext(std::string& out, const std::string& fmt,
                                             std::string::size_type searchPos,
                                             const std::string& replacement) {
  const std::string placeholder = "{}";
  const auto pos = fmt.find(placeholder, searchPos);
  if (pos == std::string::npos) {
    out.append(fmt, searchPos, std::string::npos);
    return std::string::npos;
  }
  out.append(fmt, searchPos, pos - searchPos);
  out += replacement;
  return pos + placeholder.size();
}

/**
 * @brief Minimal brace-style formatter supporting the `{}` placeholders used by
 * the ported runtime. Sequential `{}` are replaced left-to-right by the
 * stringified arguments. Surplus arguments are ignored; surplus placeholders
 * are left intact (zero arguments emits the format string verbatim). This is
 * intentionally a tiny subset of fmt — only what the ported HMAS code relies on.
 *
 * A single non-recursive variadic template handles every arity: the argument
 * pack is consumed once, eagerly, into `pieces` (each entry already
 * stringified), then a plain loop walks the format string substituting each
 * `{}` in turn. Because every argument is read unconditionally where the pack
 * is expanded, there is no pack-forwarding recursion and no terminal empty-pack
 * instantiation — so no suppression hack is needed to keep the argument unused.
 */
template <typename... Args>
inline std::string format(const std::string& fmt, const Args&... args) {
  const std::string pieces[] = {stringify(args)..., std::string()};
  std::string out;
  out.reserve(fmt.size());
  std::string::size_type scanPos = 0;
  // pieces has one trailing sentinel entry, so iterate only the real args.
  for (std::size_t i = 0; i < sizeof...(Args); ++i) {
    scanPos = substituteNext(out, fmt, scanPos, pieces[i]);
    if (scanPos == std::string::npos) {
      // No placeholders left: remaining args are surplus and ignored.
      return out;
    }
  }
  // Surplus placeholders (and any text after the last substitution) stay intact.
  out.append(fmt, scanPos, std::string::npos);
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

  // Each severity method is a single variadic template that handles every arity
  // (including zero substitution arguments — the format string is then emitted
  // verbatim). The argument pack is forwarded straight into `log`, which is the
  // one place the formatter is invoked, so there is no per-method dead pack.
  template <typename... Args>
  static void trace(const std::string& fmt, const Args&... args) {
    log(LogLevel::trace, fmt, args...);
  }

  template <typename... Args>
  static void debug(const std::string& fmt, const Args&... args) {
    log(LogLevel::debug, fmt, args...);
  }

  template <typename... Args>
  static void info(const std::string& fmt, const Args&... args) {
    log(LogLevel::info, fmt, args...);
  }

  template <typename... Args>
  static void warn(const std::string& fmt, const Args&... args) {
    log(LogLevel::warn, fmt, args...);
  }

  template <typename... Args>
  static void error(const std::string& fmt, const Args&... args) {
    log(LogLevel::err, fmt, args...);
  }

  template <typename... Args>
  static void critical(const std::string& fmt, const Args&... args) {
    log(LogLevel::critical, fmt, args...);
  }

 private:
  static LogLevel level_;
  static std::mutex mutex_;

  static void emit(LogLevel level, const std::string& message);

  // Single variadic sink: builds the formatted body (any arity) and emits it.
  // The pack is consumed by `detail::format`, so it is always read.
  template <typename... Args>
  static void log(LogLevel level, const std::string& fmt, const Args&... args) {
    if (static_cast<int>(level) < static_cast<int>(level_)) {
      return;
    }
    const std::string context = LogContext::getContextString();
    emit(level, context + " " + detail::format(fmt, args...));
  }
};

}  // namespace concurrency
}  // namespace keystone
