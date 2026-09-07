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
constexpr uint32_t kBrk0 = 0xD4200000U;   // brk  #0
constexpr uint32_t kWfi = 0xD503207FU;    // wfi
constexpr uint32_t kBSpin = 0x14000000U;  // b    .
// movz x64,#imm16 : the pattern cited above.
constexpr uint32_t movz(const uint32_t rd, const uint32_t imm16) {
  return 0xD2800000U | ((imm16 & 0xFFFFU) << 5U) | rd;
}
// ldr x64, [xBase, #off]: unsigned offset, scaled by 8 (ARM ARM C6.2.219:
// size=11 opc=01 fixed 0xF9400000, imm14 = off/8 at bit 10). Kept alongside
// movz for tests that hand-build a load; [[maybe_unused]] so the compiler
// does not flag it on the day no case happens to need it.
[[maybe_unused]] constexpr uint32_t ldr(const uint32_t rt, const uint32_t rn,
                                        const uint32_t off) {
  return 0xF9400000U | ((off >> 3U) << 10U) | (rn << 5U) | rt;
}
}  // namespace boot_words

// Byte-level boot image from big-endian-safe hand-packed words -- and from
// the M4a protocol round: a real Linux Image header (booting.rst fields at
// their documented offsets), the entry branch pointing past it, and the
// guest words in the text at `text_offset`. The header is data the loader
// reads, instructions the guest never executes; the branch skips it.
std::vector<uint8_t> raw_image(const std::vector<uint32_t> &words) {
  // booting.rst lets text sit at any 4 KiB-aligned offset under 2 MiB, and
  // the loader requires it inside the file. The boot tests pin the default
  // entry to 0x40080000 (mem_base 0x40000000 + text_offset), matching the
  // load address every other guest and the QEMU oracle use, so the hole is
  // 0x80000 -- exactly what a real Image carries.
  constexpr uint32_t kTextOffset = 0x80000U;
  std::vector<uint8_t> bytes;
  bytes.reserve(kTextOffset + words.size() * 4U);
  // code0: b to the text -- the loader may enter at byte 0 and asks to be skipped.
  const uint32_t branch = 0x14000000U | ((kTextOffset / 4U) & 0x03FFFFFFU);
  auto put32 = [&bytes](const uint32_t v) {
    for (unsigned i = 0U; i < 4U; i++) {
      bytes.push_back(static_cast<uint8_t>((v >> (8U * i)) & 0xFFU));
    }
  };
  auto put64 = [&bytes](const uint64_t v) {
    for (unsigned i = 0U; i < 8U; i++) {
      bytes.push_back(static_cast<uint8_t>((v >> (8U * i)) & 0xFFU));
    }
  };
  put32(branch);             // 0x00 code0: b _start
  put32(0U);                 // 0x04 res0
  put64(kTextOffset);        // 0x08 text_offset (le64)
  put64(words.size() * 4U);  // 0x10 image_size
  put64(0x2U);               // 0x18 flags: little-endian, 4K pages
  put64(0U);                 // 0x20 res1
  put64(0U);                 // 0x28 res2
  put64(0U);                 // 0x30 res3
  put32(0x644D5241U);        // 0x38 magic "ARM\x64"
  put32(0U);                 // 0x3C reserved
  while (bytes.size() < kTextOffset) {
    bytes.push_back(0U);  // the hole the loader skips
  }
  for (const uint32_t w : words) {
    put32(w);
  }
  return bytes;
}

// x1 = 0x09000000: the PL011 base, built with mov x1,#0 + movk.
const std::vector<uint32_t> kUartAddress = {0xD2800001U, boot_words::kMovkX1Uart};

// Bring the console up the way a driver must: CR = UARTEX|UARTEN (0x300) at
// register offset 0x18, so TX becomes legal and FR.TXFE can clear. Clobbers x0.
std::vector<uint32_t> uart_enable() {
  return {boot_words::movz(0U, 0x300U), 0xF9003020U};  // str x0, [x1, #0x18]
}

// PSCI SYSTEM_OFF (fnid 0x84000002) through the SMC conduit: the M4a exit
// protocol. The fnid is assembled MOVK-style -- movz loads the low halfword
// (#2), movk deposits the high one (#0x8400) -- both encodings harvested
// from clang's own disassembly of that exact pair, not hand-derived.
std::vector<uint32_t> psci_off() {
  return {0xD2800040U, 0xF2B08000U, 0xD4000003U};
}

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
// the default 1 GiB, text at +0x80000. Contract, cited to the usage the CLI
// prints and the task card pins.
constexpr uint64_t kBootLoadCeiling = 0x40000000ULL - 0x80000ULL;

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

TEST_F(CliBootTest, BootPsciSystemOffExitsCleanly) {
  // The M4a exit protocol: SMC #0 with fnid 0x84000002 (SYSTEM_OFF) is
  // answered by the PSCI conduit, powers the machine down, and oemu exits 0.
  std::vector<uint32_t> words = psci_off();
  words.push_back(boot_words::kBrk0);
  const TempImage file(raw_image(words));
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path()});
  EXPECT_TRUE(r.exited) << "oemu did not exit normally (killed by a signal?)";
  EXPECT_EQ(r.code, 0);
  EXPECT_TRUE(r.out.empty()) << "a bare SYSTEM_OFF must print nothing: " << r.out;
}

TEST_F(CliBootTest, BootUartForwardsBytesExactly) {
  // 'o' 'k' to the real PL011, in order, byte-for-byte, then SYSTEM_OFF.
  // The driver must raise x1 to the UART base before any register access;
  // a fresh vCPU boots with every register zero, so the address load comes
  // first. The write at offset 8 (FR, read-only) sits between them and must
  // leave no trace, and a DR read (empty RX) must neither trap nor print.
  std::vector<uint32_t> words = kUartAddress;
  const std::vector<uint32_t> en = uart_enable();
  words.insert(words.end(), en.begin(), en.end());
  words.push_back(boot_words::movz(0U, 0x6FU));
  words.push_back(boot_words::kStrX0X1);
  words.push_back(boot_words::movz(0U, 0x6BU));
  words.push_back(boot_words::kStrX0X1);
  words.push_back(boot_words::kStrX0X1Off8);  // str x0, [x1, #8]: a silent FR write
  words.push_back(boot_words::kLdrX0X1);      // ldr x0, [x1]: empty RX reads 0
  const std::vector<uint32_t> off = psci_off();
  words.insert(words.end(), off.begin(), off.end());
  const TempImage file(raw_image(words));
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path()});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 0);
  EXPECT_EQ(r.out, "ok");
}

TEST_F(CliBootTest, BootUartResetStateReadsClean) {
  // Reading DR at reset must not trap and must not stop the guest: the PL011
  // answers 0 (RX FIFO empty). The exit travels the real protocol.
  std::vector<uint32_t> words = kUartAddress;
  const std::vector<uint32_t> en = uart_enable();
  words.insert(words.end(), en.begin(), en.end());
  words.push_back(boot_words::kLdrX0X1);  // ldr x0, [x1] -> 0, no trap
  const std::vector<uint32_t> off = psci_off();
  words.insert(words.end(), off.begin(), off.end());
  const TempImage file(raw_image(words));
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path()});
  EXPECT_TRUE(r.exited);
  EXPECT_EQ(r.code, 0);
}

TEST_F(CliBootTest, BootDefaultEntryIsTheImageBase) {
  // The image parks at word zero: reaching the park proves the default entry
  // is 0x40080000, and the budget burning down proves it is NOT -- the exit
  // code is what distinguishes the two.
  std::vector<uint32_t> tail = psci_off();
  tail.push_back(boot_words::kBrk0);
  std::vector<uint32_t> words = {boot_words::kBSpin};
  words.insert(words.end(), tail.begin(), tail.end());
  const std::vector<uint8_t> parked = raw_image(words);
  const TempImage file(parked);
  ASSERT_TRUE(file.ok());
  EXPECT_EQ(capture_cli({"boot", "-kernel", file.path(), "--max-insns", "4096"}).code, 3);
}

TEST_F(CliBootTest, BootEntryOverrideSkipsThePark) {
  // Same image, entered one instruction past the park: SYSTEM_OFF now runs.
  std::vector<uint32_t> tail = psci_off();
  tail.push_back(boot_words::kBrk0);
  std::vector<uint32_t> words = {boot_words::kBSpin};
  words.insert(words.end(), tail.begin(), tail.end());
  const std::vector<uint8_t> parked = raw_image(words);
  const TempImage file(parked);
  ASSERT_TRUE(file.ok());
  const Captured r = capture_cli({"boot", "-kernel", file.path(), "--entry", "0x40080004"});
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
