/*
 * White-box tests for src/vcpu/vcpu_internal.h: the interrupt-delivery
 * decision -- priority and DAIF gating -- enumerated at every input corner.
 * Everything else in the vCPU is exercised through the black-box suite.
 */
#include "oemu/exc.h"
#include "oemu/sysreg.h"

#include <gtest/gtest.h>

#include "vcpu/vcpu_internal.h"

namespace {

/* EL1h with the named D/A/I/F bits set. */
constexpr uint64_t Pstate(bool d, bool a, bool i, bool f) {
  const unsigned nibble = (d ? 8U : 0U) | (a ? 4U : 0U) | (i ? 2U : 0U) | (f ? 1U : 0U);
  return OEMU_PSTATE_M_EL1H | (static_cast<uint64_t>(nibble) << OEMU_PSTATE_DAIF_SHIFT);
}

TEST(PendingKind, NothingPendingIsNothing) {
  EXPECT_EQ(oemu_vcpu_internal_pending_kind(Pstate(false, false, false, false), false, false),
            -1);
  EXPECT_EQ(oemu_vcpu_internal_pending_kind(Pstate(true, true, true, true), true, true), -1);
}

TEST(PendingKind, UnmaskedIrqIsIrq) {
  // I clear: delivered. The other mask bits are irrelevant to an IRQ.
  EXPECT_EQ(oemu_vcpu_internal_pending_kind(Pstate(true, true, false, true), true, false),
            static_cast<int>(OEMU_EXC_KIND_IRQ));
}

TEST(PendingKind, MaskedIrqIsNotPendingAtAll) {
  EXPECT_EQ(oemu_vcpu_internal_pending_kind(Pstate(false, false, true, false), true, false),
            -1);
}

TEST(PendingKind, UnmaskedFiqIsFiq) {
  EXPECT_EQ(oemu_vcpu_internal_pending_kind(Pstate(true, true, true, false), false, true),
            static_cast<int>(OEMU_EXC_KIND_FIQ));
}

TEST(PendingKind, MaskedFiqDoesNotHideAnUnmaskedIrq) {
  // Both pins high, F masked, I clear: the IRQ is the deliverable one.
  EXPECT_EQ(oemu_vcpu_internal_pending_kind(Pstate(false, false, false, true), true, true),
            static_cast<int>(OEMU_EXC_KIND_IRQ));
}

TEST(PendingKind, FiqBeatsIrqWhenBothUnmasked) {
  EXPECT_EQ(oemu_vcpu_internal_pending_kind(Pstate(true, true, false, false), true, true),
            static_cast<int>(OEMU_EXC_KIND_FIQ));
}

}  // namespace
