#include "projectagamemnon/planning_breakdown.hpp"

#include "projectagamemnon/store.hpp"  // generate_uuid, now_iso8601

namespace projectagamemnon {

std::vector<HmasTask> PlanningBreakdown::decompose(const TaskBrief& brief) const {
  std::vector<HmasTask> tasks;
  const std::string ts = now_iso8601();

  // ── L0 root ───────────────────────────────────────────────────────────────
  HmasTask root;
  root.id = generate_uuid();
  root.brief_id = brief.id;
  root.parent_task_id = "";
  root.layer = HmasLayer::L0_ChiefArchitect;
  root.state = TaskState::Pending;
  root.subject = brief.title;
  root.description = brief.description;
  root.created_at = ts;

  tasks.push_back(root);

  // ── L1 per-repo ───────────────────────────────────────────────────────────
  for (const auto& repo : brief.repos) {
    HmasTask l1;
    l1.id = generate_uuid();
    l1.brief_id = brief.id;
    l1.parent_task_id = root.id;
    l1.layer = HmasLayer::L1_ComponentLead;
    l1.state = TaskState::Pending;
    l1.repo = repo;
    l1.subject = brief.title + " [" + repo + "]";
    l1.description = "Coordinate implementation for repo: " + repo;
    l1.created_at = ts;

    // L1 is blocked by the L0 root completing its decomposition.
    l1.blocked_by.push_back(root.id);

    tasks[0].child_task_ids.push_back(l1.id);

    const int l1_idx = static_cast<int>(tasks.size());
    tasks.push_back(l1);

    // ── L2 per-module ─────────────────────────────────────────────────────
    auto mods_it = brief.modules.find(repo);
    std::vector<std::string> modules;
    if (mods_it != brief.modules.end()) modules = mods_it->second;

    // Always emit at least one L2 placeholder when no modules are listed.
    if (modules.empty()) modules.push_back("core");

    // Track previous L2 id so each module is blocked by the prior one
    // (sequential within a repo — avoids parallel module conflicts).
    std::string prev_l2_id;

    for (const auto& mod : modules) {
      HmasTask l2;
      l2.id = generate_uuid();
      l2.brief_id = brief.id;
      l2.parent_task_id = l1.id;
      l2.layer = HmasLayer::L2_ModuleLead;
      l2.state = TaskState::Pending;
      l2.repo = repo;
      l2.module = mod;
      l2.subject = brief.title + " [" + repo + "/" + mod + "]";
      l2.description = "Lead module implementation: " + mod + " in " + repo;
      l2.created_at = ts;
      l2.blocked_by.push_back(l1.id);
      if (!prev_l2_id.empty()) l2.blocked_by.push_back(prev_l2_id);

      tasks[l1_idx].child_task_ids.push_back(l2.id);
      prev_l2_id = l2.id;

      const int l2_idx = static_cast<int>(tasks.size());
      tasks.push_back(l2);

      // ── L3 per-impl ─────────────────────────────────────────────────────
      std::vector<std::string> impls;
      auto repo_it = brief.impls.find(repo);
      if (repo_it != brief.impls.end()) {
        auto mod_it = repo_it->second.find(mod);
        if (mod_it != repo_it->second.end()) impls = mod_it->second;
      }
      if (impls.empty()) impls.push_back("Implement " + mod);

      std::string prev_l3_id;
      for (const auto& impl_desc : impls) {
        HmasTask l3;
        l3.id = generate_uuid();
        l3.brief_id = brief.id;
        l3.parent_task_id = l2.id;
        l3.layer = HmasLayer::L3_TaskAgent;
        l3.state = TaskState::Pending;
        l3.repo = repo;
        l3.module = mod;
        l3.subject = impl_desc;
        l3.description = impl_desc;
        l3.created_at = ts;
        l3.blocked_by.push_back(l2.id);
        if (!prev_l3_id.empty()) l3.blocked_by.push_back(prev_l3_id);

        tasks[l2_idx].child_task_ids.push_back(l3.id);
        prev_l3_id = l3.id;
        tasks.push_back(l3);
      }
    }
  }

  return tasks;
}

}  // namespace projectagamemnon
