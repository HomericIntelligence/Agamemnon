#pragma once

#include "agamemnon/github_client.hpp"

#include <memory>
#include <optional>
#include <string>

namespace httplib {
class Server;
}

namespace agamemnon {
class Store;
class AuthMiddleware;

struct IssueImportConfiguration {
  json repositories = json::array();
  std::string state_branch;
};

struct CanonicalWorkIssue {
  std::string repository;
  std::string repository_id;
  std::string issue_id;
  int number = 0;
  std::string url;
  bool open = false;
  std::string title;
  std::string body;
};

struct IssueImportResponse {
  int status;
  json body;
};

void validate_issue_import_configuration(const IssueImportConfiguration& configuration);
CanonicalWorkIssue resolve_work_issue(const IssueImportConfiguration& configuration,
                                      IGitHubClient& github, const std::string& repository,
                                      int number, ImportContext& context);
std::string import_work_key(const CanonicalWorkIssue& issue);
void validate_issue_intake_provenance(const json& provenance, const std::string& task_id,
                                      const std::string& repository, int number);

/// Metadata-only admission with no publisher, provider or orchestration capability.
class FleetIssueService {
 public:
  FleetIssueService(Store& store, std::shared_ptr<const IssueImportConfiguration> configuration,
                    const AuthMiddleware& auth);
  IssueImportResponse repositories() const;
  IssueImportResponse inspect(const std::string& key, int number,
                              const std::optional<std::string>& comment = std::nullopt);
  IssueImportResponse import_request(const json& request);

 private:
  Store& store_;
  std::shared_ptr<const IssueImportConfiguration> configuration_;
};

void register_fleet_issue_routes(httplib::Server& server,
                                 std::shared_ptr<FleetIssueService> service = nullptr);
}  // namespace agamemnon
