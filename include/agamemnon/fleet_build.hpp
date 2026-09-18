#pragma once

#include "agamemnon/github_client.hpp"

#include <cstdint>
#include <string>

namespace agamemnon::fleet_build {
/// Closed, metadata-only admission boundary. This never reads snapshot content.
void validate_submission(const json& request);
/// Startup validation only: empty catalog can retain authorities for recovery.
void validate_configuration(const json& catalog, const json& authorities);
json policies(const json& catalog, const json& authorities, const json& request);
std::string digest(const json& value);
bool typed(const json& record);
json start_command(const json& record);
void validate_document(const json& document);
/// Immutable admission fields remain stable through valid lifecycle updates.
json admission_identity(const json& document);
void validate_create_attempt(const json& document);
void authorize(const json& record, const json& authorities, const std::string& key);
void validate_claim(const json& claim, const json& record, const json& command);
void validate_cancel(const json& request, const json& record);
void validate_terminal(const json& fact, const json& record);
void validate_delivery(const json& request, const json& record, const json& command);
void validate_log_configuration(const json& config);
json read_logs(const json& config, const json& record, const std::string& stream,
               std::uint64_t after, std::uint64_t limit);
void validate_log_page(const json& page, const json& record, const std::string& stream,
                       std::uint64_t after, std::uint64_t limit);
}  // namespace agamemnon::fleet_build
