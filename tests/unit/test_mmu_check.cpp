/*
 * Death tests for the translation layer's fatal contracts: the module
 * refuses a half-built machine (NULL state, a bus view missing callbacks)
 * and a bad fault-level argument, the same way every other module does --
 * these are programmer errors, not guest events.
 *
 * INTERNAL because one case reaches the DFSC table's level guard; the rest
 * pin the public entry points.
 */
#include "oemu/memops.h"
#include "oemu/mmu.h"
#include "oemu/status.h"
#include "oemu/sysreg.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "mmu/mmu_internal.h"

namespace {

/* A bus that succeeds at everything: enough shape for the checks that are
 * not about the bus to run past it. */
oemu_status ok_read(void *, uint64_t, oemu_mem_size, bool, uint64_t *) {
  return OEMU_OK;
}
oemu_status ok_write(void *, uint64_t, oemu_mem_size, uint64_t) {
  return OEMU_OK;
}
oemu_status ok_fetch(void *, uint64_t, uint32_t *) {
  return OEMU_OK;
}
oemu_status ok_validate(void *, uint64_t, uint64_t, uint32_t) {
  return OEMU_OK;
}

oemu_memops ok_bus() {
  oemu_memops ops{};
  ops.ctx = nullptr;
  ops.fetch32 = ok_fetch;
  ops.read = ok_read;
  ops.write = ok_write;
  ops.validate = ok_validate;
  return ops;
}

TEST(MmuCheck, InitRejectsMissingState) {
  oemu_sysregs sr{};
  oemu_mmu mmu{};
  const oemu_memops bus = ok_bus();
  EXPECT_DEATH(oemu_mmu_init(nullptr, &sr, &bus), "NULL oemu_mmu argument");
  EXPECT_DEATH(oemu_mmu_init(&mmu, nullptr, &bus), "NULL oemu_mmu argument");
  EXPECT_DEATH(oemu_mmu_init(&mmu, &sr, nullptr), "NULL oemu_mmu argument");
  oemu_memops partial = ok_bus();
  partial.validate = nullptr;
  EXPECT_DEATH(oemu_mmu_init(&mmu, &sr, &partial), "half-built bus view");
}

TEST(MmuCheck, TakeFaultRejectsNullMmu) {
  oemu_sysregs sr{};
  oemu_mmu mmu{};
  const oemu_memops bus = ok_bus();
  oemu_mmu_init(&mmu, &sr, &bus);
  oemu_mmu_fault f{};
  EXPECT_DEATH((void)oemu_mmu_take_fault(nullptr, &f), "NULL oemu_mmu");
  /* The record itself is optional: consuming without reading is a legal
   * "I already know what it said". */
  EXPECT_FALSE(oemu_mmu_take_fault(&mmu, nullptr));
}

TEST(MmuCheck, DfscRejectsLevelsThatCannotExist) {
  EXPECT_DEATH(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_TRANSLATION, -1), "fault level");
  EXPECT_DEATH(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_TRANSLATION, 4), "fault level");
  EXPECT_DEATH(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_PERMISSION, 0), "fault level");
  EXPECT_DEATH(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ACCESS_FLAG, 0), "fault level");
}

/* Entry points that answer with a status instead of aborting. */
TEST(MmuCheck, TranslateWithoutRequiredOutputsIsRejectedNotFatal) {
  oemu_sysregs sr{};
  oemu_mmu mmu{};
  const oemu_memops bus = ok_bus();
  oemu_mmu_init(&mmu, &sr, &bus);
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  EXPECT_EQ(oemu_mmu_translate(nullptr, 0U, false, false, &pa, &f), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_mmu_translate(&mmu, 0U, false, false, nullptr, &f), OEMU_ERR_INVALID_ARG);
  /* The fault record is optional: a caller that only wants the verdict may
   * leave it out, and the no-fault answer must be honest about that. */
  EXPECT_EQ(oemu_mmu_translate(&mmu, 0U, false, false, &pa, nullptr), OEMU_OK);
}

}  // namespace
