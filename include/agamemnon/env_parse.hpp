#pragma once
#include <cmath>
#include <cstdlib>
#include <optional>

namespace agamemnon {

// Parses a strictly-positive finite double from an env-var string.
// Returns nullopt on null/empty input, trailing junk, or non-positive
// values so callers fall back to a default instead of crashing at startup.
inline std::optional<double> parse_env_double(const char* str) {
  if (str == nullptr || *str == '\0') return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(str, &end);
  if (end == str || *end != '\0' || !(value > 0.0) || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

}  // namespace agamemnon
