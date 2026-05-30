/**
 * @file logger.cpp
 * @brief Implementation of Logger, LogContext, and CorrelationScope.
 *
 * ADR-015 port of Keystone's logger with the spdlog backend replaced by a
 * tiny self-contained, thread-safe stderr writer (see logger.hpp for the
 * rationale). The LogContext / CorrelationScope / generateCorrelationId
 * behavior is preserved byte-for-byte from the Keystone original.
 */

#include "concurrency/logger.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <sstream>

namespace keystone {
namespace concurrency {

std::string generateCorrelationId() {
  // Thread-local Mersenne Twister seeded once per thread via random_device.
  // Not cryptographically secure, but sufficient for log correlation.
  thread_local std::random_device rd;
  thread_local std::mt19937 gen(rd());
  thread_local std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

  // Produce 128 bits of random data and format as UUID4
  uint32_t a = dist(gen);
  uint32_t b = dist(gen);
  uint32_t c = dist(gen);
  uint32_t d = dist(gen);

  // Set UUID version (4) and variant (10xx) bits
  b = (b & 0xFFFF0FFFu) | 0x00004000u;  // version 4
  c = (c & 0x3FFFFFFFu) | 0x80000000u;  // variant 10xx

  char buf[37];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x", a, (b >> 16) & 0xFFFF, b & 0xFFFF,
                (c >> 16) & 0xFFFF, c & 0xFFFF, d);
  return std::string(buf);
}

// LogContext thread-local storage
thread_local LogContext::Context LogContext::context_;

void LogContext::set(const std::string& agent_id, int32_t worker_id,
                     const std::string& session_id) {
  context_.agent_id = agent_id;
  context_.worker_id = worker_id;
  context_.session_id = session_id;
}

void LogContext::clear() {
  context_.agent_id.clear();
  context_.worker_id = -1;
  context_.session_id.clear();
  context_.correlation_id.clear();
}

std::string LogContext::getAgentId() { return context_.agent_id; }

int32_t LogContext::getWorkerId() { return context_.worker_id; }

std::string LogContext::getSessionId() { return context_.session_id; }

void LogContext::setCorrelationId(const std::string& correlation_id) {
  context_.correlation_id = correlation_id;
}

void LogContext::clearCorrelationId() { context_.correlation_id.clear(); }

std::string LogContext::getCorrelationId() { return context_.correlation_id; }

std::string LogContext::getContextString() {
  if (context_.agent_id.empty()) {
    return "[no-context]";
  }

  std::ostringstream oss;
  oss << "[" << context_.agent_id << ":" << context_.worker_id << ":" << context_.session_id;
  if (!context_.correlation_id.empty()) {
    oss << ":corr=" << context_.correlation_id;
  }
  oss << "]";
  return oss.str();
}

// CorrelationScope

CorrelationScope::CorrelationScope() : CorrelationScope(generateCorrelationId()) {}

CorrelationScope::CorrelationScope(std::string correlation_id)
    : previous_id_(LogContext::getCorrelationId()), current_id_(std::move(correlation_id)) {
  LogContext::setCorrelationId(current_id_);
}

CorrelationScope::~CorrelationScope() { LogContext::setCorrelationId(previous_id_); }

// Logger static members
LogLevel Logger::level_ = LogLevel::info;
std::mutex Logger::mutex_;

namespace {
const char* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::trace:
      return "trace";
    case LogLevel::debug:
      return "debug";
    case LogLevel::info:
      return "info";
    case LogLevel::warn:
      return "warn";
    case LogLevel::err:
      return "error";
    case LogLevel::critical:
      return "critical";
    case LogLevel::off:
      return "off";
  }
  return "info";
}
}  // namespace

void Logger::init(LogLevel level) { level_ = level; }

void Logger::shutdown() {}

void Logger::setLevel(LogLevel level) { level_ = level; }

void Logger::emit(LogLevel level, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cerr << "[" << levelName(level) << "] " << message << '\n';
}

}  // namespace concurrency
}  // namespace keystone
