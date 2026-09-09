/*
 * bitfield_probe.S -- oracle-driven golden probe for the A64 bitfield family
 * (BFM/UBFM/SBFM and the BFI spelling), the instruction class behind issue #26.
 *
 * Each case loads an explicit source and destination, executes ONE encoding
 * spelled as `.inst` -- so the probe records the encoding, not an assembler's
 * opinion of it -- and prints `case <id>:<16 hex>` on the PL011. Run it under
 * the oracle for the goldens and under oemu for the diff:
 *
 *   scripts/build-guest.sh tests/guest/bitfield_probe.S
 *   timeout 30 qemu-system-aarch64 -machine virt,virtualization=off \
 *     -cpu cortex-a53 -m 256M -nographic -kernel build/guest/bitfield_probe.bin
 *   timeout 30 build/release/bin/oemu boot -kernel build/guest/bitfield_probe.bin -m 256
 *
 * Every case is chosen so the readings a host might guess -- extract at immR,
 * take the low `width` bits, rotate, no-op on a wrapped range -- disagree.
 * QEMU 10.2.1 (the oracle) answers as follows, and oemu answers identically after
 * the do_bitfield fix -- 24/24:
 *   01:0000000200000000  02:0000000200000000  03:1234567822222222
 *   04:FFFFFFFF00000000  05:FFFFFFFF22222222  06:1111111122567822
 *   07:FFFFFFFFFFFFFFFF  08:1111111122225678  09:BD5B7DDE2468ACF0
 *   0A:11EF123456782222  0B:0000000011DD1111  0C:00000000F000BEEF
 *   0D:1111111122223456  0E:111111112222FF00  0F:11111111222200FF
 *   10:111111112222FFFF  11:0000000200000000  12:0000000000003456
 *   13:0000000000003456  14:0000000000567800  15:DEADBEEF00006677
 *   16:FFFFFFFFFFFF1234  17:1234567855555555  18:5555555555555555
 *
 * Case 1 is the instruction that lost a dentry reference: `bfi x2, x0, #32, #32`
 * is the splice in lib/lockref.c's lockref_get, and a host that treats a
 * wrapped BFM as a no-op stores the old packed word back through the cmpxchg.
 *
 * Register discipline: putc clobbers x0/x14/x15/flags only (it parks x30 on the
 * red zone), so it is safe inside the hex loops; hexn owns x0/x9/x10/x11; emit
 * reads x19 (case id) and x20 (result). Freestanding, EL1, MMU off, no DTB, no
 * PSCI: the guest prints and then parks, so the runner uses a timeout.
 */
	.text
	.globl _start
_start_of_image:
	b	_start
	.word	0
	.quad	0x1000
	.quad	_image_end - _start
	.quad	0x2
	.quad	0
	.quad	0
	.quad	0
	.word	0x644d5241
	.word	0
	.balign	0x1000

	.macro	SET64 r, val
	movz	\r, #((\val) & 0xffff)
	movk	\r, #(((\val) >> 16) & 0xffff), lsl #16
	movk	\r, #(((\val) >> 32) & 0xffff), lsl #32
	movk	\r, #(((\val) >> 48) & 0xffff), lsl #48
	.endm


	.macro	PROPER id, enc, src, dst
	SET64	x1, \src
	SET64	x0, \dst
	.inst	\enc
	mov	x20, x0
	mov	w19, #\id
	bl	emit
	.endm
	.macro	PROBE id, enc, src, dst
	SET64	x0, \src
	SET64	x1, \dst
	mov	x2, x1
	.inst	\enc
	mov	x20, x2
	mov	w19, #\id
	bl	emit
	.endm
_start:
	SET64	x9, 0x4007F000
	mov	sp, x9
	adr	x0, str_hdr
	bl	puts
	PROBE	1, 0xB3607C02, 0x0000000000000002, 0x0000000100000000 /* bfi x2,x0,#32,#32 */
	PROBE	2, 0xB3607C02, 0x0000000100000002, 0x0000000100000000 /* bfi x2,x0,#32,#32 (src hi set) */
	PROBE	3, 0xB3607C02, 0xDEADBEEF12345678, 0x1111111122222222 /* bfi x2,x0,#32,#32 */
	PROBE	4, 0xB3607C02, 0x00000000FFFFFFFF, 0xFFFFFFFF00000000 /* bfi x2,x0,#32,#32 */
	PROBE	5, 0xB3607C02, 0xFFFFFFFFFFFFFFFF, 0x1111111122222222 /* bfi x2,x0,#32,#32 (all ones) */
	PROBE	6, 0xB3783C02, 0xDEADBEEF12345678, 0x1111111122222222 /* bfi x2,x0,#8,#16 */
	PROBE	7, 0xB3783C02, 0x000000000000FFFF, 0xFFFFFFFFFFFFFFFF /* bfi x2,x0,#8,#16 */
	PROBE	8, 0xB3403C02, 0xDEADBEEF12345678, 0x1111111122222222 /* bfi x2,x0,#0,#16 */
	PROBE	9, 0xB37FF802, 0xDEADBEEF12345678, 0x1111111122222222 /* bfi x2,x0,#1,#63 */
	PROBE	10, 0xB3709C02, 0xDEADBEEF12345678, 0x1111111122222222 /* bfi x2,x0,#16,#40 */
	PROBE	11, 0x33101C02, 0x00000000AABBCCDD, 0x0000000011111111 /* bfi w2,w0,#16,#8 */
	PROBE	12, 0x33003C02, 0x00000000DEADBEEF, 0x00000000F000000F /* bfi w2,w0,#0,#16 */
	PROBE	13, 0xB3485C02, 0xDEADBEEF12345678, 0x1111111122222222 /* bfm x2,x0,#8,#23 */
	PROBE	14, 0xB3485C02, 0x0000000000FF0000, 0x1111111122222222 /* bfm x2,x0,#8,#23 */
	PROBE	15, 0xB3485C02, 0x000000000000FFFF, 0x1111111122222222 /* bfm x2,x0,#8,#23 */
	PROBE	16, 0xB3403C02, 0x000000000000FFFF, 0x1111111122222222 /* bfm x2,x0,#0,#15 */
	PROBE	17, 0xD3607C02, 0x0000000000000002, 0x1111111122222222 /* ubfm x2,x0,#32,#31 = lsl #32 */
	PROBE	18, 0xD3485C02, 0xDEADBEEF12345678, 0x1111111122222222 /* ubfm x2,x0,#8,#23 */
	PROBE	19, 0x93485C02, 0xDEADBEEF12345678, 0x1111111122222222 /* sbfm x2,x0,#8,#23 */
	PROBE	20, 0xD3783C02, 0xDEADBEEF12345678, 0x1111111122222222 /* ubfm x2,x0,#56,#15 = lsl #8 */
	PROPER	21, 0xB3485C20, 0x1122334455667788, 0xDEADBEEF00000000 /* bfxil x0,x1,#8,#16 */
	PROPER	22, 0xB3485C20, 0x0000000000123400, 0xFFFFFFFFFFFFFFFF /* bfxil x0,x1,#8,#16 */
	PROPER	23, 0xB3607C20, 0x0000000012345678, 0xAAAAAAAA55555555 /* bfi x0,x1,#32,#32 */
	PROPER	24, 0xB37FF820, 0xAAAAAAAAAAAAAAAA, 0x0000000000000001 /* bfi x0,x1,#1,#63 */
	adr	x0, str_end
	bl	puts
1:	wfi
	b	1b

emit:
	str	x30, [sp, #-16]!
	adr	x0, str_case
	bl	puts
	mov	x10, x19
	mov	x11, #2
	mov	x9, #4
	bl	hexn
	adr	x0, str_colon
	bl	puts
	mov	x10, x20
	mov	x11, #16
	mov	x9, #60
	bl	hexn
	bl	crlf
	ldr	x30, [sp], #16
	ret

hexn:
	str	x30, [sp, #-16]!
1:	lsr	x0, x10, x9
	and	x0, x0, #0xf
	mov	x1, x0
	add	x1, x1, #7
	cmp	x0, #10
	csel	x0, x1, x0, ge
	add	x0, x0, #'0'
	bl	putc
	sub	x9, x9, #4
	subs	x11, x11, #1
	b.ne	1b
	ldr	x30, [sp], #16
	ret

puts:
	str	x30, [sp, #-16]!
1:	ldrb	w1, [x0], #1
	cbz	w1, 2f
	mov	x9, x0
	mov	x0, x1
	bl	putc
	mov	x0, x9
	b	1b
2:	ldr	x30, [sp], #16
	ret

crlf:
	adr	x0, str_crlf
	b	puts

putc:
	str	x30, [sp, #-16]!
	movz	x15, #0x0000
	movk	x15, #0x0900, lsl #16
1:	ldr	w14, [x15, #0x18]
	tst	w14, #0x20
	b.ne	1b
	str	w0, [x15]
	ldr	x30, [sp], #16
	ret

str_hdr:   .asciz "\r\nBITFIELD-PROBE-BEGIN\r\n"
str_case:  .asciz "case "
str_colon: .asciz ":"
str_crlf:  .asciz "\r\n"
str_end:   .asciz "\r\nBITFIELD-PROBE-END\r\n"
	.quad	0
_image_end:
