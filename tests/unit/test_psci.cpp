/*
 * Black-box tests for the PSCI conduit (include/oemu/psci.h).
 *
 * The function IDs are not our invention: they are the contract the booted
 * kernel itself carries (include/uapi/linux/psci.h of linux-6.6.156), and
 * the answers mirror what the QEMU oracle's emulator returns. The case that
 * matters most is the unsupported-but-PSCI-shaped call: it must be consumed
 * with NOT_SUPPORTED. Returning "not ours" instead sent the SMC back to the
 * guest as an undefined instruction, and the kernel oopsed at the moment it
 * tried to panic-reboot.
 */
#include "oemu/psci.h"
#include "oemu/status.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

class Psci : public ::testing::Test {
 protected:
  void SetUp() override { oemu_psci_init(&psci_); }

  bool call(uint64_t fnid, uint64_t *ret) { return oemu_psci_dispatch(&psci_, fnid, ret); }

  oemu_psci psci_{};
};

TEST_F(Psci, VersionAnswersTheProbedV11) {
  uint64_t ret = 0xDEADBEEFULL;
  EXPECT_TRUE(call(OEMU_PSCI_FN_VERSION, &ret));
  EXPECT_EQ(OEMU_PSCI_VERSION, ret); /* probed off the oracle: 0x00010001 */
  EXPECT_EQ(1ULL, psci_.calls);
}

TEST_F(Psci, FeaturesAnswerZeroSoTheGuestRegistersNoSuspend) {
  /* The kernel probes FEATURES before it believes suspend or RESET2
   * exists; a zero word is the truth (we model neither) and keeps it on
   * the plain SYSTEM_RESET path. */
  uint64_t ret = 0xFFFFFFFFULL;
  EXPECT_TRUE(call(OEMU_PSCI_FN_FEATURES, &ret));
  EXPECT_EQ(0U, ret);
}

TEST_F(Psci, SystemOffHaltsWithSuccess) {
  uint64_t ret = 0xFFFFFFFFULL;
  EXPECT_TRUE(call(OEMU_PSCI_FN_SYSTEM_OFF, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_SUCCESS, ret);
  EXPECT_TRUE(psci_.halted);
  EXPECT_FALSE(psci_.reset);
}

TEST_F(Psci, SystemResetRecordsTheRequest) {
  uint64_t ret = 0xFFFFFFFFULL;
  EXPECT_TRUE(call(OEMU_PSCI_FN_SYSTEM_RESET, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_SUCCESS, ret);
  EXPECT_TRUE(psci_.reset);
  EXPECT_FALSE(psci_.halted);
}

TEST_F(Psci, CpuOnIsConsumedAndRefused) {
  /* SMP is M4b. A guest that calls CPU_ON today must hear NOT_SUPPORTED --
   * the same answer the oracle gives for a call outside its model. */
  uint64_t ret = 0;
  EXPECT_TRUE(call(OEMU_PSCI_FN_CPU_ON, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_NOT_SUPPORTED, ret);
  EXPECT_TRUE(call(OEMU_PSCI_FN_CPU_ON_64, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_NOT_SUPPORTED, ret);
  EXPECT_TRUE(call(OEMU_PSCI_FN_AFFINITY_INFO_64, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_NOT_SUPPORTED, ret);
}

TEST_F(Psci, UnknownOrdinalInsidePsciSpaceIsRefusedNotBounced) {
  /* 0x840000FF: PSCI-shaped, unimplemented. The trap is the whole test:
   * this used to return false, the SMC reached the guest as an undefined
   * instruction, and the panic-reboot path oopsed on its own swapper. */
  uint64_t ret = 0;
  EXPECT_TRUE(call(0x840000FFULL, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_NOT_SUPPORTED, ret);
}

TEST_F(Psci, SmcccFeaturesAnswerTheProbedV10) {
  /* The kernel probes SMCCC FEATURES (fast call, id 0) inside
   * psci_0_2_init before it trusts any PSCI version -- an earlier build
   * bounced it and the kernel oopsed in setup_arch. The oracle's dmesg
   * ("psci: SMC Calling Convention v1.0") fixes the answer. */
  uint64_t ret = 0;
  EXPECT_TRUE(call(OEMU_SMCCC_FEATURES, &ret));
  EXPECT_EQ(OEMU_SMCCC_VERSION, ret); /* 0x00010000, probed */
  EXPECT_TRUE(call(OEMU_SMCCC_FEATURES_64, &ret));
  EXPECT_EQ(OEMU_SMCCC_VERSION, ret);
}

TEST_F(Psci, StandardServiceCallsAreRefusedNotBounced) {
  /* ARM_SMCCC_ARCH_FEATURES (owner 0x47, id 2): we model no standard
   * features; NOT_SUPPORTED must still consume the call. */
  uint64_t ret = 0;
  EXPECT_TRUE(call(0x84470002ULL, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_NOT_SUPPORTED, ret);
  EXPECT_TRUE(call(0xC4470002ULL, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_NOT_SUPPORTED, ret);
}

TEST_F(Psci, UnknownFastCallIsRefusedNotBounced) {
  uint64_t ret = 0;
  EXPECT_TRUE(call(0x80000007ULL, &ret));
  EXPECT_EQ(OEMU_PSCI_RET_NOT_SUPPORTED, ret);
}

TEST_F(Psci, ForeignServiceIsLeftToTheGuest) {
  /* Not PSCI's, not SMCCC-std's, not a fast call: the head is somebody
   * else's owner (e.g. 0x42 HyperV), and the conduit declines. */
  uint64_t ret = 0;
  EXPECT_FALSE(call(0x84420000ULL, &ret)); /* HyperV owner */
  EXPECT_FALSE(call(0x82000000ULL, &ret)); /* not a call we know */
  EXPECT_FALSE(call(0U, &ret));            /* not a call */
  EXPECT_EQ(0ULL, psci_.calls);
}

TEST_F(Psci, NullArgumentsAreRefused) {
  uint64_t ret = 0;
  EXPECT_FALSE(oemu_psci_dispatch(nullptr, OEMU_PSCI_FN_VERSION, &ret));
  EXPECT_FALSE(oemu_psci_dispatch(&psci_, OEMU_PSCI_FN_VERSION, nullptr));
}

}  // namespace
