#include "agamemnon/fleet_research.hpp"

#include "agamemnon/auth.hpp"
#include "agamemnon/store.hpp"
#include "agamemnon/version.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <curl/curl.h>
#include <iomanip>
#include <limits>
#include <openssl/evp.h>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "httplib.h"
#include "nlohmann/json.hpp"

namespace agamemnon {
namespace {
using json = nlohmann::json;

json parse_document(const std::string& body, std::size_t limit) {
  if (body.size() > limit) throw std::invalid_argument("invalid_document");
  std::vector<std::set<std::string>> keys;
  return json::parse(body, [&keys](int, json::parse_event_t event, json& value) {
    if (event == json::parse_event_t::object_start) keys.emplace_back();
    if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
      throw std::invalid_argument("invalid_research_import");
    if (event == json::parse_event_t::object_end) keys.pop_back();
    return true;
  });
}

json parse_request(const std::string& body) {
  const auto request = parse_document(body, 4096);
  if (!request.is_object() || request.size() != 3 ||
      request.value("schema", json()) != "hi/agamemnon/research-import/v1" ||
      !request.value("intakeId", json()).is_string() ||
      !request.value("requestDigest", json()).is_string() ||
      !std::regex_match(request["intakeId"].get<std::string>(),
                        std::regex("[a-z0-9][a-z0-9_-]{7,63}")) ||
      !std::regex_match(request["requestDigest"].get<std::string>(), std::regex("[a-f0-9]{64}")))
    throw std::invalid_argument("invalid_research_import");
  return request;
}

class ImportFailure : public std::runtime_error {
 public:
  ImportFailure(int status, const char* code) : std::runtime_error(code), status(status) {}
  int status;
};

void require_record(bool condition) {
  if (!condition) throw ImportFailure(503, "nestor_record_invalid");
}

bool matches(const json& value, const char* pattern) {
  return value.is_string() && std::regex_match(value.get<std::string>(), std::regex(pattern));
}

bool timestamp(const json& value) {
  return matches(value,
                 "[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])T"
                 "([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z");
}

json validate_intake(const json& record, const json& request, const std::string& authority) {
  require_record(record.is_object());
  const std::set<std::string> fields{"schema",         "intakeId", "requestDigest", "bodyDigest",
                                     "workRepository", "phase",    "generation",    "createdAt",
                                     "attemptId",      "issue",    "receipt"};
  for (const auto& [field, value] : record.items()) require_record(fields.contains(field));
  require_record(record.value("schema", json()) == "hi/nestor/intake/v1" &&
                 record.value("intakeId", json()) == request["intakeId"] &&
                 record.value("generation", json()).is_number_integer() &&
                 record["generation"] == 1 &&
                 matches(record.value("requestDigest", json()), "[a-f0-9]{64}") &&
                 matches(record.value("bodyDigest", json()), "[a-f0-9]{64}") &&
                 timestamp(record.value("createdAt", json())) &&
                 matches(record.value("workRepository", json()), "[a-z0-9_-]+/[a-z0-9_.-]+"));
  const auto repo = record["workRepository"].get<std::string>();
  require_record(repo.size() <= 200 && repo.find("..") == std::string::npos);
  const auto phase = record.value("phase", json());
  require_record(phase == "prepared" || phase == "creating" || phase == "created");
  if (phase == "prepared") {
    require_record(record.size() == 8 && !record.contains("attemptId") &&
                   !record.contains("issue") && !record.contains("receipt"));
  } else {
    require_record(matches(record.value("attemptId", json()), "[a-f0-9]{32}"));
  }
  if (phase == "creating")
    require_record(record.size() == 9 && !record.contains("issue") && !record.contains("receipt"));
  if (phase != "created") throw ImportFailure(409, "nestor_intake_unconfirmed");
  require_record(record.size() == 11 && record.contains("issue") && record.contains("receipt"));
  const auto& issue = record["issue"];
  const auto& receipt = record["receipt"];
  require_record(issue.is_object() && issue.size() == 3 &&
                 issue.value("repository", json()) == repo &&
                 issue.value("number", json()).is_number_integer() && issue["number"] > 0 &&
                 issue["number"] <= std::numeric_limits<int>::max());
  require_record(issue.value("url", json()) == "https://github.com/" + repo + "/issues/" +
                                                   std::to_string(issue["number"].get<int>()));
  require_record(receipt.is_object() && receipt.size() == 2 &&
                 receipt.value("kind", json()) == "confirmed_issue" &&
                 timestamp(receipt.value("observedAt", json())));
  if (record["requestDigest"] != request["requestDigest"])
    throw ImportFailure(409, "nestor_intake_changed");
  return {{"schema", "hi/agamemnon/research-intake/v1"},
          {"namespace", authority},
          {"intakeId", record["intakeId"]},
          {"requestDigest", record["requestDigest"]},
          {"bodyDigest", record["bodyDigest"]},
          {"generation", record["generation"]},
          {"attemptId", record["attemptId"]},
          {"issue", issue},
          {"createdAt", record["createdAt"]},
          {"confirmedAt", receipt["observedAt"]}};
}

std::string task_identity(const std::string& authority, const json& id) {
  const auto key = json{
      {"schema", "hi/agamemnon/research-task-key/v1"},
      {"namespace", authority},
      {"intakeId", id}}.dump();
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(key.data(), key.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1)
    throw ImportFailure(503, "research_identity_unavailable");
  std::ostringstream result;
  result << "research-" << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < size; ++index)
    result << std::setw(2) << static_cast<int>(digest[index]);
  return result.str();
}

void initialize_curl() {
  struct Lifetime {
    Lifetime() {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("Nestor transport initialization failed");
    }
    ~Lifetime() { curl_global_cleanup(); }
  };
  static const Lifetime runtime;
}

std::optional<std::string> url_part(CURLU* url, CURLUPart field) {
  char* raw = nullptr;
  const auto result = curl_url_get(url, field, &raw, 0);
  const std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
  if (result == CURLUE_OK) return std::string(raw);
  if (result == CURLUE_NO_USER || result == CURLUE_NO_PASSWORD || result == CURLUE_NO_OPTIONS ||
      result == CURLUE_NO_PORT || result == CURLUE_NO_QUERY || result == CURLUE_NO_FRAGMENT ||
      result == CURLUE_NO_ZONEID)
    return std::nullopt;
  throw std::invalid_argument("Invalid Nestor origin");
}

bool loopback_host(std::string host) {
  in_addr ipv4{};
  if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1) return (ntohl(ipv4.s_addr) >> 24U) == 127;
  if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
  in6_addr ipv6{};
  return inet_pton(AF_INET6, host.c_str(), &ipv6) == 1 && IN6_IS_ADDR_LOOPBACK(&ipv6);
}

std::string validate_origin(const std::string& origin) {
  if (origin.size() > 2048 || !(origin.starts_with("https://") || origin.starts_with("http://")) ||
      std::any_of(origin.begin(), origin.end(),
                  [](unsigned char byte) { return byte <= 32 || byte == 127; }))
    throw std::invalid_argument("Invalid Nestor origin");
  initialize_curl();
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, origin.c_str(),
                           CURLU_DISALLOW_USER | CURLU_PATH_AS_IS) != CURLUE_OK)
    throw std::invalid_argument("Invalid Nestor origin");
  const auto scheme = url_part(url.get(), CURLUPART_SCHEME).value();
  const auto host = url_part(url.get(), CURLUPART_HOST).value();
  if ((scheme != "https" && scheme != "http") || (scheme == "http" && !loopback_host(host)) ||
      url_part(url.get(), CURLUPART_PATH) != "/" || url_part(url.get(), CURLUPART_QUERY) ||
      url_part(url.get(), CURLUPART_FRAGMENT) || url_part(url.get(), CURLUPART_USER) ||
      url_part(url.get(), CURLUPART_PASSWORD) || url_part(url.get(), CURLUPART_OPTIONS) ||
      url_part(url.get(), CURLUPART_ZONEID))
    throw std::invalid_argument("Invalid Nestor origin");
  if (const auto port = url_part(url.get(), CURLUPART_PORT)) {
    int value = 0;
    const auto [end, error] = std::from_chars(port->data(), port->data() + port->size(), value);
    if (error != std::errc{} || end != port->data() + port->size() || value < 1 || value > 65535)
      throw std::invalid_argument("Invalid Nestor origin");
  }
  return origin.ends_with('/') ? origin.substr(0, origin.size() - 1) : origin;
}

std::size_t collect_nestor_body(char* data, std::size_t size, std::size_t count, void* context) {
  auto& body = *static_cast<std::string*>(context);
  if (size != 0 && count > 65536 / size) return 0;
  const auto bytes = size * count;
  if (bytes > 65536 - body.size()) return 0;
  try {
    body.append(data, bytes);
  } catch (...) {
    return 0;  // Never propagate a C++ exception through libcurl's C callback.
  }
  return bytes;
}

void reply(httplib::Response& response, int status, const char* error) {
  response.status = status;
  response.set_header("X-API-Version", std::string(kVersion));
  response.set_content(json{{"error", error}}.dump(), "application/json");
}
}  // namespace

void validate_research_intake_provenance(const json& provenance, const std::string& task_id,
                                         const std::string& repository, int number) {
  const auto issue = provenance.value("issue", json());
  if (!provenance.is_object() || provenance.size() != 10 ||
      provenance.value("schema", json()) != "hi/agamemnon/research-intake/v1" ||
      !matches(provenance.value("namespace", json()), "[a-z0-9][a-z0-9_-]{0,63}") ||
      !matches(provenance.value("intakeId", json()), "[a-z0-9][a-z0-9_-]{7,63}") ||
      !matches(provenance.value("requestDigest", json()), "[a-f0-9]{64}") ||
      !matches(provenance.value("bodyDigest", json()), "[a-f0-9]{64}") ||
      !matches(provenance.value("attemptId", json()), "[a-f0-9]{32}") ||
      !provenance.value("generation", json()).is_number_integer() ||
      provenance["generation"] != 1 || !timestamp(provenance.value("createdAt", json())) ||
      !timestamp(provenance.value("confirmedAt", json())) || repository.size() > 200 ||
      repository.find("..") != std::string::npos ||
      !matches(repository, "[a-z0-9_-]+/[a-z0-9_.-]+") || number <= 0 || !issue.is_object() ||
      issue.size() != 3 || issue.value("repository", json()) != repository ||
      !issue.value("number", json()).is_number_integer() || issue["number"] != number ||
      issue.value("url", json()) !=
          "https://github.com/" + repository + "/issues/" + std::to_string(number) ||
      task_id != task_identity(provenance["namespace"].get<std::string>(), provenance["intakeId"]))
    throw std::invalid_argument("Invalid retained research import provenance");
}

std::optional<NestorResearchConfig> research_import_configuration(
    const std::optional<std::string>& origin, const std::optional<std::string>& api_key,
    const std::optional<std::string>& authority_namespace, bool github_enabled) {
  if (!origin && !api_key && !authority_namespace) return std::nullopt;
  if (!origin || !api_key || !authority_namespace || !github_enabled || api_key->empty() ||
      api_key->size() > 4096 ||
      !std::all_of(api_key->begin(), api_key->end(),
                   [](unsigned char byte) { return byte > 32 && byte < 127; }) ||
      !std::regex_match(*authority_namespace, std::regex("[a-z0-9][a-z0-9_-]{0,63}")))
    throw std::invalid_argument("Invalid Nestor research configuration");
  return NestorResearchConfig{validate_origin(*origin), *api_key, *authority_namespace};
}

CurlNestorIntakeSource::CurlNestorIntakeSource(NestorResearchConfig config)
    : config_(research_import_configuration(config.origin, config.api_key,
                                            config.authority_namespace, true)
                  .value()) {}

NestorIntakeResponse CurlNestorIntakeSource::lookup(const std::string& intake_id) {
  if (!std::regex_match(intake_id, std::regex("[a-z0-9][a-z0-9_-]{7,63}")))
    throw std::invalid_argument("Invalid Nestor intake identifier");
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
  if (!curl) throw std::runtime_error("Nestor transport unavailable");
  const auto authorization = "Authorization: Bearer " + config_.api_key;
  const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      curl_slist_append(nullptr, authorization.c_str()), curl_slist_free_all);
  if (!headers) throw std::runtime_error("Nestor transport unavailable");
  const auto url = config_.origin + "/v1/research/intakes/" + intake_id;
  NestorIntakeResponse response{0, ""};
  auto option = [&](CURLoption key, auto value) {
    if (curl_easy_setopt(curl.get(), key, value) != CURLE_OK)
      throw std::runtime_error("Nestor transport configuration failed");
  };
  option(CURLOPT_URL, url.c_str());
  option(CURLOPT_HTTPHEADER, headers.get());
  option(CURLOPT_HTTPGET, 1L);
  option(CURLOPT_PROTOCOLS_STR, "http,https");
  option(CURLOPT_FOLLOWLOCATION, 0L);
  option(CURLOPT_PROXY, "");
  option(CURLOPT_SSL_VERIFYPEER, 1L);
  option(CURLOPT_SSL_VERIFYHOST, 2L);
  option(CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  option(CURLOPT_TIMEOUT_MS, 5000L);
  option(CURLOPT_NOSIGNAL, 1L);
  option(CURLOPT_WRITEFUNCTION, collect_nestor_body);
  option(CURLOPT_WRITEDATA, &response.body);
  if (curl_easy_perform(curl.get()) != CURLE_OK)
    throw std::runtime_error("Nestor lookup unavailable");
  long status = 0;
  if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status < 100 ||
      status > 599)
    throw std::runtime_error("Invalid Nestor HTTP response");
  response.status = static_cast<int>(status);
  return response;
}

FleetResearchService::FleetResearchService(Store& store, std::shared_ptr<NestorIntakeSource> source,
                                           std::string authority_namespace,
                                           const AuthMiddleware& auth)
    : store_(store), source_(std::move(source)), namespace_(std::move(authority_namespace)) {
  httplib::Request unauthenticated;
  unauthenticated.path = "/v1/fleet/research-intakes";
  if (!store_.github_client() || !source_ || auth.validate(unauthenticated) ||
      !std::regex_match(namespace_, std::regex("[a-z0-9][a-z0-9_-]{0,63}")))
    throw std::invalid_argument(
        "Research import requires persistence, authentication and authority");
}

ResearchImportResponse FleetResearchService::import_request(const json& request) {
  try {
    (void)parse_request(request.dump());
  } catch (const std::exception&) {
    return {400, {{"error", "invalid_research_import"}}};
  }
  NestorIntakeResponse response;
  try {
    response = source_->lookup(request["intakeId"].get<std::string>());
  } catch (const std::exception&) {
    return {503, {{"error", "nestor_unavailable"}}};
  }
  if (response.status == 404) return {404, {{"error", "nestor_intake_not_found"}}};
  if (response.status != 200) return {503, {{"error", "nestor_unavailable"}}};
  json provenance;
  try {
    provenance = validate_intake(parse_document(response.body, 65536), request, namespace_);
  } catch (const ImportFailure& error) {
    return {error.status, {{"error", error.what()}}};
  } catch (const std::exception&) {
    return {503, {{"error", "nestor_record_invalid"}}};
  }
  try {
    HmasTask proposed{};
    proposed.id = task_identity(namespace_, request["intakeId"]);
    proposed.layer = HmasLayer::L3_TaskAgent;
    proposed.state = TaskState::Pending;
    proposed.subject = "Research intake";
    proposed.description = "Execute the referenced canonical research issue.";
    proposed.repo = provenance["issue"]["repository"].get<std::string>();
    proposed.issue = provenance["issue"]["number"].get<int>();
    proposed.created_at = now_iso8601();
    proposed.delivery["researchIntake"] = provenance;
    const auto [task, created] = store_.import_research_task(proposed);
    return {
        created ? 201 : 200,
        {{"schema", "hi/agamemnon/research-import-receipt/v1"},
         {"taskId", task.id},
         {"state", task_state_to_string(task.state)},
         {"provenance", provenance},
         {"issue", provenance["issue"]},
         {"routing", {{"domain", "research"}, {"hmasRole", "task-agent"}, {"stage", "research"}}}}};
  } catch (const std::invalid_argument& error) {
    return {409,
            {{"error", std::string(error.what()) == "work_issue_already_imported"
                           ? "work_issue_already_imported"
                           : "research_import_conflict"}}};
  } catch (const std::exception&) {
    return {503, {{"error", "research_persistence_uncertain"}}};
  }
}

void register_fleet_research_routes(httplib::Server& server,
                                    std::shared_ptr<FleetResearchService> service) {
  server.Post("/v1/fleet/research-intakes",
              [service](const httplib::Request& request, httplib::Response& response) {
                json parsed;
                try {
                  parsed = parse_request(request.body);
                } catch (const std::exception&) {
                  reply(response, 400, "invalid_research_import");
                  return;
                }
                if (!service) {
                  reply(response, 503, "research_import_unavailable");
                  return;
                }
                const auto result = service->import_request(parsed);
                response.status = result.status;
                response.set_header("X-API-Version", std::string(kVersion));
                response.set_content(result.body.dump(), "application/json");
              });
}
}  // namespace agamemnon
