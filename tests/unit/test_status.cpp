// Tests for oemu_status_str.
#include "oemu/status.h"

#include <string>

#include <gtest/gtest.h>

namespace {

TEST(StatusStr, ReturnsDescriptionForEveryKnownCode) {
  EXPECT_STREQ("ok", oemu_status_str(OEMU_OK));
  EXPECT_STREQ("invalid argument", oemu_status_str(OEMU_ERR_INVALID_ARG));
  EXPECT_STREQ("out of memory", oemu_status_str(OEMU_ERR_NO_MEMORY));
  EXPECT_STREQ("size overflow", oemu_status_str(OEMU_ERR_OVERFLOW));
  EXPECT_STREQ("out of range", oemu_status_str(OEMU_ERR_RANGE));
}

TEST(StatusStr, ReturnsFallbackForUnknownCode) {
  // Uses the value right after the last enumerator: it is unmapped, yet still
  // inside the enum's representable range, so the conversion is well defined.
  // A far-out value such as 9999 would be an unspecified conversion (-Wconversion).
  const auto unmapped = static_cast<oemu_status>(static_cast<int>(OEMU_ERR_RANGE) + 1);
  EXPECT_STREQ("unknown status", oemu_status_str(unmapped));
}

// Parameterised test: same invariant checked across all valid codes. This is
// the kind of table-driven case gtest makes cheap.
class StatusStrContract : public ::testing::TestWithParam<oemu_status> {};

TEST_P(StatusStrContract, NeverReturnsNullOrEmpty) {
  const char *text = oemu_status_str(GetParam());
  ASSERT_NE(nullptr, text);
  EXPECT_FALSE(std::string(text).empty());
}

INSTANTIATE_TEST_SUITE_P(AllCodes, StatusStrContract,
                         ::testing::Values(OEMU_OK, OEMU_ERR_INVALID_ARG, OEMU_ERR_NO_MEMORY,
                                           OEMU_ERR_OVERFLOW, OEMU_ERR_RANGE));

}  // namespace
