#include "agamemnon/planning_breakdown.hpp"

#include "agamemnon/store.hpp"  // generate_uuid, now_iso8601

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace agamemnon {

ImplRef parse_impl_ref(const std::string& impl_desc) {
  ImplRef ref;
  // Leading issue ref: optional whitespace then "#123".
  size_t i = impl_desc.find_first_not_of(" \t");
  if (i == std::string::npos || impl_desc[i] != '#') return ref;
  size_t j = i + 1;
  while (j < impl_desc.size() && (std::isdigit(static_cast<unsigned char>(impl_desc[j])) != 0)) ++j;
  if (j == i + 1) return ref;
  ref.issue = std::stoi(impl_desc.substr(i + 1, j - i - 1));

  // Optional "(depends on: #A, #B)" annotation anywhere after the ref.
  const std::string marker = "(depends on:";
  const size_t dep_start = impl_desc.find(marker, j);
  if (dep_start == std::string::npos) return ref;
  const size_t dep_end = impl_desc.find(')', dep_start);
  if (dep_end == std::string::npos) return ref;
  ref.has_explicit_deps = true;
  size_t k = dep_start + marker.size();
  while (k < dep_end) {
    if (impl_desc[k] == '#') {
      size_t m = k + 1;
      while (m < dep_end && (std::isdigit(static_cast<unsigned char>(impl_desc[m])) != 0)) ++m;
      if (m > k + 1) ref.depends_on.push_back(std::stoi(impl_desc.substr(k + 1, m - k - 1)));
      k = m;
    } else {
      ++k;
    }
  }
  return ref;
}

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

        // ADR-013: impl strings may carry a GitHub issue ref plus explicit
        // dependency edges ("#123 (depends on: #456)"). Explicit annotations
        // replace the default sequential chain; issue-ref wiring happens in
        // the post-pass below once every L3 id is known.
        const ImplRef ref = parse_impl_ref(impl_desc);
        l3.issue = ref.issue;
        if (!ref.has_explicit_deps && !prev_l3_id.empty()) l3.blocked_by.push_back(prev_l3_id);

        tasks[l2_idx].child_task_ids.push_back(l3.id);
        prev_l3_id = l3.id;
        tasks.push_back(l3);
      }
    }
  }

  // ── Post-pass: wire explicit issue dependencies (ADR-013) ────────────────
  // Map every L3's issue ref to its task index, then add blocked_by edges and
  // parent child links for "(depends on: #N)" annotations. Cross-module edges
  // are supported; refs to issues outside this brief are ignored.
  std::unordered_map<int, size_t> issue_to_idx;
  for (size_t idx = 0; idx < tasks.size(); ++idx) {
    if (tasks[idx].layer == HmasLayer::L3_TaskAgent && tasks[idx].issue != 0) {
      issue_to_idx.emplace(tasks[idx].issue, idx);
    }
  }
  for (auto& t : tasks) {
    if (t.layer != HmasLayer::L3_TaskAgent || t.issue == 0) continue;
    const ImplRef ref = parse_impl_ref(t.subject);
    for (const int dep_issue : ref.depends_on) {
      auto it = issue_to_idx.find(dep_issue);
      if (it == issue_to_idx.end()) continue;
      auto& blocker = tasks[it->second];
      if (blocker.id == t.id) continue;
      if (std::find(t.blocked_by.begin(), t.blocked_by.end(), blocker.id) == t.blocked_by.end()) {
        t.blocked_by.push_back(blocker.id);
      }
      // Completion of the blocker must consider this task for delegation
      // (delegate_unblocked_children walks child_task_ids).
      if (std::find(blocker.child_task_ids.begin(), blocker.child_task_ids.end(), t.id) ==
          blocker.child_task_ids.end()) {
        blocker.child_task_ids.push_back(t.id);
      }
    }
  }

  return tasks;
}

}  // namespace agamemnon
