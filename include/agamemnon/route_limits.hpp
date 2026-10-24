#pragma once

#include <cstddef>

namespace agamemnon {

/// Configurable input-length and body-size limits for /v1 route handlers.
/// Defaults preserve the historical compile-time constants exactly (#275).
struct RouteLimits {
  std::size_t max_name_len = 256;
  std::size_t max_label_len = 256;
  std::size_t max_description_len = 4096;
  std::size_t max_subject_len = 512;
  std::size_t max_program_len = 1024;
  std::size_t max_body_bytes = 1U * 1024U * 1024U;  // 1 MiB

  /// Build limits from AGAMEMNON_* env vars, falling back to the defaults above.
  /// Non-numeric / zero / negative overrides are rejected (stderr warning) and
  /// the default is kept. Body cap precedence:
  ///   AGAMEMNON_MAX_BODY_BYTES (bytes) > SERVER_REQUEST_SIZE_LIMIT_MB (MiB) > 1 MiB.
  /// Env vars: AGAMEMNON_MAX_NAME_LEN, AGAMEMNON_MAX_LABEL_LEN,
  ///   AGAMEMNON_MAX_DESCRIPTION_LEN, AGAMEMNON_MAX_SUBJECT_LEN,
  ///   AGAMEMNON_MAX_PROGRAM_LEN, AGAMEMNON_MAX_BODY_BYTES.
  static RouteLimits from_env();
};

/// Read a positive size from `name`; returns `def` (with a stderr warning)
/// when unset, empty, non-numeric, or <= 0. Exposed for unit testing.
std::size_t env_size(const char* name, std::size_t def);

}  // namespace agamemnon
