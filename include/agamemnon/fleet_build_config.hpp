#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace agamemnon {

struct BuildConfiguration {
  nlohmann::json catalog = nlohmann::json::object();
  nlohmann::json authorities = nlohmann::json::object();
  std::string state_branch;
};

// An absent operator path disables admission. A configured path requires an
// authenticated API and durable persistence, including authority-only recovery.
// Invalid configuration throws a generic error without file or authority data.
// The state branch remains available when the admission catalog is disabled.
BuildConfiguration load_build_configuration(
    const std::optional<std::string>& path, bool durable_persistence, bool authenticated_api,
    const std::optional<std::string>& state_branch = std::nullopt);

// Optional, private literal-loopback artifact source. This only validates local
// configuration; reading a file does not connect to the configured origin.
nlohmann::json load_build_artifact_configuration(const std::optional<std::string>& path,
                                                 bool durable_persistence, bool authenticated_api);

}  // namespace agamemnon
