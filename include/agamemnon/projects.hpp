#pragma once

#include "agamemnon/github_client.hpp"

#include <memory>
#include <mutex>

namespace agamemnon {
/// Rebuildable projection. It never mutates canonical issue/task state.
class ProjectProjection {
 public:
  explicit ProjectProjection(std::shared_ptr<IGitHubClient> github, json config = nullptr);
  json health() const;
  json reconcile();

 private:
  std::shared_ptr<IGitHubClient> github_;
  json config_;
  mutable std::mutex status_mutex_;
  std::mutex run_mutex_;
  json status_;
};
}  // namespace agamemnon
