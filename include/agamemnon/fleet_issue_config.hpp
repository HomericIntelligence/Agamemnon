#pragma once

#include "agamemnon/fleet_issue.hpp"

#include <memory>
#include <optional>
#include <string>

namespace agamemnon {

// Load the operator's private repository registry and shared import-state branch.
// Both settings may be absent only when research import is also disabled.
// Invalid input throws a generic error without file paths or operator data.
std::shared_ptr<const IssueImportConfiguration> load_issue_import_configuration(
    const std::optional<std::string>& path, const std::optional<std::string>& state_branch,
    bool durable_persistence, bool authenticated_api, bool research_enabled);

}  // namespace agamemnon
