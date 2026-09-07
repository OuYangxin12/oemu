/*
 * el1_smoke.S -- M2c wrap-up acceptance guest: the boot protocol round-trip.
 *
 * Boots at EL1 (exactly as `oemu boot` and `qemu -kernel` deliver one), arms
 * the EL1 vector table, takes and returns from an SVC, prints the "BOOT-OK"
 * marker on the PL011, and exits over the PSCI conduit: an SMC SYSTEM_OFF
 * powers the machine down for a clean exit 0 under both oemu and the QEMU
 * oracle (each serves the call itself). A BRK/park below is only a dead-man's
 * switch for a platform that somehow does not answer the SMC.
 *
 * The point is the round trip, not the program: reaching "BOOT-OK" proves
 * EL1 entry, VBAR plumbing, SVC delivery, ELR/ERET return and the UART write
 * path simultaneously -- the four things `boot` must get right before any
 * kernel can be asked to.
 *
 * Constants cite the same sources as the other guests (this file shares no
 * definitions with src/): the Image header per Documentation/arch/arm64/
 * booting.rst, vector-table geometry and the ESR EC field per ARM ARM
 * DDI 0487 (D17.2 exception syndrome register; D1.4.5 vector table: 16 slots
 * of 0x80, first slot per group). EC values used: 0x15 SVC-AArch64, 0x30
 * BRK/breakpoint (both D17.2.47/60).
 *
 * Layout mirrors the machine contract (and the QEMU virt oracle baseline):
 * RAM at 0x40000000, image at 0x40080000, stack at 0x4007F000, PL011 UART0 at
 * 0x09000000.
 */
	.text
	.globl _start

/* --- AArch64 Image header (Documentation/arch/arm64/booting.rst) --------- */
_start_of_image:
	b	_start                       /* +0x00 code0: branch to the entry */
	.word	0                            /* +0x04 code1 (unused) */
	.quad	0x1000                       /* +0x08 text_offset: 4K-aligned, inside the file */
	.quad	_image_end - _start_of_image /* +0x10 image_size (LE bytes, whole file) */
	.quad	0x2                          /* +0x18 flags: LE, 4K pages (bits1-2=01) */
	.quad	0                            /* +0x20 reserved */
	.quad	0                            /* +0x28 reserved */
	.quad	0                            /* +0x30 reserved */
	.word	0x644d5241                   /* +0x38 magic "ARM\x64" */
	.word	0                            /* +0x3c pad to the 64-byte header */
	/* The header advertises text_offset 0x1000, so the file must actually
	 * carry its text at that byte offset: materialise the pad or a strict
	 * loader (booting.rst) sees text_offset past the end of the file. */
	.balign	0x1000
_start:
	/* Stack below the image (0x4007F000 is untouched RAM; the print helpers
	 * are non-leaf and must spill x30). Same choice as mmu_smoke. */
	movz	x9, #0xF000
	movk	x9, #0x4007, lsl #16
	mov	sp, x9

	/* Arm the catcher before anything can fault, so a failure names itself
	 * instead of vanishing (the mmu_smoke lesson, applied from instruction
	 * zero): every vector slot branches to the dispatch below. */
	adr	x0, el1_vectors
	msr	vbar_el1, x0
	isb

	/* Self-check the entry level: print "EL<n>" and require n == 1. A wrong
	 * level at boot must fail as itself, not as a mysterious later fault. */
	mrs	x9, CurrentEL
	lsr	x9, x9, #2
	adr	x0, str_el
	bl	puts
	mov	w0, #'0'
	add	w0, w0, w9
	bl	putc
	mov	w0, #'\n'
	bl	putc
	cmp	w9, #1
	b.ne	fail

	/* --- the SVC round trip -------------------------------------------- */
	/* x20 is callee-saved and untouched by the print helpers; the handler
	 * sets it. Zero, SVC, and check -- a silent no-op handler says so. */
	mov	x20, #0
	svc	#0
	cmp	x20, #1
	b.ne	fail

	/* --- the marker ---------------------------------------------------- */
	adr	x0, str_ok
	bl	puts

	/* Exit protocol (M4a): the PSCI conduit, not a UART sentinel. Negotiate
	 * the version then issue SYSTEM_OFF (0x84000002) over SMC -- the same
	 * contract `oemu boot` answers and the kernel obeys. Under the QEMU
	 * oracle the SMC is served by its own PSCI emulation and the machine
	 * powers down there too; the BRK/park below is only a dead-man's switch. */
	movz	x0, #0x0
	movk	x0, #0x8400, lsl #16     /* 0x84000000 PSCI_VERSION */
	smc	#0
	movz	x0, #0x2
	movk	x0, #0x8400, lsl #16     /* 0x84000002 SYSTEM_OFF */
	smc	#0

	brk	#0
	/* Fall through to the park anyway: if the BRK vector runs, it parks too. */
	park:
	b	park

fail:
	adr	x0, str_fail
	bl	puts
	b	park

/* --- EL1 exception vectors ------------------------------------------------
 * 16 slots of 0x80 (ARM ARM DDI 0487 D1.4.5): all four SP/EL groups funnel to
 * one dispatcher, because this guest is entered at EL1 and never knows which
 * group its own SVC/BRK will arrive through. */
.balign 0x800
el1_vectors:
	.rept 16
	b	el1_dispatch
	.space 124
	.endr

el1_dispatch:
	/* x21 keeps ESR across the calls below; x20 is the SVC flag and must
	 * survive too (callee-saved, and nothing here touches it). */
	mrs	x21, esr_el1
	lsr	x22, x21, #26
	and	x22, x22, #0x3F              /* ESR.EC, ARM ARM D17.2 */
	cmp	x22, #0x15                   /* SVC from AArch64 mode */
	b.eq	svc_advance
	cmp	x22, #0x30                   /* BRK #imm */
	b.eq	park
	/* Anything else is a guest-state bug: name it (stack, ESR) and park --
	 * the same "failures must be loud" rule as mmu_smoke's catcher. */
	stp	x29, x30, [sp, #-16]!
	mov	w0, #'F'
	bl	putc
	mov	w0, #' '
	bl	putc
	mov	x0, x21
	bl	putx32
	mov	w0, #'\n'
	bl	putc
	ldp	x29, x30, [sp], #16
	b	park

svc_advance:
	/* Return from the SVC: skip the 4-byte instruction (the only encoding
	 * width here) and hand the flag to the main flow. */
	mrs	x23, elr_el1
	add	x23, x23, #4
	msr	elr_el1, x23
	mov	x20, #1
	eret

/* --- output helpers (identical contract to mmu_smoke.S) --------------------
 * The busy-wait on FR.TXFF(5) is the pattern that runs unchanged against
 * QEMU's PL011; oemu's stopgap UART answers FR=0, so it never spins there. */
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

/* putx32: print w0 as 8 uppercase hex digits (non-leaf: x30 spilled). */
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

.section .rodata
str_el:		.asciz "EL"
str_ok:		.asciz "BOOT-OK\n"
str_fail:	.asciz "BOOT-FAIL\n"

_image_end:
