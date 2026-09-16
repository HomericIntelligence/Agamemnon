#include "agamemnon/fleet_issue_config.hpp"

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

#include <gtest/gtest.h>

namespace {
using agamemnon::load_issue_import_configuration;
using nlohmann::json;

class FleetIssueConfig : public ::testing::Test {
 protected:
  std::filesystem::path directory;
  std::filesystem::path path;
  static constexpr std::size_t max_bytes = 65536;
  static constexpr const char* branch = "fleet/import-state";
  static constexpr const char* marker = "private-operator-registry-marker";

  void SetUp() override {
    auto pattern = (std::filesystem::temp_directory_path() / "agam-issue-config-XXXXXX").string();
    char* made = ::mkdtemp(pattern.data());
    ASSERT_NE(made, nullptr);
    // Darwin can expose its temporary directory through a /var symlink.
    directory = std::filesystem::canonical(made);
    path = directory / "private-operator-registry.json";
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  static json repositories() {
    return json::array(
        {{{"key", "work"}, {"repository", "Example/Work"}, {"repositoryId", "R_work"}},
         {{"key", "tools"}, {"repository", "Example/Tools"}, {"repositoryId", "R_tools"}}});
  }

  static json document(json entries = repositories()) {
    return {{"schema", "hi/agamemnon/issue-intake-config/v1"},
            {"repositories", std::move(entries)}};
  }

  void write(const std::string& bytes, mode_t mode = 0600) {
    if (std::filesystem::exists(path)) ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream);
    stream << bytes;
    stream.close();
    ASSERT_TRUE(stream);
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
  }

  void rejects(const std::optional<std::string>& selected,
               const std::optional<std::string>& selected_branch = branch, bool durable = true,
               bool auth = true, bool research = false) {
    try {
      (void)load_issue_import_configuration(selected, selected_branch, durable, auth, research);
      ADD_FAILURE() << "invalid operator configuration was accepted";
    } catch (const std::exception& error) {
      const std::string message = error.what();
      EXPECT_FALSE(message.empty());
      EXPECT_EQ(message.find(marker), std::string::npos);
      EXPECT_EQ(message.find(path.string()), std::string::npos);
      EXPECT_EQ(message.find("private-operator-registry"), std::string::npos);
    }
  }
};

TEST_F(FleetIssueConfig, BothAbsentDisableWithoutStartupPrerequisites) {
  for (const bool durable : {false, true}) {
    for (const bool auth : {false, true}) {
      EXPECT_EQ(load_issue_import_configuration(std::nullopt, std::nullopt, durable, auth, false),
                nullptr);
    }
  }
}

TEST_F(FleetIssueConfig, PartialEmptyAndResearchConfigurationCannotSilentlyDisable) {
  write(document().dump());
  rejects(std::nullopt, branch);
  rejects(path.string(), std::nullopt);
  rejects("", branch);
  rejects(path.string(), "");
  rejects("", "");
  rejects(std::nullopt, std::nullopt, true, true, true);
  rejects(std::nullopt, branch, true, true, true);
  rejects(path.string(), std::nullopt, true, true, true);
}

TEST_F(FleetIssueConfig, EnabledConfigurationRequiresAuthenticationAndPersistence) {
  write(document().dump());
  rejects(path.string(), branch, false, true);
  rejects(path.string(), branch, true, false);
  rejects(path.string(), branch, false, false);
  rejects(path.string(), branch, false, true, true);
  rejects(path.string(), branch, true, false, true);
}

TEST_F(FleetIssueConfig, PrivateRegistryAndBranchArePreservedForBothImportModes) {
  for (const mode_t mode : {0400, 0600}) {
    for (const bool research : {false, true}) {
      SCOPED_TRACE(mode);
      SCOPED_TRACE(research);
      write(document().dump(), mode);
      const auto result =
          load_issue_import_configuration(path.string(), branch, true, true, research);
      ASSERT_NE(result, nullptr);
      EXPECT_EQ(result->repositories, repositories());
      EXPECT_EQ(result->state_branch, branch);
    }
  }
}

TEST_F(FleetIssueConfig, ExactByteLimitIsAcceptedAndTheNextByteIsRejected) {
  auto bytes = document().dump();
  bytes.resize(max_bytes, ' ');
  write(bytes);
  const auto result = load_issue_import_configuration(path.string(), branch, true, true, false);
  EXPECT_NE(result, nullptr);
  bytes.push_back(' ');
  write(bytes);
  rejects(path.string());
}

TEST_F(FleetIssueConfig, ExplicitMissingMalformedAndDirectoryPathsAreRejected) {
  rejects((directory / "missing.json").string());
  rejects(directory.string());
  rejects(directory.string() + "/");
  write(document().dump());
  rejects(path.string() + std::string("\0ignored", 8));
}

TEST_F(FleetIssueConfig, SharedExecutableAndSpecialPermissionBitsAreRejected) {
  for (const mode_t mode : {0000, 0200, 0640, 0604, 0666, 0700, 04600, 02600, 01600}) {
    SCOPED_TRACE(mode);
    write(document().dump(), mode);
    struct stat actual {};
    ASSERT_EQ(::stat(path.c_str(), &actual), 0);
    const auto retained_mode = actual.st_mode & 07777;
    SCOPED_TRACE(retained_mode);
    // Some filesystems remove set-ID bits from an unprivileged chmod.
    if (retained_mode == 0400 || retained_mode == 0600) {
      EXPECT_NE(load_issue_import_configuration(path.string(), branch, true, true, false), nullptr);
    } else {
      rejects(path.string());
    }
  }
}

TEST_F(FleetIssueConfig, FinalAndAncestorSymlinksCannotRedirectOperatorSelection) {
  write(document().dump());
  const auto final_link = directory / "final-link.json";
  ASSERT_EQ(::symlink(path.c_str(), final_link.c_str()), 0);
  rejects(final_link.string());
  const auto nested = directory / "actual";
  ASSERT_TRUE(std::filesystem::create_directory(nested));
  const auto original = path;
  path = nested / "registry.json";
  write(document().dump());
  const auto parent_link = directory / "parent-link";
  ASSERT_EQ(::symlink(nested.c_str(), parent_link.c_str()), 0);
  rejects((parent_link / "registry.json").string());
  const auto result = load_issue_import_configuration(path.string(), branch, true, true, false);
  EXPECT_NE(result, nullptr);
  path = original;
}

TEST_F(FleetIssueConfig, HardLinksAreRejectedWithoutReadingASecondAlias) {
  write(document().dump());
  const auto alias = directory / "second-name.json";
  ASSERT_EQ(::link(path.c_str(), alias.c_str()), 0);
  rejects(path.string());
  rejects(alias.string());
}

TEST_F(FleetIssueConfig, FifoAndDescriptorAliasCannotConsumeOrBlockOnBorrowedInput) {
  const auto fifo = directory / "operator-fifo";
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  rejects(fifo.string());
  write(document().dump());
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(descriptor, 0);
  ASSERT_EQ(::lseek(descriptor, 3, SEEK_SET), 3);
  rejects("/dev/fd/" + std::to_string(descriptor));
  EXPECT_EQ(::lseek(descriptor, 0, SEEK_CUR), 3);
  EXPECT_EQ(::close(descriptor), 0);
}

TEST_F(FleetIssueConfig, DuplicateKeysAreRejectedAtEveryObjectDepth) {
  const std::string entries = repositories().dump();
  const std::vector<std::string> bytes = {
      "{\"schema\":\"hi/agamemnon/issue-intake-config/v1\",\"schema\":\"hi/agamemnon/"
      "issue-intake-config/v1\",\"repositories\":" +
          entries + "}",
      "{\"schema\":\"hi/agamemnon/issue-intake-config/v1\",\"repositories\":" + entries +
          ",\"repo\\u0073itories\":" + entries + "}",
      "{\"schema\":\"hi/agamemnon/issue-intake-config/"
      "v1\",\"repositories\":[{\"key\":\"work\",\"key\":\"work\",\"repository\":\"Example/"
      "Work\",\"repositoryId\":\"R_work\"}]}",
      "{\"schema\":\"hi/agamemnon/issue-intake-config/"
      "v1\",\"repositories\":[{\"key\":\"work\",\"repository\":\"Example/"
      "Work\",\"repositoryId\":\"R_work\",\"repositoryId\":\"R_work\"}]}"};
  for (const auto& value : bytes) {
    write(value);
    rejects(path.string());
  }
}

TEST_F(FleetIssueConfig, ClosedSchemaMalformedJsonAndInvalidUtf8AreRejectedPrivately) {
  auto extra = document();
  extra["operatorData"] = marker;
  const std::vector<json> invalid = {nullptr,
                                     json::array(),
                                     3,
                                     true,
                                     json::object(),
                                     {{"schema", "other"}, {"repositories", repositories()}},
                                     {{"repositories", repositories()}},
                                     {{"schema", "hi/agamemnon/issue-intake-config/v1"}},
                                     extra};
  for (const auto& value : invalid) {
    write(value.dump());
    rejects(path.string());
  }
  for (const auto& bytes : {std::string(), std::string("{"), document().dump() + "{}",
                            std::string("{\"schema\":\"") + char(0xff) + "\"}"}) {
    write(bytes);
    rejects(path.string());
  }
}

TEST_F(FleetIssueConfig, RegistryValidationRejectsMalformedDuplicateAndOutOfBoundsEntries) {
  const auto valid = repositories();
  auto duplicate_key = valid;
  duplicate_key[1]["key"] = valid[0]["key"];
  auto duplicate_id = valid;
  duplicate_id[1]["repositoryId"] = valid[0]["repositoryId"];
  auto alias = valid;
  alias[1]["repository"] = "example/work";
  auto invalid_key = valid;
  invalid_key[0]["key"] = "../work";
  auto invalid_name = valid;
  invalid_name[0]["repository"] = "https://github.com/Example/Work";
  auto oversized_id = valid;
  oversized_id[0]["repositoryId"] = std::string(129, 'x');
  auto extra = valid;
  extra[0]["token"] = marker;
  auto many = json::array();
  for (int index = 0; index < 65; ++index)
    many.push_back({{"key", "work-" + std::to_string(index)},
                    {"repository", "Example/Work-" + std::to_string(index)},
                    {"repositoryId", "R_" + std::to_string(index)}});
  for (const auto& entries : {json::object(), json::array(), duplicate_key, duplicate_id, alias,
                              invalid_key, invalid_name, oversized_id, extra, many}) {
    write(document(entries).dump());
    rejects(path.string());
  }
  many.erase(many.end() - 1);
  write(document(many).dump());
  const auto result = load_issue_import_configuration(path.string(), branch, true, true, false);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->repositories, many);
}

TEST_F(FleetIssueConfig, SharedStateBranchUsesTheExistingRegistryValidatorRules) {
  write(document().dump());
  for (const auto& invalid : {std::string(), std::string("/absolute"), std::string("refs/../main"),
                              std::string("branch//child"), std::string("trailing/"),
                              std::string("has space"), std::string(129, 'b')}) {
    SCOPED_TRACE(invalid);
    rejects(path.string(), invalid);
  }
  for (const auto& valid : {"main", "fleet/import-state", "state_1.2"}) {
    const auto result = load_issue_import_configuration(path.string(), valid, true, true, false);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->state_branch, valid);
  }
}
}  // namespace
