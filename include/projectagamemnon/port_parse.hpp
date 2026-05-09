#pragma once
#include <charconv>
#include <cstring>
#include <optional>

namespace projectagamemnon {

struct PortParseResult {
  std::optional<int> port;
  const char* error = nullptr;  // non-owning string literal
};

inline PortParseResult parse_port(const char* str) {
  if (!str || !*str) return {std::nullopt, "empty"};
  int parsed = 0;
  const auto* end = str + std::strlen(str);
  auto [ptr, ec] = std::from_chars(str, end, parsed);
  if (ec != std::errc{} || ptr != end) return {std::nullopt, "not a valid integer"};
  if (parsed < 1 || parsed > 65535) return {std::nullopt, "out of range [1,65535]"};
  return {parsed, nullptr};
}

}  // namespace projectagamemnon
