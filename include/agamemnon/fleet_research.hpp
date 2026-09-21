#pragma once

#include <memory>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"

namespace httplib {
class Server;
}

namespace agamemnon {
class Store;
class AuthMiddleware;

struct NestorIntakeResponse {
  int status;
  std::string body;
};

/// The sole external authority seam. Implementations perform a canonical GET only.
class NestorIntakeSource {
 public:
  virtual ~NestorIntakeSource() = default;
  virtual NestorIntakeResponse lookup(const std::string& intake_id) = 0;
};

struct NestorResearchConfig {
  std::string origin;
  std::string api_key;
  std::string authority_namespace;
};

std::optional<NestorResearchConfig> research_import_configuration(
    const std::optional<std::string>& origin, const std::optional<std::string>& api_key,
    const std::optional<std::string>& authority_namespace, bool github_enabled);

class CurlNestorIntakeSource : public NestorIntakeSource {
 public:
  explicit CurlNestorIntakeSource(NestorResearchConfig config);
  NestorIntakeResponse lookup(const std::string& intake_id) override;

 private:
  NestorResearchConfig config_;
};

struct ResearchImportResponse {
  int status;
  nlohmann::json body;
};
void validate_research_intake_provenance(const nlohmann::json& provenance,
                                         const std::string& task_id, const std::string& repository,
                                         int number);

/// Durable metadata admission; deliberately has no publisher or orchestrator.
class FleetResearchService {
 public:
  FleetResearchService(Store& store, std::shared_ptr<NestorIntakeSource> source,
                       std::string authority_namespace, const AuthMiddleware& auth);
  ResearchImportResponse import_request(const nlohmann::json& request);

 private:
  Store& store_;
  std::shared_ptr<NestorIntakeSource> source_;
  std::string namespace_;
};

/// Register the metadata-only research admission boundary after API middleware.
void register_fleet_research_routes(httplib::Server& server,
                                    std::shared_ptr<FleetResearchService> service = nullptr);

}  // namespace agamemnon
