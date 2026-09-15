/*
 * bench/perf/bench-init.c -- the /init that drives the in-guest benchmark.
 *
 * Freestanding PID 1 for guest/build/initramfs-bench.cpio: print the two
 * gate-shaped markers (so the run is recognizable to anything that greps a
 * boot transcript), execve /bench, and get out of the way. The benchmark
 * itself (bench/perf/oemu-perf.c) owns the finish line -- it powers the
 * machine down as its last act.
 *
 * execve rather than fork+wait on purpose: PID 1 replaced by the benchmark
 * means one process, one timeline, and a dead benchmark is a kernel panic
 * ("Attempted to kill init") -- loud, which is the project's failure mode,
 * never a silent half-run.
 *
 * A refused exec is reported and then powered down, so a broken build
 * finishes as a failed marker instead of a 420-second timeout.
 */

typedef unsigned long size_t;

#define SYS_write      64
#define SYS_execve     221
#define SYS_reboot     142
#define SYS_exit_group 94

#define REBOOT_MAGIC1        0xfee1dead
#define REBOOT_MAGIC2        672274793 /* this tree's LINUX_REBOOT_MAGIC2 */
#define REBOOT_CMD_POWER_OFF 0x4321FEDC

#define SVC_CLOBBERS "x4", "x5", "x6", "x7", "memory"

static long sys4(long n, long a, long b, long c, long d) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : SVC_CLOBBERS);
  return x0;
}

static long sys3(long n, long a, long b, long c) {
  return sys4(n, a, b, c, 0);
}

static size_t slen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

static void say(const char *s) {
  (void)sys3(SYS_write, 1, (long)s, (long)slen(s));
}

void _start(void);

void _start(void) {
  static char path[] = "/bench";
  static char arg0[] = "/bench";
  static char *const argv[2] = {arg0, (char *)0};
  static char *const envp[1] = {(char *)0};

  say("BOOT OK\n");
  say("BENCH-INIT-PASSED\n");
  const long rc = sys3(SYS_execve, (long)path, (long)argv, (long)envp);
  say("BENCH-INIT-EXEC-FAILED rc=-");
  {
    /* rc is a small negative errno; print it without a libc. */
    char num[8];
    size_t i = 0;
    unsigned long v = (unsigned long)(-rc);
    do {
      num[i++] = (char)('0' + (v % 10UL));
      v /= 10UL;
    } while ((v != 0UL) && (i < sizeof(num)));
    while (i > 0UL) {
      const char c = num[--i];
      (void)sys3(SYS_write, 1, (long)&c, 1);
    }
  }
  say("\n");
  (void)sys4(SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_CMD_POWER_OFF, 0);
  (void)sys3(SYS_exit_group, 1, 0, 0);
  for (;;) {
    /* unreachable */
  }
}
