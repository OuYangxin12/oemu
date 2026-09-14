#!/bin/sh
#
# tests/guest/init-busybox.sh -- PID 1 of the busybox initramfs variant of the
# M5 gate (docs/task-cards/m5-closeout.md).
#
# Same contract as the hand-written tests/guest/init.c, in front of a real
# userspace: print BOOT OK, run our assertions, print MINIMAL-BOOT-CHECK-PASSED,
# then hand the console to an interactive shell so the gate can type a line into
# it and ask it to power off. The markers are the same three the oracle recorded
# under QEMU (docs/linux-minimal-qemu.md); what changed is who prints them.
#
# Packed into guest/build/initramfs-busybox.cpio by
# scripts/build-busybox-initramfs.sh. Run by the kernel as /init (rdinit=/init),
# so the /bin/sh symlink in the same archive must resolve.
#
# The checks below are deliberately the ones M5's own failure history says can
# silently pass: a console that exists but loses bytes, a clock that counts but
# whose jiffies never advance (the initramfs-unpack freeze of round 3), and a
# /proc nobody can read. The oracle burned two drafts of the proc check:
# /proc/cpuinfo is ARM32 lore, /proc/self/status does not resolve for PID 1,
# and /proc/uptime -- which exists and doubles as the clock check -- needs the
# proc mount to have stuck. That mount can fail while the kernel is still
# mid-namespace-setup: the first attempt is best-effort, and the retry loop
# lives after --install because sleep is an applet, and an applet only exists
# once the installer has run.
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

mount -t proc proc /proc 2>/dev/null || true   # best effort, retried below
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

# The oracle recipe mounts /proc first, then installs the applet symlinks.
# Absolute path: busybox is a multi-call binary and refuses to locate itself
# ("'busybox' is not an absolute path") when argv[0] is bare, which leaves
# every applet -- including the ones the checks below need -- uninstalled.
/bin/busybox --install -s /usr/bin

echo "BOOT OK"

fail=0
tries=0
until mount -t proc proc /proc 2>/dev/null; do
  tries=$((tries + 1))
  if [ "$tries" -ge 5 ]; then
    echo "check failed: /proc would not mount" >&2
    fail=1
    break
  fi
  sleep 1
done
if [ ! -c /dev/console ]; then
  echo "check failed: /dev/console is not a character device" >&2
  fail=1
fi
if ! cut -d' ' -f1 /proc/uptime >/dev/null; then
  echo "check failed: /proc/uptime unreadable" >&2
  fail=1
fi
# jiffies must actually move: M5 round 3 booted, printed, and froze exactly here
u1=$(cut -d. -f1 /proc/uptime)
sleep 1
u2=$(cut -d. -f1 /proc/uptime)
if [ "$u2" -lt "$((u1 + 1))" ]; then
  echo "check failed: /proc/uptime did not advance ($u1 -> $u2)" >&2
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo "MINIMAL-BOOT-CHECK-PASSED"
else
  echo "MINIMAL-BOOT-CHECK-FAILED" >&2
  exit 1
fi

cd /
export PS1='~ # '
exec /bin/sh -i </dev/console >/dev/console 2>&1
