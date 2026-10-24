#include "agamemnon/route_limits.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace agamemnon {

std::size_t env_size(const char* name, std::size_t def) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return def;
  }
  try {
    long long parsed = std::stoll(v);
    if (parsed <= 0) {
      std::cerr << "[agamemnon] WARNING: " << name << "=\"" << v
                << "\" must be a positive integer; using default " << def << "\n";
      return def;
    }
    return static_cast<std::size_t>(parsed);
  } catch (const std::exception&) {
    std::cerr << "[agamemnon] WARNING: " << name << "=\"" << v
              << "\" is not a valid integer; using default " << def << "\n";
    return def;
  }
}

RouteLimits RouteLimits::from_env() {
  RouteLimits l;
  l.max_name_len = env_size("AGAMEMNON_MAX_NAME_LEN", l.max_name_len);
  l.max_label_len = env_size("AGAMEMNON_MAX_LABEL_LEN", l.max_label_len);
  l.max_description_len = env_size("AGAMEMNON_MAX_DESCRIPTION_LEN", l.max_description_len);
  l.max_subject_len = env_size("AGAMEMNON_MAX_SUBJECT_LEN", l.max_subject_len);
  l.max_program_len = env_size("AGAMEMNON_MAX_PROGRAM_LEN", l.max_program_len);

  // Body cap precedence: AGAMEMNON_MAX_BODY_BYTES (bytes) wins. If unset, fall
  // back to the legacy SERVER_REQUEST_SIZE_LIMIT_MB (megabytes -> bytes). Else
  // keep the 1 MiB default. UNIT NOTE: the MB knob is multiplied by 1024*1024.
  if (std::getenv("AGAMEMNON_MAX_BODY_BYTES") != nullptr) {
    l.max_body_bytes = env_size("AGAMEMNON_MAX_BODY_BYTES", l.max_body_bytes);
  } else if (std::getenv("SERVER_REQUEST_SIZE_LIMIT_MB") != nullptr) {
    const std::size_t mb = env_size("SERVER_REQUEST_SIZE_LIMIT_MB", l.max_body_bytes / (1024U * 1024U));
    l.max_body_bytes = mb * 1024U * 1024U;
  }
  return l;
}

}  // namespace agamemnon
