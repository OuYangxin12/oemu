// Tests that the version string stays in sync with the version macros.
#include "oemu/version.h"

#include <string>

#include <gtest/gtest.h>

namespace {

TEST(Version, StringMatchesMacros) {
  const std::string expected = std::to_string(OEMU_VERSION_MAJOR) + "." +
                               std::to_string(OEMU_VERSION_MINOR) + "." +
                               std::to_string(OEMU_VERSION_PATCH);
  EXPECT_EQ(expected, oemu_version_string());
}

TEST(Version, StringIsStableAcrossCalls) {
  // Returns a static literal, so the pointer itself must not change.
  EXPECT_EQ(oemu_version_string(), oemu_version_string());
}

}  // namespace
