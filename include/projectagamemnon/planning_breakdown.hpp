#pragma once

#include <vector>

#include "projectagamemnon/hmas_types.hpp"

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

}  // namespace projectagamemnon
