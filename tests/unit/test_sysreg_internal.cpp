// White-box tests for the system-register table.
//
// These reach into src/sysreg/sysreg_internal.h to validate the table
// wholesale: every row structurally well-formed, encodings pinned to the
// assembler-harvested values, and init actually applying every reset value --
// contracts a read/write-only API could only cover one encoding at a time.
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <cstddef>
#include <cstdint>
#include <set>

#include <gtest/gtest.h>

#include "sysreg/sysreg_internal.h"

namespace {

class SysregInternalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, 0x40080000, 0x41000000));
    oemu_sysregs_init(&sr_, &regs_, OEMU_EL1);
  }

  oemu_regs regs_{};
  oemu_sysregs sr_{};
};

// --- table structure --------------------------------------------------------------

TEST_F(SysregInternalTest, TableIsAscendingAndUniqueBySel) {
  const oemu_sysreg_table table = oemu_sysreg_internal_table();
  ASSERT_NE(nullptr, table.rows);
  ASSERT_GT(table.count, 0u);

  std::set<uint32_t> seen;
  for (size_t i = 0; i < table.count; i++) {
    EXPECT_TRUE(seen.insert(table.rows[i].sel).second)
        << "duplicate encoding 0x" << std::hex << table.rows[i].sel;
    if (i > 0) {
      EXPECT_LT(table.rows[i - 1].sel, table.rows[i].sel) << "table must stay ascending by sel";
    }
  }
}

TEST_F(SysregInternalTest, EveryRowIsWellFormed) {
  const oemu_sysreg_table table = oemu_sysreg_internal_table();
  const size_t struct_size = sizeof(oemu_sysregs);
  for (size_t i = 0; i < table.count; i++) {
    const oemu_sysreg_row &row = table.rows[i];
    ASSERT_NE(nullptr, row.name) << "row " << i << " has no name";
    EXPECT_LE(row.min_el, OEMU_EL3) << row.name;
    EXPECT_EQ(0u, row.flags & ~(OEMU_SYSREG_F_RO | OEMU_SYSREG_F_WI)) << row.name;

    const bool is_ro = (row.flags & OEMU_SYSREG_F_RO) != 0;
    const bool is_wi = (row.flags & OEMU_SYSREG_F_WI) != 0;

    if (is_wi) {
      // A write-ignored stub has nothing to read or write: no storage, no
      // callbacks. Storage at offset 0 would alias the regs pointer.
      EXPECT_EQ(0, row.offset) << row.name;
      EXPECT_EQ(nullptr, row.get) << row.name;
      EXPECT_EQ(nullptr, row.set) << row.name;
      continue;
    }
    if (row.get != nullptr) {
      // Callback row: value lives outside this struct, so no storage.
      EXPECT_EQ(0, row.offset) << row.name;
      if (!is_ro) {
        EXPECT_NE(nullptr, row.set) << "writable callback row needs a setter: " << row.name;
      } else {
        EXPECT_EQ(nullptr, row.set) << "read-only callback row needs no setter: " << row.name;
      }
    } else {
      // Plain storage row (RO or RW): the offset path serves both directions,
      // so no callbacks at all. The offset must land inside the struct on an
      // 8-byte boundary and leave room for a full uint64_t.
      EXPECT_EQ(nullptr, row.set) << "plain row must use the offset path: " << row.name;
      EXPECT_NE(0, row.offset) << "offset 0 aliases the regs pointer: " << row.name;
      EXPECT_EQ(0u, row.offset % alignof(uint64_t)) << row.name;
      EXPECT_LE(row.offset + sizeof(uint64_t), struct_size) << row.name;
    }
  }
}

TEST_F(SysregInternalTest, EncodingsMatchTheAssemblerHarvest) {
  // Pinned raw values from build/sysgen/encodings.txt (clang's integrated
  // assembler). If one of the OEMU_SYSREG_* constants drifts, this fails
  // before a guest ever sees a wrong encoding.
  const struct {
    uint32_t sel;
    uint32_t harvested;
  } rows[] = {
      {OEMU_SYSREG_NZCV, 0x1A10},       {OEMU_SYSREG_DAIF, 0x1A11},
      {OEMU_SYSREG_CURRENT_EL, 0x0212}, {OEMU_SYSREG_SPSEL, 0x0210},
      {OEMU_SYSREG_SP_EL0, 0x0208},     {OEMU_SYSREG_SP_EL1, 0x2208},
      {OEMU_SYSREG_SCTLR_EL1, 0x0080},  {OEMU_SYSREG_VBAR_EL1, 0x0600},
      {OEMU_SYSREG_TPIDR_EL0, 0x1E82},  {OEMU_SYSREG_TPIDRRO_EL0, 0x1E83},
      {OEMU_SYSREG_TTBR0_EL1, 0x0100},  {OEMU_SYSREG_ESR_EL3, 0x3290},
  };
  for (const auto &row : rows) {
    const oemu_sysreg_row *found = oemu_sysreg_internal_find(row.sel);
    ASSERT_NE(nullptr, found) << "missing row for sel 0x" << std::hex << row.sel;
    EXPECT_EQ(row.harvested, found->sel) << found->name;
  }
}

// --- init and reset -----------------------------------------------------------------

TEST_F(SysregInternalTest, InitAppliesEveryResetValue) {
  const oemu_sysreg_table table = oemu_sysreg_internal_table();
  for (size_t i = 0; i < table.count; i++) {
    const oemu_sysreg_row &row = table.rows[i];
    if (row.offset == 0) {
      continue;  // callback and WI rows keep no storage in oemu_sysregs
    }
    const uint64_t raw =
        *reinterpret_cast<const uint64_t *>(reinterpret_cast<const char *>(&sr_) + row.offset);
    EXPECT_EQ(row.reset_value, raw) << row.name << " was not reset";
  }
}

TEST_F(SysregInternalTest, CallbackRowsSeeTheBootState) {
  // SPSel=1 and DAIF=1111 at an EL1 boot; CurrentEL reads level 1; NZCV and
  // SP_EL0 come straight from the paired register file.
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SPSEL, &value));
  EXPECT_EQ(1u, value);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_DAIF, &value));
  EXPECT_EQ(0xFu, value);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_CURRENT_EL, &value));
  EXPECT_EQ(4u, value);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_NZCV, &value));
  EXPECT_EQ(0u, value);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SP_EL0, &value));
  EXPECT_EQ(oemu_regs_sp(&regs_), value);
}

TEST_F(SysregInternalTest, InitDoesNotTouchTheRegisterFile) {
  oemu_regs regs{};
  ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs, 0xDEAD0000, 0xBEEF0000));
  oemu_sysregs sr{};
  oemu_sysregs_init(&sr, &regs, OEMU_EL1);
  EXPECT_EQ(0xDEAD0000u, oemu_regs_pc(&regs)) << "PC belongs to oemu_cpu_init";
  EXPECT_EQ(0xBEEF0000u, oemu_regs_sp(&regs)) << "SP_EL0 belongs to oemu_cpu_init";
}

// --- PSTATE helpers -----------------------------------------------------------------

TEST_F(SysregInternalTest, ModeConstantsMapToTheirLevel) {
  EXPECT_EQ(OEMU_EL0, oemu_pstate_el(OEMU_PSTATE_M_EL0T));
  EXPECT_EQ(OEMU_EL1, oemu_pstate_el(OEMU_PSTATE_M_EL1H));
  EXPECT_EQ(OEMU_EL2, oemu_pstate_el(OEMU_PSTATE_M_EL2H));
  EXPECT_EQ(OEMU_EL3, oemu_pstate_el(OEMU_PSTATE_M_EL3H));
}

TEST_F(SysregInternalTest, ModeRoundTripsThroughPstateEl) {
  for (unsigned el = 0; el < OEMU_EL_COUNT; el++) {
    const uint64_t pstate = oemu_pstate_mode(static_cast<oemu_el>(el)) | OEMU_PSTATE_SPSEL;
    EXPECT_EQ(el, static_cast<unsigned>(oemu_pstate_el(pstate))) << "EL" << el;
  }
}

TEST_F(SysregInternalTest, PstateFieldHelpers) {
  const uint64_t pstate = OEMU_PSTATE_M_EL1H | OEMU_PSTATE_SPSEL |
                          (0b1101u << OEMU_PSTATE_DAIF_SHIFT) | OEMU_PSTATE_IL;
  EXPECT_EQ(OEMU_EL1, oemu_pstate_el(pstate));
  EXPECT_EQ(1u, oemu_pstate_sp_sel(pstate));
  EXPECT_EQ(0b1101u, oemu_pstate_daif(pstate));
}

TEST_F(SysregInternalTest, FindMissesReturnNull) {
  EXPECT_EQ(nullptr, oemu_sysreg_internal_find(0x3FFF));
  EXPECT_EQ(nullptr, oemu_sysreg_internal_find(0x1E87));
}

}  // namespace
