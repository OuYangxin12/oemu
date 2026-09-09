/*
 * tests/guest/probe.c -- a diagnostic /init that reports each write's return code.
 *
 * `MINIMAL-BOOT-CHECK-PASSED` going missing in the boot gate said "the console
 * stalls" but not *where*: the tty core, the driver, or us. This fixture answers
 * that in one boot: it writes a short line, then a 26- and a 64-byte line to
 * fd 1, printing "W(fd=.. n=..) -> ret" around every call, then tries a second
 * open of /dev/ttyAMA0 with O_NONBLOCK. Under QEMU every write returns its full
 * length; under oemu the first line lands and nothing after it does. Build it
 * with the host clang (no cross toolchain here) and pack it with
 * scripts/mkcpio.py -- see docs/booting-linux.md.
 *
 *   clang --target=aarch64-none-elf -fuse-ld=lld -nostdlib -static -O1 \
 *       -fno-builtin -Wl,-e,_start -o /tmp/probe_init tests/guest/probe.c
 *   python3 scripts/mkcpio.py .dsh2/probe.cpio init=/tmp/probe_init
 */
typedef unsigned long size_t;
#define SYS_read 63
#define SYS_write 64
#define SYS_open 56
#define SYS_close 57
#define SYS_reboot 142
#define SYS_exit_group 94
static long sys3(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n; register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b; register long x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { (void)sys3(SYS_write, 1, (long)s, (long)slen(s)); }
static char hx[19];
static const char *hex(long v) {
  static const char d[] = "0123456789abcdef";
  unsigned long u = (unsigned long)v;
  int i = 16; hx[18] = 0;
  for (; i >= 0; i--) { hx[i] = d[u & 0xF]; u >>= 4; }
  hx[17] = 'h'; return hx;
}
static void wr(int fd, const char *s, long n) {
  say("W(fd="); say(hex(fd)); say(" n="); say(hex(n)); say(") -> ");
  long r = sys3(SYS_write, fd, (long)s, n);
  say(hex(r)); say("\n");
}
static const char l26[] = "PROBE-LONG-LINE-0123456789\n";
static const char l64[] = "PROBE-64-0123456789012345678901234567890123456789012345678901234\n";
static const char dev[] = "/dev/ttyAMA0";
void _start(void) {
  say("PROBE-A\n");                                   /* first short write */
  wr(1, l26, 26);                                     /* long write on fd 1 */
  wr(1, l64, 64);                                     /* longer write on fd 1 */
  int fd = (int)sys3(SYS_open, (long)dev, 2 | 0x800, 0); /* O_WRONLY|O_NONBLOCK */
  say("OPEN="); say(hex(fd)); say("\n");
  if (fd >= 0) { wr(fd, l26, 26); wr(fd, l64, 64); sys3(SYS_close, fd, 0, 0); }
  say("PROBE-Z\n");
  sys3(SYS_reboot, 0x01234567, 0x4321FDA1, 0);
  sys3(SYS_exit_group, 0, 0, 0);
  for (;;) {}
}
