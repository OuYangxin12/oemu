/*
 * mmu_smoke.S -- M3a acceptance guest: stage-1 MMU on, then prove translation.
 *
 * Boots at EL1 (oemu boot and qemu -kernel agree on this), builds a 39-bit-VA
 * 4 KiB-granule page-table set in RAM, enables the MMU, and exercises an
 * access that crosses a page boundary through the translated mapping. Prints
 * "EL<n>" as a diagnostic, then "MMU-OK" on success or "MMU-FAIL" on mismatch,
 * and parks in WFI (oemu yields; the QEMU oracle runner timeout-kills after
 * the marker is on the wire).
 *
 * All constants are cited to the local Linux 6.6 tree
 * (guest/src/linux-6.6.156/arch/arm64/include/asm/pgtable-hwdef.h) so this
 * guest stays an independent oracle -- it shares no definitions with src/.
 *
 * Layout decisions that mirror the machine contract: RAM at 0x40000000
 * (QEMU virt), image at 0x40080000, page tables at 0x40100000..0x40102FFF,
 * scratch pages at 0x40130000/0x40131000, PL011 UART0 at 0x09000000.
 */
	.text
	.globl _start

/* --- AArch64 Image header (Documentation/arch/arm64/booting.rst) --------- */
_start_of_image:
	b	_start                       /* +0x00 code0: branch to the entry */
	.word	0                            /* +0x04 code1 (unused) */
	.quad	0x80000                      /* +0x08 text_offset: 4K/39-48-bit */
	.quad	_image_end - _start_of_image /* +0x10 image_size (LE bytes) */
	.quad	0x2                          /* +0x18 flags: LE, 4K pages (bits1-2=01) */
	.quad	0                            /* +0x20 reserved */
	.quad	0                            /* +0x28 reserved */
	.quad	0                            /* +0x30 reserved */
	.word	0x644d5241                   /* +0x38 magic "ARM\x64" */
	.word	0                            /* +0x3c pad to the 64-byte header */

_start:
	/* Reserve a stack below the image: the print helpers are non-leaf and
	 * must spill x30 across their own calls. 0x4007F000 is 16-byte aligned
	 * RAM the image and the page tables never touch. */
	movz	x9, #0xF000
	movk	x9, #0x4007, lsl #16
	mov	sp, x9

	/* Arm an EL1 vector catcher first: any exception from here on prints
	 * "F" and parks instead of vanishing into an unassigned-memory loop.
	 * Without this, a faulting guest is indistinguishable from a slow one. */
	adr	x0, el1_vectors
	msr	vbar_el1, x0

	/* Who am I: print "EL<n>" so a wrong entry level names itself.
	 * x9 holds the level across the puts() call, which clobbers x0-x3. */
	mrs	x9, CurrentEL
	lsr	x9, x9, #2
	adr	x0, str_el
	bl	puts
	mov	w0, #'0'
	add	w0, w0, w9
	bl	putc
	mov	w0, #'\n'
	bl	putc

	bl	build_tables
	bl	pre_write_scratch
	bl	dump_tables
	bl	walk_sim
	adr	x0, str_stage_s
	bl	puts

	/* --- enable stage-1 translation ---------------------------------- */
	/* MAIR_EL1: attr0 = 0xFF normal WB RA/WA, attr1 = 0x04 Device-nGnRE. */
	movz	x0, #0x4ff
	msr	mair_el1, x0
	/*
	 * TCR_EL1, 39-bit VA / 4 KiB (Linux pgtable-hwdef.h):
	 *   T0SZ  = 64-39 = 25   bits[5:0]
	 *   IRGN0 = WB-WA(1)     bits[9:8]
	 *   ORGN0 = WB-WA(1)     bits[11:10]
	 *   SH0   = inner(3)     bits[13:12]
	 *   TG0   = 4K(0)        bits[15:14]
	 *   T1SZ  = 25           bits[21:16]
	 *   TG1   = 4K(0b10)     bits[31:30]  (2<<30, a different slot from TG0)
	 * Assembled: 0x80000000 | 0x01900000 | 0x3500 | 0x19 = 0x81903519.
	 */
	movz	x0, #0x3519           /* T0SZ | IRGN0 | ORGN0 | SH0 */
	movk	x0, #0x8190, lsl #16  /* T1SZ | TG1 */
	msr	tcr_el1, x0
	adr	x0, str_stage_r
	bl	puts
	/* TTBR0/1 both -> the single L1 root at 0x40100000 (low half is
	 * 0x0000: 0x40100000 & 0xFFFF). */
	movz	x0, #0x0000
	movk	x0, #0x4010, lsl #16
	msr	ttbr0_el1, x0
	msr	ttbr1_el1, x0
	/* Sysreg readback while still untranslated: catch a bad write now
	 * instead of blaming the walk later. Order: MAIR, TCR, TTBR0. */
	adr	x0, str_sysreg
	bl	puts
	mrs	x0, mair_el1
	bl	putx32
	mrs	x0, tcr_el1
	bl	putx32
	mrs	x0, ttbr0_el1
	bl	putx32
	mov	w0, #'\n'
	bl	putc
	dsb	sy
	isb
	mrs	x0, sctlr_el1
	orr	x0, x0, #1            /* SCTLR.M: translation enabled */
	msr	sctlr_el1, x0
	/* TLB reset state is architecturally UNKNOWN: purge before trusting
	 * any translation (Linux head.S purges the same way around M). */
	tlbi	vmalle1
	dsb	sy
	isb
	adr	x0, str_stage_m
	bl	puts

	/* --- the translation probes --------------------------------------- */
	/* Bisect the data path: (a) aligned 8B in one page, (b) unaligned 8B
	 * still inside page 0, (c) unaligned 8B crossing into page 1. */
	movz	x20, #0x0000
	movk	x20, #0x8000, lsl #16  /* 0x80000000 */
	ldr	x5, [x20]
	adr	x0, str_stage_a
	bl	puts
	movz	x20, #0x0FF4
	movk	x20, #0x8000, lsl #16  /* 0x80000FF4, 4-mod-8, one page */
	ldr	x5, [x20]
	adr	x0, str_stage_b
	bl	puts
	movz	x20, #0xFFC
	movk	x20, #0x8000, lsl #16  /* 0x80000FFC: spans into page 1 */
	ldr	x5, [x20]
	adr	x0, str_stage_c
	bl	puts
	movz	x6, #0xA1A1
	movk	x6, #0xA1A1, lsl #16
	movz	x7, #0xB2B2
	movk	x7, #0xB2B2, lsl #16
	lsl	x8, x7, #32
	orr	x6, x6, x8            /* expected: 0xB2B2B2B2A1A1A1A1 */
	cmp	x5, x6
	b.ne	mmu_fail

	/* Write through the translated page, verify via the identity block. */
	movz	x20, #0x1004          /* VA 0x80001004 -> PA 0x40131004 */
	movk	x20, #0x8000, lsl #16
	movz	x21, #0xC3C3
	movk	x21, #0xC3C3, lsl #16
	str	w21, [x20]
	movz	x22, #0x1004          /* VA 0x40131004: identity RAM block */
	movk	x22, #0x4013, lsl #16
	ldr	w23, [x22]
	cmp	w23, w21
	b.ne	mmu_fail

	adr	x0, str_ok
	bl	puts
	wfi_park:
	wfi
	b	wfi_park

mmu_fail:
	adr	x0, str_fail
	bl	puts
	wfi_fail:
	wfi
	b	wfi_fail

/* --- EL1 exception vectors ------------------------------------------------ */
/* 16 x 128-byte slots, all funnelling into one handler: a smoke guest has no
 * use for return-from-exception, only for naming the failure. */
.balign 0x800
el1_vectors:
	.rept 16
	b	fault_park
	.space 124
	.endr
fault_park:
	/* Name the failure: ESR_EL1 (fault class) and FAR_EL1 (faulting VA).
	 * putx32 needs a live stack, which _start established. */
	stp	x29, x30, [sp, #-16]!
	adr	x0, str_fault
	bl	puts
	mrs	x21, esr_el1
	lsr	x0, x21, #32
	bl	putx32
	mov	x0, x21
	bl	putx32
	adr	x0, str_sp
	bl	puts
	mrs	x21, far_el1
	lsr	x0, x21, #32
	bl	putx32
	mov	x0, x21
	bl	putx32
	mov	w0, #'\n'
	bl	putc
	ldp	x29, x30, [sp], #16
	v_park:
	wfi
	b	v_park

/* putx32: print w0 as 8 uppercase hex digits. Non-leaf (calls putc), so x30
 * is spilled; the value parks in x19 and the shift counter in x9. */
putx32:
	stp	x29, x30, [sp, #-16]!
	mov	x19, x0
	mov	x9, #32
1:	sub	x9, x9, #4
	lsr	x3, x19, x9
	and	w3, w3, #0xf
	cmp	w3, #10
	b.lo	2f
	add	w3, w3, #('A' - 10)
	b	3f
2:	add	w3, w3, #'0'
3:	mov	w0, w3
	bl	putc
	cbnz	x9, 1b
	ldp	x29, x30, [sp], #16
	ret

/* dump_tables: print the six descriptor words the walk will actually read:
 * root[0..2], L2[0], L3[0], L3[1]. The cursor rides x20, which putx32 does
 * not touch. */
dump_tables:
	stp	x29, x30, [sp, #-16]!
	mov	w0, #'D'
	bl	putc
	mov	w0, #':'
	bl	putc
	movz	x20, #0x0000
	movk	x20, #0x4010, lsl #16 /* root */
	ldr	w21, [x20], #8
	mov	x0, x21
	bl	putx32
	ldr	w21, [x20], #8
	mov	x0, x21
	bl	putx32
	ldr	w21, [x20], #8
	mov	x0, x21
	bl	putx32
	movz	x20, #0x1000
	movk	x20, #0x4010, lsl #16 /* L2 @0x40101000 */
	ldr	w21, [x20]
	mov	x0, x21
	bl	putx32
	movz	x20, #0x2000
	movk	x20, #0x4010, lsl #16 /* L3 @0x40102000 */
	ldr	w21, [x20], #8
	mov	x0, x21
	bl	putx32
	ldr	w21, [x20]
	mov	x0, x21
	bl	putx32
	mov	w0, #'\n'
	bl	putc
	ldp	x29, x30, [sp], #16
	ret

/* walk_sim: replay the hardware walk for VA 0x80000000 by hand while the
 * MMU is still off: root[2] -> L2[0] -> L3[0], printing each descriptor. */
walk_sim:
	stp	x29, x30, [sp, #-16]!
	mov	w0, #'W'
	bl	putc
	mov	w0, #':'
	bl	putc
	movz	x20, #0x0000
	movk	x20, #0x4010, lsl #16
	ldr	x21, [x20, #16]          /* root[2] */
	mov	x0, x21
	bl	putx32
	and	x22, x21, #0xFFFFF000    /* L2 base */
	ldr	x21, [x22]               /* L2[0] */
	mov	x0, x21
	bl	putx32
	and	x22, x21, #0xFFFFF000    /* L3 base */
	ldr	x21, [x22]               /* L3[0] */
	mov	x0, x21
	bl	putx32
	mov	w0, #'\n'
	bl	putc
	ldp	x29, x30, [sp], #16
	ret

/* --- page tables ---------------------------------------------------------- */
/*
 * Identity mapping for a machine that only needs RAM + UART:
 *   L1[0]  1 GiB block  0x00000000..0x3FFFFFFF  device (UART), AttrIndx=1
 *   L1[1]  1 GiB block  0x40000000..0x7FFFFFFF  normal WB, exec-allowed
 *   L1[2]  table 0x40101000 -> L2[0] table 0x40102000 -> L3[0..1] pages
 * Block/page words: bits[1:0] encode the descriptor type -- 0b01 = BLOCK
 * (levels 1-2 only; at L3 it is reserved => translation fault), 0b11 =
 * PAGE at L3 / TABLE below. Then AF | SH inner (3<<8) | AttrIndx.
 * Descriptors per DDI 0487J, bit positions cross-checked against Linux
 * pgtable-hwdef.h and QEMU 8.2.2 target/arm/ptw.c behaviour.
 */
build_tables:
	/* Zero the three 4 KiB tables at 0x40100000..0x40102FFF (12 KiB). */
	movz	x10, #0x0000
	movk	x10, #0x4010, lsl #16
	mov	x11, #0x3000
1:	str	xzr, [x10], #8
	sub	x11, x11, #8
	cbnz	x11, 1b

	/* Layout (L1 start, T0SZ=25):
	 *   root @0x40100000 (L1): [0] device block, [1] RAM block, [2] table
	 *   L2 @0x40101000: [0] table -> L3
	 *   L3 @0x40102000: [0..1] scratch pages */
	movz	x12, #0x0000
	movk	x12, #0x4010, lsl #16
	movz	x13, #0x405           /* root[0]: block, dev, AF, AttrIndx=1 */
	str	x13, [x12]
	movz	x13, #0x0701          /* root[1]: block, WB, AF, inner */
	movk	x13, #0x4000, lsl #16 /* -> 0x40000701 */
	str	x13, [x12, #8]
	movz	x13, #0x1003          /* root[2]: table -> 0x40101003 */
	movk	x13, #0x4010, lsl #16
	str	x13, [x12, #16]

	movz	x13, #0x2003          /* L2[0]: table -> 0x40102003 */
	movk	x13, #0x4010, lsl #16
	str	x13, [x12, #0x1000]

	/* PAGE descriptors at L3: bits[1:0] = 0b11 (0b01 there is a BLOCK,
	 * reserved at level 3 -> translation fault). */
	movz	x13, #0x0703          /* L3[0]: page -> 0x40130703 */
	movk	x13, #0x4013, lsl #16
	str	x13, [x12, #0x2000]
	movz	x13, #0x1703          /* L3[1]: page -> 0x40131703 */
	movk	x13, #0x4013, lsl #16
	str	x13, [x12, #0x2008]
	ret

/* Pattern bytes land before the MMU is on (VA == PA then); the post-MMU
 * cross-page load re-reads them through the translated mapping. */
pre_write_scratch:
	movz	x10, #0x0FFC          /* PA 0x40130FFC: last word of scratch p0 */
	movk	x10, #0x4013, lsl #16
	movz	x11, #0xA1A1
	movk	x11, #0xA1A1, lsl #16
	str	w11, [x10]
	movz	x10, #0x1000          /* PA 0x40131000: first word of p1 */
	movk	x10, #0x4013, lsl #16
	movz	x11, #0xB2B2
	movk	x11, #0xB2B2, lsl #16
	str	w11, [x10]
	ret

/* --- PL011 (0x09000000) --------------------------------------------------- */
/* Poll FR.TXFF (bit 5) before each byte: correct on QEMU and on oemu's
 * model alike, and costs nothing at smoke scale. */
putc:
	movz	x1, #0x0000
	movk	x1, #0x0900, lsl #16  /* UART base 0x09000000 */
1:	ldr	w2, [x1, #0x18]       /* FR */
	tbnz	w2, #5, 1b            /* TXFF busy */
	strb	w0, [x1]
	ret

puts:
	movz	x1, #0x0000
	movk	x1, #0x0900, lsl #16
2:	ldrb	w2, [x0], #1
	cbz	w2, 3f
1:	ldr	w3, [x1, #0x18]
	tbnz	w3, #5, 1b
	strb	w2, [x1]
	b	2b
3:	ret

.section .rodata
str_el:		.asciz "EL"
str_stage_s:	.asciz "S"            /* tables + scratch built */
str_stage_r:	.asciz "R"            /* MAIR/TCR/TTBR programmed */
str_stage_m:	.asciz "M"            /* MMU on, fetch translated */
str_stage_a:	.asciz "a"            /* aligned 8B, one page */
str_stage_b:	.asciz "b"            /* unaligned 8B, one page */
str_stage_c:	.asciz "c"            /* unaligned 8B, crossing pages */
str_ok:		.asciz "\nMMU-OK\n"
str_fail:	.asciz "\nMMU-FAIL\n"
str_fault:	.asciz "\nMMU-FAULT "
str_sp:		.asciz ":"
str_sysreg:	.asciz "R:"

_image_end:
