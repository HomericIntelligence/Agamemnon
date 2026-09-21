#include "agamemnon/fleet_build.hpp"
#include "agamemnon/fleet_build_config.hpp"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "fleet_build_tls_fixture.hpp"
#include <gtest/gtest.h>

namespace {
using agamemnon::load_build_artifact_configuration;
using agamemnon::load_build_configuration;
using nlohmann::json;

class FleetBuildConfig : public ::testing::Test {
 protected:
  std::filesystem::path directory;
  std::filesystem::path path;
  static constexpr std::size_t max_bytes = 1024 * 1024;
  static constexpr const char* secret = "synthetic-authority-never-log-this";

  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "agam-build-config-XXXXXX").string();
    char* made = ::mkdtemp(pattern.data());
    ASSERT_NE(made, nullptr);
    // Darwin's temporary directory can be reached through /var -> /private/var.
    directory = std::filesystem::canonical(made);
    path = directory / "private-operator-secret.json";
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  static json authorities() {
    return {{"schema", "hi/fleet/build-authorities/v1"},
            {"authorities",
             {{{"id", "supervisor-1"},
               {"workerId", "tool-worker-1"},
               {"allocationId", "tool-allocation-1"},
               {"generation", 1},
               {"key", secret}}}}};
  }

  static json document(json catalog = json::object(), json authority = authorities()) {
    return {{"schema", "hi/fleet/build-configuration/v1"},
            {"catalog", std::move(catalog)},
            {"authorities", std::move(authority)}};
  }

  void write(const std::string& bytes, mode_t mode = 0600) {
    // Only private synthetic files are used; no services or operator inputs.
    if (std::filesystem::exists(path)) ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream);
    stream << bytes;
    stream.close();
    ASSERT_TRUE(stream);
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
  }

  void rejects(const std::optional<std::string>& selected, bool durable = true, bool auth = true,
               std::optional<std::string> state_branch = std::string("fleet-state")) {
    try {
      (void)load_build_configuration(selected, durable, auth, state_branch);
      ADD_FAILURE() << "invalid operator configuration was accepted";
    } catch (const std::exception& error) {
      const std::string message = error.what();
      EXPECT_FALSE(message.empty());
      EXPECT_EQ(message.find(secret), std::string::npos);
      EXPECT_EQ(message.find(path.string()), std::string::npos);
      EXPECT_EQ(message.find("private-operator-secret"), std::string::npos);
    }
  }

  static json artifacts(const std::string& origin = "https://127.0.0.1:43210") {
    // Generate once so repeated reads and expected JSON use the same public CA.
    static const auto certificate = fleet_build_tls_test::certificate_pem();
    return {{"schema", "hi/fleet/build-artifacts/v2"},
            {"origin", origin},
            {"key", secret},
            {"caCertificatePem", certificate}};
  }

  void rejects_artifact(const std::optional<std::string>& selected, bool durable = true,
                        bool auth = true) {
    try {
      (void)load_build_artifact_configuration(selected, durable, auth);
      ADD_FAILURE() << "invalid artifact configuration was accepted";
    } catch (const std::exception& error) {
      const std::string message = error.what();
      EXPECT_FALSE(message.empty());
      EXPECT_EQ(message.find(secret), std::string::npos);
      EXPECT_EQ(message.find(path.string()), std::string::npos);
      EXPECT_EQ(message.find("fleet-artifact-invalid-trust"), std::string::npos);
      EXPECT_EQ(message.find("-----BEGIN"), std::string::npos);
    }
  }
};

TEST_F(FleetBuildConfig, AbsentPathDisablesAdmissionWithoutStartupPrerequisites) {
  const auto result = load_build_configuration(std::nullopt, false, false);
  EXPECT_EQ(result.catalog, json::object());
  EXPECT_EQ(result.authorities, json::object());
  EXPECT_TRUE(result.state_branch.empty());
}

TEST_F(FleetBuildConfig, AbsentCatalogRetainsRecoveryBranchWithoutEnablingAdmission) {
  for (const auto& branch :
       {std::string("fleet-state"), std::string("fleet/state"),
        std::string(100, 'a') + "/" + std::string(100, 'b') + "/" + std::string(53, 'c')}) {
    SCOPED_TRACE(branch);
    const auto result = load_build_configuration(std::nullopt, true, true, branch);
    EXPECT_EQ(result.catalog, json::object());
    EXPECT_EQ(result.authorities, json::object());
    EXPECT_EQ(result.state_branch, branch);
  }
  rejects(std::nullopt, false, true);
  rejects(std::nullopt, true, false);
  rejects(std::nullopt, false, false);
}

TEST_F(FleetBuildConfig, RecoveryBranchMustBeValidWithOrWithoutAConfigurationFile) {
  write(document().dump());
  const std::vector<std::string> invalid{
      "",
      "/main",
      "main/",
      "main//state",
      "main..state",
      ".main",
      "main.",
      "main.lock",
      "main?ref=other",
      "main#fragment",
      "main:other",
      std::string(101, 'a'),
      std::string(100, 'a') + "/" + std::string(100, 'b') + "/" + std::string(54, 'c'),
      std::string("main\0other", 10)};
  for (const auto& branch : invalid) {
    SCOPED_TRACE(branch);
    rejects(std::nullopt, true, true, branch);
    rejects(path.string(), true, true, branch);
  }
}

TEST_F(FleetBuildConfig, PrivateRecoveryConfigurationPreservesAuthoritiesExactly) {
  for (const mode_t mode : {0400, 0600}) {
    SCOPED_TRACE(mode);
    write(document().dump(), mode);
    const auto result = load_build_configuration(path.string(), true, true, "fleet-state");
    EXPECT_EQ(result.catalog, json::object());
    EXPECT_EQ(result.authorities, authorities());
    EXPECT_EQ(result.state_branch, "fleet-state");
  }
}

TEST_F(FleetBuildConfig, PreservesTheCompleteAdmissionCatalogAndAuthorityBinding) {
  // Same closed policy shape as the producer/admission fixture; no real runtime
  // authority or remote input is used by this file-loading test.
  const auto gib = 1024LL * 1024 * 1024;
  const auto image = "sha256:" + std::string(64, 'f');
  const auto toolchain = std::string(64, '1');
  const json workspace = {{"id", "source"},
                          {"repository", "HomericIntelligence/Hephaestus"},
                          {"parentWorkspace", "/work/source"},
                          {"snapshotPolicyDigest", std::string(64, 'c')}};
  const json recipe = {{"id", "hephaestus-test-unit-v1"},
                       {"repository", "HomericIntelligence/Hephaestus"},
                       {"argv", {"just", "test-unit"}},
                       {"parameters", json::object()},
                       {"recipeDigest", std::string(64, 'd')},
                       {"lockDigest", std::string(64, 'e')},
                       {"platform", "linux/aarch64"},
                       {"imageDigest", image},
                       {"toolchainDigest", toolchain},
                       {"resources",
                        {{"cpus", 2},
                         {"gpus", 0},
                         {"memoryBytes", 2 * gib},
                         {"diskBytes", 4 * gib},
                         {"wallSeconds", 120},
                         {"outputBytes", 65536},
                         {"artifactBytes", 1048576},
                         {"snapshotBytes", 16777216},
                         {"snapshotMembers", 1000}}}};
  const json allocation = {
      {"id", "tool-allocation-1"},
      {"workerId", "tool-worker-1"},
      {"generation", 1},
      {"authorityId", "supervisor-1"},
      {"platform", "linux/aarch64"},
      {"imageDigest", image},
      {"toolchainDigest", toolchain},
      {"qualificationReceiptDigest", std::string(64, '2')},
      {"resources",
       {{"cpus", 18}, {"gpus", 0}, {"memoryBytes", 72 * gib}, {"diskBytes", 64 * gib}}},
      {"supervision", {{"cpus", 2}, {"memoryBytes", 8 * gib}}}};
  const json catalog = {{"schema", "hi/fleet/build-catalog/v1"},
                        {"workspaces", json::array({workspace})},
                        {"recipes", json::array({recipe})},
                        {"allocations", json::array({allocation})}};
  write(document(catalog).dump());
  rejects(path.string(), true, true, std::nullopt);
  rejects(path.string(), true, true, std::string());
  const auto result = load_build_configuration(path.string(), true, true, "fleet-state");
  EXPECT_EQ(result.catalog, catalog);
  EXPECT_EQ(result.authorities, authorities());
  EXPECT_EQ(result.state_branch, "fleet-state");
  auto incompatible = authorities();
  incompatible["authorities"][0]["workerId"] = "different-worker";
  write(document(catalog, incompatible).dump());
  rejects(path.string());
}

TEST_F(FleetBuildConfig, ConfiguredDisabledAndRecoveryModesStillRequireDurabilityAndAuth) {
  for (const auto& authority : {json::object(), authorities()}) {
    write(document(json::object(), authority).dump());
    rejects(path.string(), false, true);
    rejects(path.string(), true, false);
    rejects(path.string(), false, false);
  }
}

TEST_F(FleetBuildConfig, ExplicitMissingEmptyAndEmbeddedNulPathsDoNotFallBack) {
  rejects(path.string());
  rejects(std::string());
  write(document().dump());
  rejects(path.string() + std::string(1, '\0') + "ignored-suffix");
}

TEST_F(FleetBuildConfig, RejectsNonregularAndLinkedInputsWithoutBlocking) {
  rejects(directory.string());
  ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
  rejects(path.string());
  ASSERT_TRUE(std::filesystem::remove(path));
  write(document().dump());
  const auto alias = directory / "alias";
  std::filesystem::create_symlink(path, alias);
  rejects(alias.string());
  const auto parent_alias = directory / "parent-alias";
  std::filesystem::create_directory_symlink(directory, parent_alias);
  rejects((parent_alias / path.filename()).string());
  const auto hardlink = directory / "hardlink";
  std::filesystem::create_hard_link(path, hardlink);
  rejects(hardlink.string());
}

TEST_F(FleetBuildConfig, RejectsPermissionsBeyondOwnerReadOrReadWrite) {
  // Darwin strips set-user/group-ID bits here; its retained sticky bit exercises
  // the special-mode rejection without mistaking an accepted 0600 file for 04600.
  for (const mode_t mode : {0000, 0200, 0644, 0620, 0610, 0700, 01600}) {
    SCOPED_TRACE(mode);
    write(document().dump(), mode);
    struct stat observed {};
    ASSERT_EQ(::lstat(path.c_str(), &observed), 0);
    ASSERT_EQ(observed.st_mode & 07777, mode);
    rejects(path.string());
  }
}

TEST_F(FleetBuildConfig, RejectsDescriptorAliasesWithoutConsumingTheBorrowedFile) {
  write(document().dump());
  const int borrowed = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(borrowed, 0);
  for (const auto* prefix : {"/dev/fd/", "/proc/self/fd/"}) {
    rejects(std::string(prefix) + std::to_string(borrowed));
    EXPECT_EQ(::lseek(borrowed, 0, SEEK_CUR), 0);
  }
  EXPECT_EQ(::close(borrowed), 0);
}

TEST_F(FleetBuildConfig, EnforcesOneMiBAtTheBoundaryWithoutTruncation) {
  auto bytes = document().dump();
  bytes.resize(max_bytes, ' ');
  write(bytes);
  EXPECT_EQ(load_build_configuration(path.string(), true, true, "fleet-state").authorities,
            authorities());
  bytes.push_back(' ');
  write(bytes);
  rejects(path.string());
}

TEST_F(FleetBuildConfig, RejectsMalformedDuplicateAndDeepJsonWithoutLeakingInput) {
  const auto valid = document().dump();
  auto duplicate_generation = valid;
  const auto generation = duplicate_generation.find("\"generation\":1");
  ASSERT_NE(generation, std::string::npos);
  duplicate_generation.replace(generation, std::string("\"generation\":1").size(),
                               "\"generation\":0,\"generation\":1");
  std::vector<std::string> invalid = {
      "",
      "{",
      valid + "{}",
      "{\"" + std::string(secret) + "\":}",
      "{\"schema\":\"ignored\"," + valid.substr(1),
      "{\"schema\":\"hi/fleet/build-configuration/v1\",\"catalog\":{},"
      "\"authorities\":{\"duplicate\":1,\"duplicate\":2}}",
      duplicate_generation,
      std::string(1000, '[') + "0" + std::string(1000, ']')};
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    write(invalid[index]);
    rejects(path.string());
  }
}

TEST_F(FleetBuildConfig, RequiresTheExactClosedWrapperSchema) {
  auto missing = document();
  missing.erase("authorities");
  auto extra = document();
  extra["artifacts"] = json::object();
  auto wrong_schema = document();
  wrong_schema["schema"] = "hi/fleet/build-configuration/v2";
  for (const auto& value : {json::array(), json(nullptr), missing, extra, wrong_schema}) {
    write(value.dump());
    rejects(path.string());
  }
}

TEST_F(FleetBuildConfig, DelegatesCatalogAndRetainedAuthorityValidationBeforeStartup) {
  auto duplicate = authorities();
  duplicate["authorities"].push_back(duplicate["authorities"][0]);
  auto unknown = authorities();
  unknown["authorities"][0]["unexpected"] = secret;
  auto wrong_type = authorities();
  wrong_type["authorities"][0]["generation"] = "1";
  for (const auto& value :
       {document(json::array()), document({{"unknown", secret}}),
        document(json::object(), json::array()), document(json::object(), duplicate),
        document(json::object(), unknown), document(json::object(), wrong_type)}) {
    write(value.dump());
    rejects(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactPathAbsentIsDisabled) {
  EXPECT_EQ(load_build_artifact_configuration(std::nullopt, false, false), json::object());
}

TEST_F(FleetBuildConfig, ArtifactHttpsProfileAcceptsDedicatedTrust) {
  for (const auto* origin : {"https://127.0.0.1:43210", "https://[::1]:43210"}) {
    SCOPED_TRACE(origin);
    const auto value = artifacts(origin);
    write(value.dump(), 0400);
    EXPECT_NO_THROW(
        { EXPECT_EQ(load_build_artifact_configuration(path.string(), true, true), value); });
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsRejectsLegacyPlaintextProfiles) {
  for (const auto* origin : {"http://127.0.0.1:43210", "http://[::1]:43210"}) {
    SCOPED_TRACE(origin);
    const json legacy = {
        {"schema", "hi/fleet/build-artifacts/v1"}, {"origin", origin}, {"key", secret}};
    write(legacy.dump());
    rejects_artifact(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsRejectsPlaintextEvenWithTheV2SchemaAndTrust) {
  for (const auto* origin : {"http://127.0.0.1:43210", "http://[::1]:43210"}) {
    SCOPED_TRACE(origin);
    write(artifacts(origin).dump());
    rejects_artifact(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactConfigurationPreservesLiteralLoopbackProfiles) {
  for (const auto& value : {json::object(), artifacts(), artifacts("https://[::1]:43210"),
                            artifacts("https://127.0.0.1:1"), artifacts("https://[::1]:65535")}) {
    for (const mode_t mode : {0400, 0600}) {
      SCOPED_TRACE(mode);
      write(value.dump(), mode);
      EXPECT_EQ(load_build_artifact_configuration(path.string(), true, true), value);
    }
  }
}

TEST_F(FleetBuildConfig, ArtifactConfigurationNeedsDurabilityAndAuth) {
  for (const auto& value : {json::object(), artifacts()}) {
    write(value.dump());
    rejects_artifact(path.string(), false, true);
    rejects_artifact(path.string(), true, false);
    rejects_artifact(path.string(), false, false);
  }
}

TEST_F(FleetBuildConfig, ArtifactConfigurationUsesThePrivateBoundedJsonReader) {
  rejects_artifact(path.string());
  rejects_artifact(std::string());
  auto bytes = artifacts().dump();
  bytes.resize(max_bytes, ' ');
  write(bytes);
  EXPECT_EQ(load_build_artifact_configuration(path.string(), true, true), artifacts());
  write(bytes + " ");
  rejects_artifact(path.string());
  write(artifacts().dump(), 0644);
  rejects_artifact(path.string());
  write(artifacts().dump());
  const auto alias = directory / "artifact-alias";
  std::filesystem::create_symlink(path, alias);
  rejects_artifact(alias.string());
  // Last-value-wins parsing would otherwise leave a valid literal-loopback document.
  write("{\"key\":\"discard\"," + artifacts().dump().substr(1));
  rejects_artifact(path.string());
  write("{\"caCertificatePem\":\"discard\"," + artifacts().dump().substr(1));
  rejects_artifact(path.string());
  write("{\"" + std::string(secret) + "\":}");
  rejects_artifact(path.string());
}

TEST_F(FleetBuildConfig, ArtifactConfigurationRejectsRemoteAndUnknownProfiles) {
  for (const auto* origin : {"https://localhost:43210",
                             "https://192.0.2.1:43210",
                             "https://127.0.0.2:43210",
                             "https://127.1:43210",
                             "https://2130706433:43210",
                             "https://[0:0:0:0:0:0:0:1]:43210",
                             "https://[::ffff:127.0.0.1]:43210",
                             "https://[::1%25lo]:43210",
                             "https://user:pass@127.0.0.1:43210",
                             "https://127.0.0.1:43210/",
                             "https://127.0.0.1:43210/path",
                             "https://127.0.0.1:43210?proxy=1",
                             "https://127.0.0.1:43210#fragment",
                             "https://127.0.0.1",
                             "https://127.0.0.1:0",
                             "https://127.0.0.1:01",
                             "https://127.0.0.1:+1",
                             "https://127.0.0.1:65536",
                             "https://[::1]:65536",
                             "HTTPS://127.0.0.1:43210",
                             " https://127.0.0.1:43210",
                             "https://127.0.0.1:43210 "}) {
    SCOPED_TRACE(origin);
    write(artifacts(origin).dump());
    rejects_artifact(path.string());
  }
  auto extra = artifacts();
  extra["redirects"] = true;
  auto wrong_schema = artifacts();
  wrong_schema["schema"] = "hi/fleet/build-artifacts/v1";
  auto invalid_key = artifacts();
  invalid_key["key"] = std::string(secret) + "\n";
  for (const auto& value : {json::array(), extra, wrong_schema, invalid_key}) {
    write(value.dump());
    rejects_artifact(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsRequiresEveryClosedProfileField) {
  for (const auto* field : {"schema", "origin", "key", "caCertificatePem"}) {
    SCOPED_TRACE(field);
    auto value = artifacts();
    value.erase(field);
    write(value.dump());
    rejects_artifact(path.string());
  }
  auto trust_path = artifacts();
  trust_path["caCertificateFile"] = path.string();
  write(trust_path.dump());
  rejects_artifact(path.string());
}

TEST_F(FleetBuildConfig, ArtifactHttpsRequiresStringCertificateDataAtStartup) {
  for (const auto& certificate :
       {json(nullptr), json(false), json(1), json::array(), json::object()}) {
    auto value = artifacts();
    value["caCertificatePem"] = certificate;
    write(value.dump());
    rejects_artifact(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsRejectsMalformedCertificateAtStartup) {
  const auto certificate = artifacts().at("caCertificatePem").get<std::string>();
  const std::vector<std::string> invalid = {
      "",
      " \t\r\n",
      "fleet-artifact-invalid-trust",
      "-----BEGIN CERTIFICATE-----\nfleet-artifact-invalid-trust\n"
      "-----END CERTIFICATE-----\n",
      "-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n",
      certificate.substr(0, certificate.find("-----END CERTIFICATE-----")),
      "-----BEGIN TRUSTED CERTIFICATE-----\nfleet-artifact-invalid-trust\n"
      "-----END TRUSTED CERTIFICATE-----\n"};
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    auto value = artifacts();
    value["caCertificatePem"] = invalid[index];
    write(value.dump());
    rejects_artifact(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsRejectsAdditionalPemObjectsAtStartup) {
  const auto certificate = artifacts().at("caCertificatePem").get<std::string>();
  // Deliberately invalid envelope text, not a private key or CRL. No synthetic
  // or operator private-key bytes are written to the configuration fixture.
  const auto invalid_envelope = [](const std::string& label) {
    return "-----BEGIN " + label + "-----\nfleet-artifact-invalid-trust\n-----END " + label +
           "-----\n";
  };
  const auto key_envelope = invalid_envelope("PRIVATE KEY");
  const auto crl_envelope = invalid_envelope("X509 CRL");
  const std::vector<std::string> invalid = {certificate + certificate, certificate + key_envelope,
                                            key_envelope + certificate, certificate + crl_envelope,
                                            crl_envelope + certificate};
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    auto value = artifacts();
    value["caCertificatePem"] = invalid[index];
    write(value.dump());
    rejects_artifact(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsAllowsOnlyWhitespaceOutsideTheCertificateEnvelope) {
  const auto certificate = artifacts().at("caCertificatePem").get<std::string>();
  auto value = artifacts();
  value["caCertificatePem"] = " \t\r\n" + certificate + " \t\r\n";
  write(value.dump());
  EXPECT_EQ(load_build_artifact_configuration(path.string(), true, true), value);
  for (const auto& invalid :
       {"fleet-artifact-invalid-trust\n" + certificate,
        certificate + "fleet-artifact-invalid-trust", certificate + std::string(1, '\x7f')}) {
    value["caCertificatePem"] = invalid;
    write(value.dump());
    rejects_artifact(path.string());
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsRejectsEmbeddedAndTrailingNulCertificateData) {
  const auto certificate = artifacts().at("caCertificatePem").get<std::string>();
  for (const auto position : {std::size_t{0}, certificate.size() / 2, certificate.size()}) {
    SCOPED_TRACE(position);
    auto invalid = certificate;
    invalid.insert(position, 1, '\0');
    auto value = artifacts();
    value["caCertificatePem"] = invalid;
    write(value.dump());
    rejects_artifact(path.string());
    EXPECT_THROW(agamemnon::fleet_build::validate_log_configuration(value), std::exception);
  }
}

TEST_F(FleetBuildConfig, ArtifactHttpsBoundsTrustEvenWithoutTheStartupFileReader) {
  auto certificate = artifacts().at("caCertificatePem").get<std::string>();
  ASSERT_LT(certificate.size(), max_bytes);
  certificate.resize(max_bytes, ' ');
  auto value = artifacts();
  value["caCertificatePem"] = certificate;
  EXPECT_NO_THROW(agamemnon::fleet_build::validate_log_configuration(value));
  certificate.push_back(' ');
  value["caCertificatePem"] = certificate;
  EXPECT_THROW(agamemnon::fleet_build::validate_log_configuration(value), std::exception);
  write(value.dump());
  rejects_artifact(path.string());
}

}  // namespace
