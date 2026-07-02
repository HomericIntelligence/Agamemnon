#pragma once

#include "projectagamemnon/hmas_types.hpp"

#include <string>
#include <vector>

namespace projectagamemnon {

/// Decomposes a TaskBrief into a flat list of HmasTasks arranged in the
/// L0 → L1 → L2 → L3 hierarchy.
///
/// Hierarchy:
///   L0 (1 root)  — owns the full brief
///     L1 (per repo)  — one per brief.repos entry
///       L2 (per module)  — one per repo's module list
///         L3 (per impl)  — one per module's impl task list (or 1 default)
///
/// All IDs are generated; parent/child linkage is populated before return.
class PlanningBreakdown {
 public:
  /// Decompose brief into a flat, ordered task list.
  /// The first element is always the L0 root.
  /// brief.id must be set by the caller before invoking decompose().
  std::vector<HmasTask> decompose(const TaskBrief& brief) const;
};

/// Parsed GitHub issue reference carried by an L3 impl string (ADR-013):
/// "#123 (depends on: #456, #789)" → issue=123, depends_on={456, 789}.
/// Non-annotated impl strings parse to issue=0 with no dependencies.
struct ImplRef {
  int issue = 0;
  std::vector<int> depends_on;
  bool has_explicit_deps = false;  // "(depends on: ...)" was present
};

/// Parse an impl string's leading "#N" issue ref and optional
/// "(depends on: #A, #B)" annotation. Exposed for unit testing.
ImplRef parse_impl_ref(const std::string& impl_desc);

}  // namespace projectagamemnon
