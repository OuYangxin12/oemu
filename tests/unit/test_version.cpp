// Tests that the version string stays in sync with the version macros.
//
// oemu_version_string() assembles its answer from the same macros a release
// edits, so a bump that forgets the string fails here. The second case pins
// that the function returns a static literal: the pointer itself must stay
// valid across calls, which callers may rely on.
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
