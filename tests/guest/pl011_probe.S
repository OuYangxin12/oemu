/*
 * pl011_probe.S -- oracle probe, not an acceptance gate.
 *
 * Runs under qemu-system-aarch64 -machine virt (via scripts/qemu-oracle.sh)
 * and prints, as hex on the very UART it probes, the facts the PL011 model
 * pins against QEMU: the reset flag register, the peripheral ID table, and
 * -- the decisive one -- what happens to a DR write while the UART is
 * still disabled (CR=0 at reset, before the guest enables it). The output
 * is read by a human once and encoded into src/dev/pl011.c's comments and
 * tests; nothing parses it in CI. Re-run it whenever the oracle version
 * moves and the pin lives.
 */
	.text
	.globl _start

_start_of_image:
	b	_start
	.word	0
	.quad	0x80000
	.quad	_image_end - _start_of_image
	.quad	0x2
	.quad	0
	.quad	0
	.quad	0
	.word	0x644d5241
	.word	0

_start:
	movz	x9, #0xF000
	movk	x9, #0x4007, lsl #16
	mov	sp, x9
	movz	x19, #0x0000
	movk	x19, #0x0900, lsl #16	/* x19 = 0x09000000: the UART */

	adr	x0, msg_fr
	bl	puts
	ldr	w0, [x19, #4]		/* FR at reset */
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0x18]	/* CR at reset */
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0x1c]	/* FIFLS at reset */
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0x2c]	/* RIS at reset */
	bl	putx32
	adr	x0, msg_nl
	bl	puts

	adr	x0, msg_id
	bl	puts
	ldr	w0, [x19, #0xfe0]
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0xfe4]
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0xfe8]
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0xfec]
	bl	putx32
	adr	x0, msg_nl
	bl	puts

	adr	x0, msg_pcl
	bl	puts
	ldr	w0, [x19, #0xfd0]
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0xfd4]
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0xfd8]
	bl	putx32
	adr	x0, msg_sp
	bl	puts
	ldr	w0, [x19, #0xfdc]
	bl	putx32
	adr	x0, msg_nl
	bl	puts

	/* DR write with the UART still disabled: dropped or latched? The
	 * marker line makes the answer visible between two flush points. */
	adr	x0, msg_drop1
	bl	puts
	mov	w0, #0x41			/* 'A' while CR==0 */
	str	w0, [x19]
	adr	x0, msg_drop2
	bl	puts
	mov	w0, #0x101			/* CR: UARTEN|TXE */
	str	w0, [x19, #0x18]
	mov	w0, #0x42			/* 'B' enabled */
	str	w0, [x19]
	adr	x0, msg_end
	bl	puts

	/* PSCI conduit facts: VERSION and SYSTEM_OFF function IDs as the
	 * DTB promises (method=hvc under QEMU virt). */
	adr	x0, msg_psci
	bl	puts
	mov	x0, #0
	movk	x0, #0x8400, lsl #16	/* PSCI_VERSION */
	hvc	#0
	mov	x22, x0
	bl	putx32
	adr	x0, msg_nl
	bl	puts

	mov	x0, #0
	movk	x0, #0x8400, lsl #16	/* PSCI_VERSION again, idempotent? */
	hvc	#0
	mov	x0, #8
	movk	x0, #0x8400, lsl #16	/* SYSTEM_OFF */
	hvc	#0
1:	wfi
	b	1b

/* puts: print NUL-terminated at x0. putx32: print x0 as 8 hex digits. */
puts:
	stp	x29, x30, [sp, #-16]!
1:	ldrb	w1, [x0], #1
	cbnz	w1, 2f
	ldp	x29, x30, [sp], #16
	ret
2:	str	w1, [x19]
	b	1b

putx32:
	stp	x29, x30, [sp, #-32]!
	mov	x8, x0
	mov	x20, x30
	mov	x21, #28
1:	lsr	x0, x8, x21			/* x8 >> variable shift */
	ands	w0, w0, #0xf
	add	w1, w0, #48
	add	w2, w0, #39			/* 'a'-'0' for hex letters */
	cmp	w0, #10
	csel	w1, w2, w1, hi
	str	w1, [x19]
	subs	x21, x21, #4
	b.ge	1b
	mov	x30, x20
	ldp	x29, x30, [sp], #32
	ret

	.section .rodata
msg_fr:		.asciz	"FR0="
msg_id:		.asciz	"ID="
msg_pcl:	.asciz	"PCL="
msg_sp:		.asciz	" "
msg_nl:		.asciz	"\n"
msg_psci:	.asciz	"PSCI-VER="
msg_drop1:	.asciz	"DROP1|"
msg_drop2:	.asciz	"|DROP2|"
msg_end:	.asciz	"|PROBE-END\n"
_image_end:
