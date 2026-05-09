#include "projectagamemnon/version.hpp"

#include <algorithm>
#include <gtest/gtest.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "projectagamemnon/nats_client.hpp"
#include "projectagamemnon/routes.hpp"
#include "projectagamemnon/store.hpp"

#include <thread>

#include "httplib.h"

namespace projectagamemnon::test {

TEST(VersionTest, ProjectNameIsCorrect) { EXPECT_EQ(kProjectName, "ProjectAgamemnon"); }

TEST(VersionTest, VersionIsSet) { EXPECT_FALSE(kVersion.empty()); }

TEST(VersionTest, VersionMatchesCMake) {
  // kVersion must stay in sync with the VERSION field in CMakeLists.txt.
  // If this fails, update version.hpp to match CMakeLists.txt.
  EXPECT_EQ(kVersion, "0.1.0");
}

TEST(VersionTest, MajorMinorPatchConsistent) {
  EXPECT_EQ(kVersionMajor, 0);
  EXPECT_EQ(kVersionMinor, 1);
  EXPECT_EQ(kVersionPatch, 0);
}

TEST(VersionTest, VersionIsSemver) {
  // Must contain exactly two dots (M.m.p format).
  const std::string v{kVersion};
  EXPECT_EQ(std::count(v.begin(), v.end(), '.'), 2);
}

}  // namespace projectagamemnon::test
