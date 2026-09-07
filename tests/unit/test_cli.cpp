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

#include <sys/stat.h>
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

// --- `oemu boot` (M2c) -------------------------------------------------------
//
// Boot images here are raw little-endian instruction words -- no ELF, no cross
// toolchain, byte-built like the guest sources they imitate. Instruction-word
// oracles, P2-style: (a) encodings already GAS-verified into this repository's
// test corpus (movz xN,#imm follows the 0xD2800000|imm16<<5|Rd pattern of
// 0xD2800BC8 "mov x8,#94" above; `b .` = 0x14000000 from test_exec; str =
// 0xF9000020 and wfi = 0xD503207F from test_vcpu; brk #0 follows the
// assembler-harvested brk #0x234 = 0xD4204680 with imm16 zeroed), and (b) the
// single MOVK below, derived field-by-field from ARM ARM DDI 0487 C5.6.174
// (sf=1, opc=11: fixed 0xF2800000 | hw=1 for lsl #16 | 0x900<<5 | x1). The
// literal-data words (the sentinel byte, the UART address) are machine
// contract from src/main.c's documented layout, not decoder behaviour.
namespace boot_words {
constexpr uint32_t kMovkX1Uart = 0xF2A12001U;  // movk x1, #0x900, lsl #16
constexpr uint32_t kStrX0X1 = 0xF9000020U;     // str  x0, [x1]  (device gets low byte)
// str x0, [x1, #8]: STR immediate (ARM ARM C6.2.391: size=11 opc=01 fixed
// 0xF9000000, scaled imm14 = 8/8 = 1 at bit 10, Rn=1, Rt=0).
constexpr uint32_t kStrX0X1Off8 = 0xF9001020U;
// ldr x0, [x1]: same family, opc=01 (ARM ARM C6.2.219) -- Rt=0, Rn=1.
constexpr uint32_t kLdrX0X1 = 0xF9400020U;
constexpr uint32_t kBrk0 = 0xD4200000U;        // brk  #0
constexpr uint32_t kWfi = 0xD503207FU;         // wfi
constexpr uint32_t kBSpin = 0x14000000U;       // b    .
// movz x64,#imm16 : the pattern cited above.
constexpr uint32_t movz(const uint32_t rd, const uint32_t imm16) {
  return 0xD2800000U | ((imm16 & 0xFFFFU) << 5U) | rd;
}
}  // namespace boot_words

// Byte-level boot image from big-endian-safe hand-packed words.
std::vector<uint8_t> raw_image(const std::vector<uint32_t> &words) {
  std::vector<uint8_t> bytes;
  bytes.reserve(words.size() * 4U);
  for (const uint32_t w : words) {
    bytes.push_back(static_cast<uint8_t>(w & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((w >> 8U) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((w >> 16U) & 0xFFU));
    bytes.push_back(static_cast<uint8_t>((w >> 24U) & 0xFFU));
  }
  return bytes;
}

// x1 = 0x09000000: the stopgap UART base, built with mov x1,#0 + movk.
const std::vector<uint32_t> kUartAddress = {0xD2800001U, boot_words::kMovkX1Uart};

// A sparse file of exactly `size` bytes: ftruncate leaves holes that read
// back as zeros, so testing the 256 MiB load ceiling costs no disk and no
// memory until oemu itself streams the file.
class TempSparse {
 public:
  explicit TempSparse(const uint64_t size) {
    char tmpl[] = "/tmp/oemu-cli-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd == -1) {
      return;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
      close(fd);
      return;
    }
    close(fd);
    path_ = tmpl;
  }
  ~TempSparse() {
    if (!path_.empty()) {
      std::remove(path_.c_str());
    }
  }
  TempSparse(const TempSparse &) = delete;
  TempSparse &operator=(const TempSparse &) = delete;
  bool ok() const { return !path_.empty(); }
  const std::string &path() const { return path_; }

 private:
  std::string path_;
};

// The load ceiling from src/main.c's documented layout: RAM 0x40000000 +
// 256 MiB, image base 0x40080000. Contract, cited to the usage the CLI
// prints and the task card pins.
constexpr uint64_t kBootLoadCeiling = 0x10000000ULL - 0x80000ULL;

class CliBootTest : public CliTest {
 protected:
  // Runs the CLI with stdout captured, so the fake UART's forwarded bytes are
  // assertable as data. Reads to EOF before waiting: a full pipe buffer must
  // never deadlock the thing under test.
  struct Captured {
    bool exited = false;
    int code = -1;
    std::string out;
  };
  Captured capture_cli(const std::vector<std::string> &args) {
    Captured result;
    int fds[2];
    if (pipe(fds) != 0) {
      return result;
    }
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("oemu"));
    for (const std::string &a : args) {
      argv.push_back(const_cast<char *>(a.c_str()));
    }
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child == 0) {
      close(fds[0]);
      if (dup2(fds[1], STDOUT_FILENO) == -1) {
        _exit(127);
      }
      close(fds[1]);
      execv(exe_.c_str(), argv.data());
      _exit(127);
    }
    close(fds[1]);
    if (child <= 0) {
      close(fds[0]);
      return result;
    }
    char chunk[4096];
    ssize_t n = 0;
    while ((n = read(fds[0], chunk, sizeof(chunk))) > 0) {
      result.out.append(chunk, static_cast<std::size_t>(n));
    }
    close(fds[0]);
    int status = 0;
    if (waitpid(child, &status, 0) == child) {
      result.exited = WIFEXITED(status);
      if (result.exited) {
        result.code = WEXITSTATUS(status);
      }
    }
    return result;
  }
};

TEST_F(CliBootTest, BootSentinelImagePowersOffCleanly) {
  // mov x0,#4 ; x1=UART ; str -> sentinel EOT: poweroff(0) asked, exit 0 given.
  const std::vector<uint8_t> image =
      raw_image({boot_words::movz(0U, 0x4U), kUartAddress[0], kUartAddress[1],
                 boot_words::kStrX0X1, boot_words::kBrk0});
  const TempImage file(image);
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path()});
  EXPECT_TRUE(r.exited) << "oemu did not exit normally (killed by a signal?)";
  EXPECT_EQ(r.code, 0);
  EXPECT_TRUE(r.out.empty()) << "the sentinel must not reach stdout: " << r.out;
}

TEST_F(CliBootTest, BootUartForwardsBytesExactly) {
  // 'o' 'k' then EOT: the UART stream must arrive byte-for-byte, in order.
  // The write at offset 8 sits between them and must leave no trace -- the
  // documented stopgap behaviour is "other writes are ignored".
  const std::vector<uint8_t> image =
      raw_image({boot_words::movz(0U, 0x6FU), kUartAddress[0], kUartAddress[1],
                 boot_words::kStrX0X1, boot_words::movz(0U, 0x6BU), boot_words::kStrX0X1,
                 boot_words::kStrX0X1Off8,  // str x0, [x1, #8]: outside DR
                 boot_words::movz(0U, 0x4U), boot_words::kStrX0X1});
  const TempImage file(image);
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path()});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.out, "ok");
  EXPECT_EQ(r.code, 0);
}

TEST_F(CliBootTest, BootUartRegistersReadAsZero) {
  // Reading DR must not trap and must not stop the guest: load through the
  // UART address, then exit normally. The stopgap contract is "every register
  // reads 0"; M4a's PL011 will refine FR, guests must not care.
  const std::vector<uint8_t> image =
      raw_image({kUartAddress[0], kUartAddress[1], boot_words::kLdrX0X1,
                 boot_words::movz(0U, 0x4U), boot_words::kStrX0X1});
  const TempImage file(image);
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path()});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 0);
}

TEST_F(CliBootTest, BootDefaultEntryIsTheImageBase) {
  // The image parks at word zero: reaching the park proves the default entry
  // is 0x40080000, and the budget burning down proves it is NOT -- the exit
  // code is what distinguishes the two.
  const std::vector<uint8_t> parked =
      raw_image({boot_words::kBSpin, boot_words::movz(0U, 0x4U), kUartAddress[0],
                 kUartAddress[1], boot_words::kStrX0X1});
  const TempImage file(parked);
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path(), "--max-insns", "4096"}).code, 3);
}

TEST_F(CliBootTest, BootEntryOverrideSkipsThePark) {
  // Same image as above, entered one instruction in: the sentinel now runs.
  const std::vector<uint8_t> parked =
      raw_image({boot_words::kBSpin, boot_words::movz(0U, 0x4U), kUartAddress[0],
                 kUartAddress[1], boot_words::kStrX0X1});
  const TempImage file(parked);
  ASSERT_TRUE(file.ok());
  const Captured r =
      capture_cli({"boot", "-kernel", file.path(), "--entry", "0x40080004"});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 0);
}

TEST_F(CliBootTest, BootBlockedWfiReportsBlocked) {
  // One vCPU, masked interrupts, no wake source: a parked core is a
  // deadlock and must own its exit code (4), distinct from a budget (3).
  const TempImage file(raw_image({boot_words::kWfi}));
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path()}).code, 4);
}

TEST_F(CliBootTest, BootZeroBudgetTimesOutBeforeFirstInstruction) {
  const std::vector<uint8_t> image = raw_image(
      {boot_words::movz(0U, 0x4U), kUartAddress[0], kUartAddress[1], boot_words::kStrX0X1});
  const TempImage file(image);
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path(), "--max-insns", "0"}).code, 3);
}

TEST_F(CliBootTest, BootMissingKernelIsError) {
  EXPECT_EQ(capture_cli({"boot", "-kernel", "/nonexistent/oemu-boot-test.bin"}).code, 1);
}

TEST_F(CliBootTest, BootWithoutKernelArgumentIsUsage) {
  EXPECT_EQ(capture_cli({"boot"}).code, 2);
  EXPECT_EQ(capture_cli({"boot", "-kernel"}).code, 2);  // the flag alone: no path
  EXPECT_EQ(capture_cli({"boot", "spelled-wrong.bin"}).code, 2);
}

TEST_F(CliBootTest, BootUnknownOptionIsUsage) {
  const TempImage file(raw_image({boot_words::kBSpin}));
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path(), "--bogus"}).code, 2);
}

TEST_F(CliBootTest, BootInvalidEntryIsUsage) {
  const TempImage file(raw_image({boot_words::kBSpin}));
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path(), "--entry", "abc"}).code, 2);
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path(), "--entry", "-1"}).code, 2);
}

TEST_F(CliBootTest, BootEntryOutsideRamIsErrorNotUsage) {
  // Parses fine; the machine refuses. A caller mistake that survives parsing
  // must surface as a failure (1), not as a guest-visible abort at an address
  // no region owns, and not as usage (2) either -- the flags were all valid.
  const TempImage file(raw_image({boot_words::kBSpin}));
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path(), "--entry", "0x10"}).code, 1);
}

TEST_F(CliBootTest, BootImagePastTheLoadCeilingIsRefused) {
  // One byte past the documented ceiling: rejection must come from the size
  // contract, not from an access fault -- so the machine must be untouched
  // (exit 1, no timeout, no output) and the check must happen before any
  // instruction runs, which the sparse file makes cheap.
  const TempSparse file(kBootLoadCeiling + 1ULL);
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path(), "--max-insns", "16"});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 1);
  EXPECT_TRUE(r.out.empty());
}

}  // namespace
