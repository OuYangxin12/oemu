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
 * The conduit is PROBED FROM THE DTB, honestly: the boot protocol hands the
 * DTB physical address in x0; the guest walks the FDT token stream (node
 * begin/end with inline padded names, properties decoded through the string
 * block), reads /psci's `method`, and travels the conduit it names. A
 * missing or malformed DTB, an absent /psci, or a non-"hvc" method falls
 * back to SMC -- oemu's D4 conduit, and the machine default a confused
 * guest should reach. Measured facts this guest encodes: the QEMU oracle
 * hands x0 = ram_base + ram_size/2 = 0x48000000; -machine virt says method
 * "hvc"; the PSCI model answers NOT_SUPPORTED until PSCI_VERSION has been
 * asked, so the guest negotiates before SYSTEM_OFF (id 0x84000002) exactly
 * like Linux does. No shared definitions with src/; magic and layout per the
 * FDT specification and Documentation/arch/arm64/booting.rst.
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

	/* Pick the conduit from the DTB's /psci method. */
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
	movz	x0, #0x2
	movk	x0, #0x8400, lsl #16     /* 0x84000002 SYSTEM_OFF (spec id) */
	smc	#0
	b	park
use_hvc:
	adr	x0, str_hvc
	bl	puts
	movz	x0, #0x0
	movk	x0, #0x8400, lsl #16     /* 0x84000000 PSCI_VERSION */
	hvc	#0
	movz	x0, #0x2
	movk	x0, #0x8400, lsl #16     /* 0x84000002 SYSTEM_OFF (spec id) */
	hvc	#0
	/* Diagnose the return: a handled PSCI call must take the machine
	 * down; landing here means the conduit call came back. */
	mov	x21, x0
	bl	putx64
	adr	x0, str_sp
	bl	puts
	b	park
park:
	adr	x0, str_ret
	bl	puts
wfi_loop:
	wfi
	b	wfi_loop

/*
 * probe_conduit -> x0: 1 when /psci method == "hvc", else 0 (SMC).
 *
 * A flat token walk, deliberately minimal -- no depth stack, no subtree
 * logic, every branch ends at the loop top:
 *   BEGIN_NODE whose name is exactly "psci" raises the in-psci flag;
 *   any other BEGIN or any END_NODE drops it;
 *   a PROP seen while the flag is up resolves its name through the
 *   string block; a name of "method" compares its 4-byte value against
 *   "hvc\0" and answers 1 on the spot.
 * Everything else -- no DTB, bad magic, /psci absent, some other
 * method -- answers 0: the SMC machine default.
 * x20 dtb base, x21 cursor, x23 in-psci, x24 strings, x25 blob end;
 * x0-x9 scratch.
 */
probe_conduit:
	stp	x29, x30, [sp, #-32]!
	str	x21, [sp, #-16]!
	mov	x21, x20
	cbz	x20, .Lpc_none
	ldr	w0, [x20]
	rev	w0, w0
	movz	w1, #0xfeed
	movk	w1, #0xd00d, lsl #16
	cmp	w0, w1
	b.ne	.Lpc_badf
	ldr	w0, [x20, #4]
	rev	w0, w0
	add	x25, x20, x0                 /* blob end */
	ldr	w0, [x20, #12]
	rev	w0, w0
	add	x24, x20, x0                 /* strings block */
	ldr	w0, [x20, #8]
	rev	w0, w0
	add	x21, x20, x0                 /* the walk starts at off_struct */
	mov	x23, #0
.Lpc_loop:
	add	x0, x21, #8
	cmp	x0, x25
	b.hi	.Lpc_default
	ldr	w0, [x21], #4
	rev	w0, w0
	cmp	w0, #1
	b.eq	.Lpc_begin
	cmp	w0, #2
	b.eq	.Lpc_end
	cmp	w0, #3
	b.eq	.Lpc_prop
	b	.Lpc_default                 /* END or nonsense */
.Lpc_begin:
	/* inline name at x21: find its NUL, then compare "psci" */
	mov	x2, x21
1:	ldrb	w3, [x2], #1
	cbnz	w3, 1b
	sub	x4, x2, x21                  /* strlen + 1 */
	add	x5, x4, #3
	and	x5, x5, #~3                  /* padded advance */
	cmp	x4, #5                       /* "psci\0" is exactly 5 */
	b.ne	.Lpc_begin_no
	ldrb	w1, [x21]
	ldrb	w2, [x21, #1]
	ldrb	w3, [x21, #2]
	ldrb	w4, [x21, #3]
	adr	x6, str_psci
	ldrb	w7, [x6]
	ldrb	w8, [x6, #1]
	ldrb	w9, [x6, #2]
	ldrb	w10, [x6, #3]
	cmp	w1, w7
	b.ne	.Lpc_begin_no
	cmp	w2, w8
	b.ne	.Lpc_begin_no
	cmp	w3, w9
	b.ne	.Lpc_begin_no
	cmp	w4, w10
	b.ne	.Lpc_begin_no
	mov	x23, #1                      /* inside /psci */
	b	.Lpc_begin_next
.Lpc_begin_no:
	mov	x23, #0
.Lpc_begin_next:
	add	x21, x21, x5
	b	.Lpc_loop
.Lpc_end:
	mov	x23, #0
	b	.Lpc_loop
.Lpc_prop:
	add	x0, x21, #8
	cmp	x0, x25
	b.hi	.Lpc_default
	ldr	w1, [x21]
	rev	w1, w1                       /* dlen: token + 4 */
	ldr	w0, [x21, #4]
	rev	w0, w0                       /* name offset: token + 8 */
	/* resolve the name both ways: by string-block offset and inline at
	 * token+12. Whichever reads "method" is the layout this tree uses;
	 * real-world blobs are split on this (QEMU writes inline, libfdt
	 * tools write offsets), so the probe accepts either. */
	add	x6, x24, x0                  /* name by string-block offset */
	add	x7, x21, #8                  /* name inline, token + 12 */
	mov	x5, x1
	add	x5, x5, #3
	and	x5, x5, #~3
	add	x4, x21, #8
	add	x4, x4, x5                   /* cursor after the padded body */
	mov	x21, x4                      /* walk on regardless */
	cbz	x23, .Lpc_loop               /* outside /psci: not our property */
	/* compare "method" at either candidate address; x3 keeps the winner */
	ldrb	w1, [x6]
	cmp	w1, #'m'
	b.eq	.Lpc_name_ok
	ldrb	w1, [x7]
	cmp	w1, #'m'
	b.ne	.Lpc_loop
	mov	x3, x7
	b	.Lpc_name_more
.Lpc_name_ok:
	mov	x3, x6
.Lpc_name_more:
	/* the literals are immediates, not adr loads: the cursor must never
	 * touch x30, the link register -- a `bl` on the way here rewrote it,
	 * and the "compare" silently graded the return address instead of
	 * bytes. */
	ldrb	w1, [x3]
	ldrb	w2, [x3, #1]
	ldrb	w3, [x3, #2]
	ldrb	w4, [x3, #3]
	ldrb	w7, [x3, #4]
	ldrb	w8, [x3, #5]
	cmp	w1, #'m'
	b.ne	.Lpc_loop
	cmp	w2, #'e'
	b.ne	.Lpc_loop
	cmp	w3, #'t'
	b.ne	.Lpc_loop
	cmp	w4, #'h'
	b.ne	.Lpc_loop
	cmp	w7, #'o'
	b.ne	.Lpc_loop
	cmp	w8, #'d'
	b.ne	.Lpc_loop
	/* the method property: its value begins at token + 12, i.e. right
	 * after the dlen and name-offset words -- x4 strided past the
	 * padded body, so step back over the padding only. */
	sub	x4, x4, x5
	ldr	w1, [x4]                     /* value bytes, LE view of "hvc\0" */
	movz	w2, #0x7668                  /* 'h' 'v' */
	movk	w2, #0x0063, lsl #16         /* 'c' '\0' */
	cmp	w1, w2
	b.ne	.Lpc_default
	mov	w0, #1                       /* the DTB says HVC */
	b	.Lpc_ret
.Lpc_default:
	mov	w0, #0
	b	.Lpc_ret
.Lpc_badf:
	adr	x0, str_bad
	bl	puts
	mov	w0, #0
	b	.Lpc_ret
.Lpc_none:
	adr	x0, str_no0
	bl	puts
	mov	w0, #0
.Lpc_ret:
	ldr	x21, [sp], #16
	ldp	x29, x30, [sp], #32
	ret

/* putx32: print w0 as 8 uppercase hex digits. The value parks in x19 and the
 * shift counter in x9: putc clobbers x0-x2, and callers may rely on x10. */
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
str_ok:		.asciz	"PSCI-OK\n"
str_smc:	.asciz	"PSCI-SMC\n"
str_hvc:	.asciz	"PSCI-HVC\n"
str_no0:	.asciz	"PSCI-NODTB\n"
str_bad:	.asciz	"PSCI-BADFDT\n"
str_sp:		.asciz	":"
str_ret:	.asciz	"PSCI-RETURNED\n"
str_psci:	.asciz	"psci"
str_method:	.asciz	"method"
str_hvcv:	.asciz	"hvc"

_image_end:
