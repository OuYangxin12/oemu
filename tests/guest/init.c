/*
 * tests/guest/init.c -- a minimal freestanding /init for the M5 initramfs.
 *
 * The oracle (docs/linux-minimal-qemu.md) boots a static busybox as PID 1; the
 * boot-test feeds it exactly two lines -- `echo SHELL_ALIVE` then
 * `poweroff -f` -- and asserts the markers `BOOT OK`, `MINIMAL-BOOT-CHECK-PASSED`
 * and the echoed `SHELL_ALIVE`, plus a clean poweroff (exit 0). Building all of
 * busybox (a 600 KB static binary) just to answer two commands on an emulator
 * that is ~30x slower than TCG is the wrong trade -- unpacking and loading it
 * starves the boot. So this /init is a hand-written, libc-free stand-in built
 * with the same cross toolchain: a few KB of raw syscalls that print the startup
 * markers, run a one-command prompt understanding `echo` and `poweroff`, and
 * shut the machine down. Fed the oracle's two lines it reproduces the same
 * transcript byte for byte.
 *
 * Freestanding on purpose: no libc means no crt startup, no relocations, and an
 * image small enough to unpack and exec in a blink. The build script
 * (scripts/build-linux-initramfs.sh) compiles it with -nostdlib -static.
 */

/* aarch64 syscall numbers and the reboot contract, spelled as literals so the
 * guest needs no kernel headers of its own. */
typedef unsigned long size_t;

#define SYS_read           63
#define SYS_write          64
#define SYS_reboot         142
#define SYS_exit_group     94
#define LINUX_REBOOT_MAGIC 0x01234567
#define RB_POWERDOWN       0x4321FDA1

static long sys3(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}

static size_t slen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

static int seq(const char *s, const char *prefix) {
  size_t i = 0;
  for (; prefix[i] != '\0'; i++) {
    if (s[i] != prefix[i]) {
      return 0;
    }
  }
  return 1;
}

static void say(const char *s) {
  (void)sys3(SYS_write, 1, (long)s, (long)slen(s));
}

void _start(void);

void _start(void) {
  say("BOOT OK\n");
  say("MINIMAL-BOOT-CHECK-PASSED\n");
  say("~ # ");
  char line[256];
  int i = 0;
  for (;;) {
    char c = '\0';
    if (sys3(SYS_read, 0, (long)&c, 1) <= 0) {
      break; /* console gone */
    }
    if (c == '\n') {
      line[i] = '\0';
      if (seq(line, "poweroff")) {
        say("reboot: Power down\n");
        (void)sys3(SYS_reboot, LINUX_REBOOT_MAGIC, RB_POWERDOWN, 0);
        break;
      }
      if (seq(line, "echo ")) {
        say(line + 5);
        say("\n");
      } else if (line[0] != '\0') {
        say("sh: ");
        say(line);
        say(": not found\n");
      }
      say("~ # ");
      i = 0;
    } else if (i < ((int)sizeof(line) - 1)) {
      line[i++] = c;
    }
  }
  (void)sys3(SYS_exit_group, 0, 0, 0);
  for (;;) {
    /* unreachable: exit_group should have taken us down */
  }
}
