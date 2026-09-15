#pragma once

#include "agamemnon/github_client.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace httplib {
class Server;
}

namespace agamemnon {
class Store;
class NatsPublisher;
class Orchestrator;
class ProjectProjection;

class FleetError : public std::runtime_error {
 public:
  FleetError(int code, const std::string& message) : std::runtime_error(message), status(code) {}
  int status;
};

/// Single-controller GitHub-backed Fleet control records. No task scheduler.
/// Commands and their state transitions share an atomic issue-body write.
class FleetService {
 public:
  FleetService(Store& store, NatsPublisher& publisher, Orchestrator* orchestrator = nullptr,
               std::string resolution_key = "",
               std::shared_ptr<ProjectProjection> projects = nullptr);
  json projects_health() const;
  json reconcile_projects();
  json create(const std::string& kind, const json& body);
  json list(const std::string& kind);
  json get(const std::string& kind, const std::string& id);
  json get_command(const std::string& command_id);
  json events(std::uint64_t after);
  json command(const std::string& kind, const std::string& id, const std::string& operation,
               const json& body);
  json acknowledge(const std::string& kind, const std::string& id, const json& fact);
  json resolve(const std::string& kind, const std::string& id, const json& decision,
               const std::string& resolution_key);
  /// Subject identity is checked before accepting a worker lifecycle fact.
  json on_worker_event(const std::string& subject, const json& fact);

 private:
  Store& store_;
  NatsPublisher& publisher_;
  Orchestrator* orchestrator_;
  std::string resolution_key_;
  std::shared_ptr<IGitHubClient> github_;
  std::shared_ptr<ProjectProjection> projects_;
  std::mutex mutex_;
  bool loaded_ = false;
  std::uint64_t sequence_ = 0;
  struct Entry {
    std::string issue;
    json document;
  };
  std::map<std::string, Entry> entries_;
  void load_();
  Entry& find_(const std::string& kind, const std::string& id);
  void persist_(Entry& entry, json document, const std::string& event);
  static std::string body_(const json& document);
};

void register_fleet_routes(httplib::Server& server, std::shared_ptr<FleetService> fleet);
}  // namespace agamemnon
