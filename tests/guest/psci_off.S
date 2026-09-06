/*
 * psci_off.S -- M4 acceptance guest: print on the PL011, then power off
 * through PSCI SYSTEM_OFF.
 *
 * Doubles as a fixed asset for two milestones (task cards m4a/m4b): it is a
 * fully valid AArch64 Image for the M4a loader tests, and it is the M4b
 * acceptance program -- oemu must route PSCI and exit with code 0 exactly
 * like qemu-system-aarch64 does today (verified by scripts/qemu-oracle.sh).
 * Prints "PSCI-OK" before the call and parks with a diagnostic print if the
 * call ever returns (it must not on a passing machine).
 *
 * The conduit is probed, not assumed -- the same rule the baseline kernel
 * followed ("PSCI v1.1, SMC conduit (probed from DT)", docs/linux-minimal-qemu.md):
 * the boot protocol hands the DTB's physical address in x0; if it looks like
 * an FDT (magic 0xd00dfeed, big-endian on the wire), the guest scans the blob
 * for the method value "hvc\0" and uses HVC, falling back to SMC otherwise --
 * which is oemu's D4 conduit. Measured on qemu-system-aarch64 8.2.2 with the
 * M5 baseline flags: x0 = 0x48000000, /psci method = hvc, and the PSCI model
 * answers NOT_SUPPORTED (-1) unless PSCI_VERSION negotiated first, so the
 * guest negotiates before SYSTEM_OFF exactly like Linux does. No shared
 * definitions with src/; magic and layout per the FDT specification and
 * Documentation/arch/arm64/booting.rst.
 */
	.text
	.globl _start

_start_of_image:
	b	_start
	.word	0
	.quad	0x80000
	.quad	_image_end - _start_of_image
	.quad	0x2                          /* LE, 4K pages */
	.quad	0
	.quad	0
	.quad	0
	.word	0x644d5241                   /* "ARM\x64" */
	.word	0

_start:
	/* Reserve a stack below the image: the print helpers are non-leaf and
	 * must spill x30 across their own calls. 0x4007F000 is 16-byte aligned
	 * RAM the image never touches. */
	movz	x9, #0xF000
	movk	x9, #0x4007, lsl #16
	mov	sp, x9

	/* The boot protocol hands the DTB PA in x0 (booting.rst); every call
	 * below clobbers x0, so the probe's input is parked in x20 first. */
	mov	x20, x0

	adr	x0, str_ok
	bl	puts

	/* Pick the conduit from the DTB's /psci method: "hvc" -> HVC,
	 * anything else (or no DTB) -> SMC (oemu's D4 contract). */
	mov	x0, x20
	bl	probe_conduit
	cbnz	x0, use_hvc
	adr	x0, str_smc
	bl	puts
	/* Negotiate first (as the kernel does): QEMU's PSCI model starts at
	 * 0.1 and only serves SYSTEM_OFF after PSCI_VERSION returns. */
	movz	x0, #0x0
	movk	x0, #0x8400, lsl #16     /* 0x84000000 PSCI_VERSION */
	smc	#0
	movz	x0, #0x8
	movk	x0, #0x8400, lsl #16     /* 0x84000008 SYSTEM_OFF */
	smc	#0
	b	park
use_hvc:
	adr	x0, str_hvc
	bl	puts
	movz	x0, #0x0
	movk	x0, #0x8400, lsl #16     /* 0x84000000 PSCI_VERSION */
	hvc	#0
	movz	x0, #0x8
	movk	x0, #0x8400, lsl #16     /* 0x84000008 SYSTEM_OFF */
	hvc	#0
	/* Diagnose the return: a handled PSCI call returns x0 = 0
	 * (QEMU_PSCI_RET_SUCCESS); an unhandled one leaves x0 untouched. */
	mov	x21, x0
	bl	putx64
	adr	x0, str_sp
	bl	puts	/* Unreachable on a passing machine; make the deviation printable. */
	park:
	adr	x0, str_ret
	bl	puts
	wfi_loop:
	wfi
	b	wfi_loop

/*
 * probe_conduit -> x0: 1 when the DTB prescribes HVC, else 0.
 * On entry the boot protocol left the DTB PA in x0; a zero or a wrong magic
 * means "trust the machine default" (SMC).
 */
probe_conduit:
	stp	x29, x30, [sp, #-16]!
	mov	x0, x20                      /* the parked DTB pointer */
	cbz	x0, 10f                      /* no DTB pointer: default SMC */
	/* Diagnose before deciding: entry x0 and its first word. All prints
	 * ride x20/x21, which the printers do not clobber. */
	mov	x21, x20
	bl	putx64
	adr	x0, str_sp
	bl	puts
	ldr	w0, [x20]                    /* first word, as read (LE) */
	bl	putx32
	adr	x0, str_sp
	bl	puts
	ldr	w1, [x20]                    /* FDT magic, big-endian on the wire */
	rev	w1, w1                       /* -> host order: 0xd00dfeed */
	movz	w2, #0xfeed                  /* low half first: 0xD00DFEED */
	movk	w2, #0xd00d, lsl #16
	cmp	w1, w2
	b.ne	11f
	/* Scan the whole blob for "hvc\0" (method value bytes): word LE
	 * 0x00637668. totsize is BE at +4; bound the scan by it. */
	ldr	w1, [x20, #4]
	rev	w1, w1
	cbz	w1, 9f
	movz	w2, #0x7668                  /* 'h' 'v' */
	movk	w2, #0x0063, lsl #16         /* 'c' '\0' -> 0x00637668 */
	mov	x3, x20                      /* cursor */
	add	x4, x20, x1                  /* end = base + totsize */
1:	ldrb	w5, [x3]
	cmp	w5, #'h'
	b.ne	2f
	ldrb	w5, [x3, #1]
	cmp	w5, #'v'
	b.ne	2f
	ldrb	w5, [x3, #2]
	cmp	w5, #'c'
	b.ne	2f
	ldrb	w5, [x3, #3]
	cbz	w5, 8f                       /* "hvc\0" found */
2:	add	x3, x3, #1
	cmp	x3, x4
	b.lo	1b
9:	mov	x0, #0                       /* scanned, no hvc: SMC */
	ldp	x29, x30, [sp], #16
	ret
10:	adr	x0, str_no0
	bl	puts
	mov	x0, #0
	ldp	x29, x30, [sp], #16
	ret
11:	adr	x0, str_bad
	bl	puts
	mov	x0, #0
	ldp	x29, x30, [sp], #16
	ret
8:	mov	x0, #1                       /* DTB says HVC */
	ldp	x29, x30, [sp], #16
	ret

/* putx32: print w0 as 8 uppercase hex digits. The value parks in x19 and the
 * shift counter in x9: putc clobbers x0-x2, and callers may rely on x10. */
putx32:
	stp	x29, x30, [sp, #-16]!
	mov	x19, x0
	mov	x9, #32
1:	sub	x9, x9, #4
	lsr	x3, x19, x9            /* 64-bit shift: x19 is the parked value */
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

/* putx64: print the address in x21 as 16 hex digits. Non-leaf. */
putx64:
	stp	x29, x30, [sp, #-16]!
	lsr	x0, x21, #32
	bl	putx32
	mov	x0, x21
	bl	putx32
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

.section .rodata
str_ok:		.asciz "PSCI-OK\n"
str_smc:	.asciz "PSCI-SMC\n"
str_hvc:	.asciz "PSCI-HVC\n"
str_no0:	.asciz "PSCI-NODTB\n"
str_bad:	.asciz "PSCI-BADFDT\n"
str_sp:		.asciz ":"
str_ret:	.asciz "PSCI-RETURNED\n"

_image_end:
