/*
 * id_probe.S -- oracle probe: read the AArch64 feature/ID system registers the
 * kernel keys its VA layout and paging geometry off, and dump each as 16 hex
 * digits on the PL011. Captured under `qemu-system-aarch64 -cpu cortex-a53`
 * (docs/linux-minimal-qemu.md), this guest is what pins oemu's ID-register set
 * to the oracle: the kernel refuses to build a correct linear map unless
 * ID_AA64MMFR0_EL1.VA_BITS/PA_BITS match the CPU it believes it runs on, and a
 * mismatch sends it dereferencing a bogus linear-map address (the prb_reserve
 * fault that kept the earlycon banner from ever printing).
 *
 * Freestanding Image, EL1 entry, DTB in x0 (unused here). Print helpers per
 * tests/guest/psci_off.S: putc polls UARTFR.TXFF (bit 5), UART at 0x09000000.
 */
	.text
	.globl _start

_start_of_image:
	b	_start
	.word	0
	.quad	0x1000                       /* text_offset */
	.quad	_image_end - _start
	.quad	0x2                          /* LE, 4K pages */
	.quad	0
	.quad	0
	.quad	0
	.word	0x644d5241                   /* "ARM\x64" */
	.word	0
	.balign	0x1000

_start:
	movz	x9, #0xF000
	movk	x9, #0x4007, lsl #16
	mov	sp, x9

	adr	x0, str_hdr
	bl	puts

	/* Each probe: mrs the id reg into x21, putx64 dumps it. */
	mrs	x21, midr_el1
	adr	x0, l_midr
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, revidr_el1
	adr	x0, l_revidr
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, mpidr_el1
	adr	x0, l_mpidr
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64pfr0_el1
	adr	x0, l_pfr0
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64pfr1_el1
	adr	x0, l_pfr1
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64isar0_el1
	adr	x0, l_isar0
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64isar1_el1
	adr	x0, l_isar1
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64isar2_el1
	adr	x0, l_isar2
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64isar3_el1
	adr	x0, l_isar3
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64mmfr0_el1
	adr	x0, l_mmfr0
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64mmfr1_el1
	adr	x0, l_mmfr1
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64mmfr2_el1
	adr	x0, l_mmfr2
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64dfr0_el1
	adr	x0, l_dfr0
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64dfr1_el1
	adr	x0, l_dfr1
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_aa64afr0_el1
	adr	x0, l_afr0
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, ctr_el0
	adr	x0, l_ctr
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, clidr_el1
	adr	x0, l_clidr
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, aidr_el1
	adr	x0, l_aidr
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, S3_0_C0_C0_5	/* GMID_EL1 (raw encoding: clang lacks the mnemonic) */
	adr	x0, l_gmid
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, cntfrq_el0
	adr	x0, l_cntfrq
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	/* The AArch32 ID group, dumped for the record. Linux's
	 * __cpuinfo_store_cpu reads all of these ONLY when id_aa64pfr0_32bit_el0
	 * is true -- an MRS of them is a synchronous Undefined on an AArch64-only
	 * core. oemu has no AArch32, so it advertises ID_AA64PFR0_EL1.EL0=0 and the
	 * kernel never reaches this path; oemu therefore implements NONE of these
	 * (they would trap). This dump documents the one place oemu's ID set
	 * deliberately differs from the cortex-a53 oracle, which runs AArch32 at
	 * EL0 and returns nonzero here. Run under qemu -cpu cortex-a53 only. */
	mrs	x21, id_pfr0_el1
	adr	x0, l_pfr0_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_pfr1_el1
	adr	x0, l_pfr1_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_dfr0_el1
	adr	x0, l_dfr0_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_isar0_el1
	adr	x0, l_isar0_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_isar1_el1
	adr	x0, l_isar1_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_isar2_el1
	adr	x0, l_isar2_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_isar3_el1
	adr	x0, l_isar3_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_isar4_el1
	adr	x0, l_isar4_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_isar5_el1
	adr	x0, l_isar5_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_mmfr0_el1
	adr	x0, l_mmfr0_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_mmfr1_el1
	adr	x0, l_mmfr1_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_mmfr2_el1
	adr	x0, l_mmfr2_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_mmfr3_el1
	adr	x0, l_mmfr3_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_mmfr4_el1
	adr	x0, l_mmfr4_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, id_mmfr5_el1
	adr	x0, l_mmfr5_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, MVFR0_EL1
	adr	x0, l_mvfr0_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	mrs	x21, MVFR1_EL1
	adr	x0, l_mvfr1_32
	bl	puts
	mov	x0, x21
	bl	putx64
	bl	putnl

	adr	x0, str_done
	bl	puts

park:
	wfi
	b	park

/* putx64: print x0 as 16 uppercase hex digits (non-leaf; x19 holds the value). */
putx64:
	stp	x29, x30, [sp, #-16]!
	mov	x19, x0
	mov	x9, #64
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

putc:
	movz	x1, #0x0000
	movk	x1, #0x0900, lsl #16
1:	ldr	w2, [x1, #0x18]
	tbnz	w2, #5, 1b
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

putnl:
	mov	w0, #'\n'
	b	putc

	.section .rodata
str_hdr:	.asciz	"IDPROBE\n"
str_done:	.asciz	"IDPROBE-DONE\n"
l_midr:		.asciz	"MIDR="
l_revidr:	.asciz	"REVIDR="
l_mpidr:	.asciz	"MPIDR="
l_pfr0:		.asciz	"PFR0="
l_pfr1:		.asciz	"PFR1="
l_isar0:	.asciz	"ISAR0="
l_isar1:	.asciz	"ISAR1="
l_isar2:	.asciz	"ISAR2="
l_isar3:	.asciz	"ISAR3="
l_isar4:	.asciz	"ISAR4="
l_isar5:	.asciz	"ISAR5="
l_mmfr0:	.asciz	"MMFR0="
l_mmfr1:	.asciz	"MMFR1="
l_mmfr2:	.asciz	"MMFR2="
l_dfr0:		.asciz	"DFR0="
l_dfr1:		.asciz	"DFR1="
l_afr0:		.asciz	"AFR0="
l_zfr0:		.asciz	"ZFR0="
l_ctr:		.asciz	"CTR="
l_clidr:	.asciz	"CLIDR="
l_ccsidr:	.asciz	"CCSIDR="
l_aidr:		.asciz	"AIDR="
l_gmid:		.asciz	"GMID="
l_cntfrq:	.asciz	"CNTFRQ="
l_pfr0_32:	.asciz	"ID_PFR0="
l_pfr1_32:	.asciz	"ID_PFR1="
l_dfr0_32:	.asciz	"ID_DFR0="
l_isar0_32:	.asciz	"ID_ISAR0="
l_isar1_32:	.asciz	"ID_ISAR1="
l_isar2_32:	.asciz	"ID_ISAR2="
l_isar3_32:	.asciz	"ID_ISAR3="
l_isar4_32:	.asciz	"ID_ISAR4="
l_isar5_32:	.asciz	"ID_ISAR5="
l_mmfr0_32:	.asciz	"ID_MMFR0="
l_mmfr1_32:	.asciz	"ID_MMFR1="
l_mmfr2_32:	.asciz	"ID_MMFR2="
l_mmfr3_32:	.asciz	"ID_MMFR3="
l_mmfr4_32:	.asciz	"ID_MMFR4="
l_mmfr5_32:	.asciz	"ID_MMFR5="
l_mvfr0_32:	.asciz	"ID_MVFR0="
l_mvfr1_32:	.asciz	"ID_MVFR1="

_image_end:
