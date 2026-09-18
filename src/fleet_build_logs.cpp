#include "agamemnon/fleet.hpp"
#include "agamemnon/fleet_build.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <curl/curl.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

namespace agamemnon::fleet_build {
namespace {
constexpr std::size_t response_limit = 400000;  // Includes JSON escaping of 64 KiB.

void require(bool condition) {
  if (!condition) throw FleetError(503, "local build log response or configuration is invalid");
}

void fields(const json& value, const std::set<std::string>& names) {
  require(value.is_object() && value.size() == names.size());
  for (const auto& [name, unused] : value.items()) require(names.contains(name));
}

bool matches(const json& value, const char* pattern) {
  return value.is_string() && std::regex_match(value.get<std::string>(), std::regex(pattern));
}

void validate_certificate(const json& value) {
  require(value.is_string());
  const auto& bytes = value.get_ref<const std::string&>();
  require(!bytes.empty() && bytes.size() <= 1024 * 1024 && bytes.find('\0') == std::string::npos);
  constexpr std::string_view whitespace = " \t\r\n\f\v";
  constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
  constexpr std::string_view end = "-----END CERTIFICATE-----";
  const auto first = bytes.find_first_not_of(whitespace);
  require(first != std::string::npos);
  const auto last = bytes.find_last_not_of(whitespace);
  const std::string_view envelope(bytes.data() + first, last - first + 1);
  // The PEM reader otherwise tolerates unrelated text and further PEM objects.
  require(envelope.starts_with(begin) && envelope.ends_with(end) &&
          envelope.find("-----BEGIN", begin.size()) == std::string_view::npos &&
          envelope.find("-----END") == envelope.size() - end.size());
  const std::unique_ptr<BIO, decltype(&BIO_free)> input(
      BIO_new_mem_buf(envelope.data(), static_cast<int>(envelope.size())), BIO_free);
  require(input != nullptr);
  const std::unique_ptr<X509, decltype(&X509_free)> certificate(
      PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
  require(certificate != nullptr);
}

std::uint64_t quantity(const json& value) {
  require(value.is_number_integer() && value >= 0 &&
          value <= std::numeric_limits<std::int64_t>::max());
  return value.get<std::uint64_t>();
}

void initialize_curl() {
  struct Lifetime {
    Lifetime() { require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK); }
    ~Lifetime() { curl_global_cleanup(); }
  };
  static const Lifetime lifetime;
}

std::size_t collect(char* data, std::size_t size, std::size_t count, void* context) {
  auto& body = *static_cast<std::string*>(context);
  if (size != 0 && count > response_limit / size) return 0;
  const auto bytes = size * count;
  if (bytes > response_limit - body.size()) return 0;
  try {
    body.append(data, bytes);
  } catch (...) {
    return 0;  // C callbacks cannot propagate C++ exceptions.
  }
  return bytes;
}

std::string byte_digest(const std::string& data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
  unsigned int size = 0;
  require(EVP_Digest(data.data(), data.size(), result.data(), &size, EVP_sha256(), nullptr) == 1);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < size; ++i) out << std::setw(2) << static_cast<int>(result[i]);
  return out.str();
}
}  // namespace

void validate_log_configuration(const json& config) {
  if (config.is_object() && config.empty()) return;
  fields(config, {"schema", "origin", "key", "caCertificatePem"});
  require(config.at("schema") == "hi/fleet/build-artifacts/v2" && config.at("origin").is_string() &&
          config.at("key").is_string());
  const auto key = config.at("key").get<std::string>();
  require(!key.empty() && key.size() <= 4096 &&
          std::all_of(key.begin(), key.end(), [](unsigned char c) { return c > 32 && c < 127; }));
  const auto origin = config.at("origin").get<std::string>();
  std::smatch matched;
  // This profile is deliberately local only. No DNS, HTTP, alternate numeric
  // host spelling, implicit port, URL credentials, path, query or fragment.
  require(std::regex_match(origin, matched,
                           std::regex(R"(https://(127\.0\.0\.1|\[::1\]):([1-9][0-9]{0,4}))")));
  const auto port = matched[2].str();
  int number = 0;
  const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
  require(error == std::errc{} && end == port.data() + port.size() && number <= 65535);
  validate_certificate(config.at("caCertificatePem"));
}

void validate_log_page(const json& page, const json& record, const std::string& stream,
                       std::uint64_t after, std::uint64_t limit) {
  fields(page, {"schema", "buildId", "attempt", "snapshotDigest", "stream", "after", "next", "data",
                "chunkDigest", "complete", "truncated", "manifest"});
  const auto& build = record.at("build");
  require(page.at("schema") == "hi/fleet/build-logs/v1" && page.at("buildId") == record.at("id") &&
          quantity(page.at("attempt")) == quantity(build.at("attempt")) &&
          page.at("snapshotDigest") == build.at("request").at("snapshot").at("manifestDigest") &&
          page.at("stream") == stream && quantity(page.at("after")) == after &&
          page.at("data").is_string() && page.at("complete").is_boolean() &&
          page.at("truncated").is_boolean());
  const auto data = page.at("data").get<std::string>();
  require(data.size() <= limit && data.size() <= std::numeric_limits<std::int64_t>::max() - after &&
          quantity(page.at("next")) == after + data.size() &&
          page.at("chunkDigest") == byte_digest(data));
  const auto& manifest = page.at("manifest");
  if (!manifest.is_null()) {
    fields(manifest, {"reference", "digest"});
    require(matches(manifest.at("reference"), "[A-Za-z0-9][A-Za-z0-9_-]{0,127}") &&
            matches(manifest.at("digest"), "[a-f0-9]{64}"));
  }
  require(!page.at("complete").get<bool>() || !manifest.is_null());
  if (build.contains("terminal") && !build.at("terminal").at("logs").is_null())
    require(manifest == build.at("terminal").at("logs"));
}

json read_logs(const json& config, const json& record, const std::string& stream,
               std::uint64_t after, std::uint64_t limit) {
  if (config.empty()) throw FleetError(503, "private build log backend is not configured");
  validate_log_configuration(config);  // Reject remote destinations before any connection.
  initialize_curl();
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
  require(curl != nullptr);
  const auto authorization = "Authorization: Bearer " + config.at("key").get<std::string>();
  const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
      curl_slist_append(nullptr, authorization.c_str()), curl_slist_free_all);
  require(headers != nullptr);
  const auto& build = record.at("build");
  const auto url =
      config.at("origin").get<std::string>() + "/v1/fleet/build-jobs/" +
      record.at("id").get<std::string>() +
      "/logs?attempt=" + std::to_string(quantity(build.at("attempt"))) + "&snapshotDigest=" +
      build.at("request").at("snapshot").at("manifestDigest").get<std::string>() +
      "&stream=" + stream + "&after=" + std::to_string(after) + "&limit=" + std::to_string(limit);
  auto option = [&](CURLoption key, auto value) {
    require(curl_easy_setopt(curl.get(), key, value) == CURLE_OK);
  };
  std::string body;
  option(CURLOPT_URL, url.c_str());
  option(CURLOPT_HTTPHEADER, headers.get());
  option(CURLOPT_HTTPGET, 1L);
  option(CURLOPT_PROTOCOLS_STR, "https");
  auto certificate = config.at("caCertificatePem").get<std::string>();
  curl_blob trust{certificate.data(), certificate.size(), CURL_BLOB_COPY};
  option(CURLOPT_SSL_VERIFYPEER, 1L);
  option(CURLOPT_SSL_VERIFYHOST, 2L);
  option(CURLOPT_CAINFO, static_cast<const char*>(nullptr));
  option(CURLOPT_CAPATH, static_cast<const char*>(nullptr));
  option(CURLOPT_SSL_OPTIONS, 0L);
  option(CURLOPT_CAINFO_BLOB, &trust);
  option(CURLOPT_FOLLOWLOCATION, 0L);
  option(CURLOPT_PROXY, "");
  option(CURLOPT_CONNECTTIMEOUT_MS, 500L);
  option(CURLOPT_TIMEOUT_MS, 1000L);
  option(CURLOPT_NOSIGNAL, 1L);
  option(CURLOPT_WRITEFUNCTION, collect);
  option(CURLOPT_WRITEDATA, &body);
  require(curl_easy_perform(curl.get()) == CURLE_OK);
  long status = 0;
  require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK);
  if (status == 409) throw FleetError(409, "private log cursor is ahead of available output");
  require(status == 200);
  try {
    std::vector<std::set<std::string>> keys;
    const auto page = json::parse(body, [&keys](int depth, json::parse_event_t event, json& value) {
      require(depth <= 16);
      if (event == json::parse_event_t::object_start) keys.emplace_back();
      if (event == json::parse_event_t::key)
        require(keys.back().insert(value.get<std::string>()).second);
      if (event == json::parse_event_t::object_end) keys.pop_back();
      return true;
    });
    validate_log_page(page, record, stream, after, limit);
    return page;
  } catch (const json::exception&) {
    throw FleetError(503, "private build log response is malformed");
  }
}
}  // namespace agamemnon::fleet_build
