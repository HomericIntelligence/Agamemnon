#include "projectagamemnon/auth.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "httplib.h"

namespace projectagamemnon {

AuthMiddleware::AuthMiddleware(std::string api_key) : api_key_(std::move(api_key)) {}

bool AuthMiddleware::is_exempt(const std::string& path) const {
  return path == "/health" || path == "/v1/health";
}

bool AuthMiddleware::validate(const httplib::Request& req) const {
  if (is_exempt(req.path)) {
    return true;
  }

  // If no key is configured, allow all requests (useful for tests / local dev).
  if (api_key_.empty()) {
    return true;
  }

  // Check Authorization: Bearer <key>
  if (req.has_header("Authorization")) {
    const std::string& auth = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.size() > prefix.size() && auth.substr(0, prefix.size()) == prefix) {
      return auth.substr(prefix.size()) == api_key_;
    }
  }

  // Check X-API-Key: <key>
  if (req.has_header("X-API-Key")) {
    return req.get_header_value("X-API-Key") == api_key_;
  }

  return false;
}

}  // namespace projectagamemnon
