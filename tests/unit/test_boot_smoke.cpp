/*
 * The L2 smoke for `oemu boot`: run the freestanding AArch64 guest in
 * tests/guest/el1_smoke.S through the real CLI and assert the markers it is
 * designed to emit.
 *
 * This is the guest-level end of the M2c wrap-up: EL1 entry, VBAR plumbing,
 * the SVC/ERET round trip and the fake-UART write path -- the four things the
 * boot protocol must get right before any kernel is asked to, checked at once
 * by an external program that shares no code with src/.
 *
 * The guest image is NOT built by this target: scripts/build-guest.sh needs a
 * clang + ld.lld cross pair, and CI owns the cases that install one (task
 * card m2c-boot-cli-smoke). Without the image -- the normal state on a host
 * with no cross toolchain -- every case here SKIPs loudly, naming the command
 * that would make it run. That is the bench/guest convention, inherited
 * verbatim: a missing optional toolchain is a skip, never a failure, and
 * never a false green either -- the test names what it could not check.
 *
 * The driver watches only external signals, like every L2 test should: the
 * process exit code and the stdout stream the fake UART forwards. It peeks at
 * no guest state.
 */
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

namespace {

// Same /proc/self/exe trick as test_cli: the oemu binary is a sibling of this
// test executable inside build/<preset>/bin.
std::string exe_dir() {
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
  return self.substr(0, slash + 1U);
}

// build-guest.sh writes build/guest/*.bin; from build/<preset>/bin that is
// ../../guest. One relative contract, no configured path.
std::string oemu_path() { return exe_dir() + "oemu"; }
std::string guest_path() { return exe_dir() + "../../guest/el1_smoke.bin"; }

struct CaptureResult {
  bool exited = false;
  int code = -1;
  std::string out;
};

// Runs the CLI with stdout piped, reading to EOF before waiting so a chatty
// guest cannot deadlock on a full pipe buffer.
CaptureResult run_capture(const std::vector<std::string> &args) {
  CaptureResult result;
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
    execv(oemu_path().c_str(), argv.data());
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

// The budget guards the test, not the guest: the program is a few hundred
// instructions, and anything that runs 50M of them is stuck -- which must
// surface as EXIT_TIMEOUT (3), loudly, rather than eating the 60 s case
// timeout unnoticed.
constexpr const char *kBudget = "50000000";

TEST(BootSmoke, El1ArmVectorsSvcRoundTripAndMarker) {
  if (access(oemu_path().c_str(), X_OK) != 0) {
    GTEST_SKIP() << "oemu binary not found at " << oemu_path() << " (build it first)";
  }
  if (access(guest_path().c_str(), R_OK) != 0) {
    GTEST_SKIP() << "guest image missing: " << guest_path()
                 << " -- run scripts/build-guest.sh tests/guest/el1_smoke.S (needs clang + ld.lld)";
  }

  const CaptureResult r = run_capture({"boot", "-kernel", guest_path(), "--max-insns", kBudget});
  ASSERT_TRUE(r.exited) << "oemu did not exit normally (killed by a signal?)";
  EXPECT_NE(r.out.find("EL1"), std::string::npos) << "no EL1 self-report on the UART: " << r.out;
  EXPECT_NE(r.out.find("BOOT-OK"), std::string::npos)
      << "no BOOT-OK: the SVC/ERET round trip did not complete: " << r.out;
  EXPECT_EQ(r.out.find("BOOT-FAIL"), std::string::npos) << "the guest explicitly failed: " << r.out;
  EXPECT_EQ(r.code, 0) << "expected the EOT sentinel to power the machine off cleanly";
}

}  // namespace
