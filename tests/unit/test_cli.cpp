/*
 * Black-box tests for the `oemu run` command line.
 *
 * These exercise exactly the surface the library tests cannot reach: argument
 * parsing, reading an image off disk, the stack the driver maps, and -- above all
 * -- the guest's exit code reaching the process exit status. They do it by
 * running the real `oemu` binary, not a re-implementation, so what CI checks is
 * what a user types.
 *
 * The images are the same byte-built ELF64 the loader tests use (support/
 * elf_builder.h), written to a temp file so the CLI takes its normal file path.
 * No cross toolchain is involved: the payload is three GAS-verified instructions
 * that call exit_group (mov x8,#94 = 0xd2800bc8; mov x0,#7 = 0xd28000e0;
 * mov x0,#0 = 0xd2800000; svc #0 = 0xd4000001).
 *
 * The CLI is a separate process, so every case is fork/execv/waitpid rather than
 * an in-process call. The binary is located through /proc/self/exe: it is a
 * sibling of this test executable in the same build bin/ directory.
 */
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "support/elf_builder.h"

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace oemu_test::elf;

// The `oemu` binary sits next to this test executable (both in build/<preset>/bin).
// Resolving via /proc/self/exe avoids baking a preset-specific path into the test.
std::string oemu_path() {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1U);
  if (n <= 0) {
    return {};
  }
  buf[n] = '\0';
  const std::string self(buf);
  const std::size_t slash = self.find_last_of('/');
  if (slash == std::string::npos) {
    return {};
  }
  return self.substr(0, slash + 1U) + "oemu";
}

// A static ET_EXEC AArch64 image whose whole program is `exit_group(code)`, with
// `code_mov` the GAS-verified `mov x0,#<code>` word.
std::vector<uint8_t> exit_image(uint32_t code_mov) {
  const std::vector<uint8_t> code = to_bytes({0xD2800BC8U, code_mov, 0xD4000001U});
  return build_image({SegmentSpec{0x400000U, code, (uint64_t)code.size(), kFlagRx}}, 0x400000U);
}

// Writes bytes to a fresh temp file (mkstemp) and removes it on scope exit.
class TempImage {
 public:
  explicit TempImage(const std::vector<uint8_t> &bytes) {
    char tmpl[] = "/tmp/oemu-cli-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd == -1) {
      return;
    }
    close(fd);
    path_ = tmpl;
    FILE *f = std::fopen(path_.c_str(), "wb");
    if (f == nullptr) {
      path_.clear();
      return;
    }
    const std::size_t written = std::fwrite(bytes.data(), 1U, bytes.size(), f);
    const int closed = std::fclose(f);
    if (written != bytes.size() || closed != 0) {
      std::remove(path_.c_str());
      path_.clear();
      return;
    }
  }
  ~TempImage() {
    if (!path_.empty()) {
      std::remove(path_.c_str());
    }
  }
  TempImage(const TempImage &) = delete;
  TempImage &operator=(const TempImage &) = delete;
  bool ok() const { return !path_.empty(); }
  const std::string &path() const { return path_; }

 private:
  std::string path_;
};

struct RunResult {
  bool exited = false;
  int code = -1;
};

class CliTest : public ::testing::Test {
 protected:
  void SetUp() override {
    exe_ = oemu_path();
    if (access(exe_.c_str(), X_OK) != 0) {
      GTEST_SKIP() << "oemu binary not found at " << exe_ << " (build it first)";
    }
  }

  // Runs the CLI as a child process with `args` (after argv[0]) and reports how
  // it terminated. execv failure is a distinct 127, never a status we assert on.
  RunResult run_cli(const std::vector<std::string> &args) {
    RunResult result;
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("oemu"));
    for (const std::string &a : args) {
      argv.push_back(const_cast<char *>(a.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child == 0) {
      execv(exe_.c_str(), argv.data());
      _exit(127);
    }
    EXPECT_GT(child, 0);
    int status = 0;
    if (waitpid(child, &status, 0) == child) {
      result.exited = WIFEXITED(status);
      if (result.exited) {
        result.code = WEXITSTATUS(status);
      }
    }
    return result;
  }

  std::string exe_;
};

TEST_F(CliTest, RunsProgramAndPropagatesExitSeven) {
  TempImage image(exit_image(0xD28000E0U));  // mov x0,#7
  ASSERT_TRUE(image.ok());
  const RunResult r = run_cli({"run", image.path()});
  EXPECT_TRUE(r.exited) << "oemu did not exit normally (killed by a signal? sanitizer abort?)";
  EXPECT_EQ(r.code, 7);
}

TEST_F(CliTest, RunsProgramAndPropagatesExitZero) {
  TempImage image(exit_image(0xD2800000U));  // mov x0,#0
  ASSERT_TRUE(image.ok());
  const RunResult r = run_cli({"run", image.path()});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 0);
}

TEST_F(CliTest, InstructionBudgetSurfacesAsTimeout) {
  TempImage image(exit_image(0xD28000E0U));
  ASSERT_TRUE(image.ok());
  const RunResult r = run_cli({"run", image.path(), "--max-insns", "0"});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 3);  // EXIT_TIMEOUT: the budget was spent before exit
}

TEST_F(CliTest, MissingImageIsAnError) {
  const RunResult r = run_cli({"run", "/nonexistent/oemu-cli-test.elf"});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 1);
}

TEST_F(CliTest, NotAnElfIsAnError) {
  const std::vector<uint8_t> junk{'n', 'o', 't', ' ', 'a', 'n', ' ', 'e', 'l', 'f'};
  TempImage image(junk);
  ASSERT_TRUE(image.ok());
  const RunResult r = run_cli({"run", image.path()});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 1);
}

TEST_F(CliTest, NoArgumentsIsUsage) {
  const RunResult r = run_cli({});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 2);  // EXIT_USAGE
}

TEST_F(CliTest, HelpExitsSuccess) {
  const RunResult r = run_cli({"--help"});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 0);
}

TEST_F(CliTest, UnknownOptionIsUsage) {
  const RunResult r = run_cli({"run", "/nonexistent.oemu-cli.elf", "--bogus"});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 2);  // EXIT_USAGE
}

TEST_F(CliTest, InvalidMaxInsnsIsUsage) {
  TempImage image(exit_image(0xD28000E0U));
  ASSERT_TRUE(image.ok());
  // Not a number, and a magnitude past uint64 -- both must be refused as usage.
  EXPECT_EQ(run_cli({"run", image.path(), "--max-insns", "abc"}).code, 2);
  EXPECT_EQ(run_cli({"run", image.path(), "--max-insns", "99999999999999999999999"}).code, 2);
}

}  // namespace
