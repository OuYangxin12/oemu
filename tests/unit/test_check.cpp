// Death tests: verify that contract violations abort with a useful diagnostic.
//
// gtest runs each death test in a forked child, so an abort() is observed
// without taking down the test binary. This is the main capability a pure C
// assertion framework cannot easily offer, and the reason death tests live in
// their own file here.
//
// Caveats worth knowing:
//   - the suite name should end in "DeathTest" so gtest orders it before
//     threaded suites;
//   - under ASan the default "fast" death-test style can report leaks from the
//     aborted child, so this file selects the "threadsafe" style.
#include "oemu/check.h"

#include <ctime>

#include <gtest/gtest.h>

namespace {

class CheckDeathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Re-execs the test binary in the child instead of forking mid-state.
    // Slower, but robust under sanitizers.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  }
};

TEST_F(CheckDeathTest, PassingConditionDoesNotAbort) {
  OEMU_REQUIRE(1 + 1 == 2, "arithmetic must work");
  SUCCEED();
}

TEST_F(CheckDeathTest, FailingConditionAborts) {
  EXPECT_DEATH({ OEMU_REQUIRE(false, "deliberate failure"); }, "check failed");
}

TEST_F(CheckDeathTest, DiagnosticIncludesExpressionAndMessage) {
  const int value = 0;
  EXPECT_DEATH(
      { OEMU_REQUIRE(value != 0, "value must be non-zero"); },
      "value != 0: value must be non-zero");
}

TEST_F(CheckDeathTest, DiagnosticNamesTheSourceFile) {
  EXPECT_DEATH({ OEMU_REQUIRE(false, "locate me"); }, "test_check\\.cpp:[0-9]+");
}

TEST_F(CheckDeathTest, CheckFailAbortsDirectly) {
  EXPECT_DEATH({ oemu_check_fail(__FILE__, 42, "expr", "msg"); }, "check failed: expr: msg");
}

TEST_F(CheckDeathTest, CheckFailToleratesNullArguments) {
  EXPECT_DEATH({ oemu_check_fail(nullptr, 0, nullptr, nullptr); }, "check failed");
}

// The macro must evaluate its condition exactly once: a side effect inside the
// expression is a classic macro bug.
TEST_F(CheckDeathTest, ConditionIsEvaluatedExactlyOnce) {
  int calls = 0;
  const auto bump = [&calls]() {
    ++calls;
    return true;
  };
  OEMU_REQUIRE(bump(), "must pass");
  EXPECT_EQ(1, calls);
}

// Guards against the classic unbraced-macro bug: the macro must behave as a
// single statement inside an if/else without braces.
TEST_F(CheckDeathTest, MacroIsASingleStatement) {
  // The condition comes from a runtime value on purpose: with a literal `true`
  // the else branch is provably dead and Clang rejects it under
  // -Wunreachable-code.
  bool take_first = (std::time(nullptr) != 0);
  bool else_taken = false;

  if (take_first) {
    OEMU_REQUIRE(true, "ok");
  } else {
    else_taken = true;
  }

  EXPECT_TRUE(take_first);
  EXPECT_FALSE(else_taken);
}

}  // namespace
