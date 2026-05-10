#pragma once

#include <string>

// Forward declaration to avoid pulling in httplib here.
namespace httplib {
struct Request;
}

namespace projectagamemnon {

/// Validates API key authentication for incoming HTTP requests.
///
/// Checks the Authorization: Bearer <key> or X-API-Key: <key> header against
/// the configured secret. Health endpoints are exempt from authentication.
class AuthMiddleware {
 public:
  explicit AuthMiddleware(std::string api_key);

  /// Returns true if the request carries a valid API key, or if the path is
  /// exempt from authentication.
  [[nodiscard]] bool validate(const httplib::Request& req) const;

  /// Returns true if the path is exempt from authentication (health endpoints).
  [[nodiscard]] bool is_exempt(const std::string& path) const;

 private:
  std::string api_key_;
};

}  // namespace projectagamemnon
