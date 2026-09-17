#pragma once

#include <cstdlib>
#include <string>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"

namespace agamemnon {

// NOTE(#359): per-IP token-bucket rate limiting lives in
// include/agamemnon/rate_limiter.hpp and is enforced together with
// auth in the pre-routing handler registered by register_routes()
// (src/routes.cpp). Production API-key auth uses AuthMiddleware
// (agamemnon/auth.hpp); this free function is retained for header-only callers.
//
// Returns true when the request carries a valid API key, or when auth is
// disabled (AGAMEMNON_API_KEY env var is unset or empty).
inline bool validate_api_key(const httplib::Request& req) {
  const char* key = std::getenv("AGAMEMNON_API_KEY");
  if (key == nullptr || *key == '\0') return true;
  auto it = req.headers.find("X-Api-Key");
  return it != req.headers.end() && it->second == std::string(key);
}

}  // namespace agamemnon
