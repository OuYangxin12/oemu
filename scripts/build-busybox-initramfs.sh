#!/bin/sh
#
# scripts/build-busybox-initramfs.sh -- assemble the busybox initramfs variant
# of the M5 gate (docs/task-cards/m5-closeout.md).
#
# The M5 gate passed with a hand-written 2 KB /init (tests/guest/init.c), but the
# empirical baseline that DEFINES "oemu boots Linux" (docs/linux-minimal-qemu.md)
# runs busybox 1.37.0 statically linked. Replaying the same three-marker gate on
# a real userspace is what closes that item -- and it exercises the paths the
# minimal PID 1 never touches: exec of a 2 MB ELF, ash, /proc, slab churn from
# hundreds of --install symlinks, and the kernel's reboot(2) ABI through a real
# applet rather than our own syscall stub.
#
# The recipe is the oracle's own, transcribed from docs/linux-minimal-qemu.md:
# defconfig, then three documented deviations --
#   CONFIG_TC            off (tc.c needs CBQ, gone from modern kernel headers)
#   CONFIG_SHA*_HWACCEL  off (busybox 1.37 defines __SHA__ on aarch64 too and
#                            takes the x86 SHA-NI path -- a known upstream bug)
#   CONFIG_STATIC        on  (no libc in the initramfs)
# plus one fixture-specific patch, documented below under "reboot ABI".
#
# Products (guest/build/, gitignored -- images stay out of the tree):
#   initramfs-busybox.cpio  the initramfs proper
#   initramfs-busybox.list  the gen_init_cpio spec it was packed from
#   busybox-aarch64         the stripped static binary inside it
#
# Usage: scripts/build-busybox-initramfs.sh
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
G="$root/guest"
TC="$G/toolchain"
CC="$TC/usr/bin/aarch64-linux-gnu-gcc"
GEN=$(echo "$G"/src/linux-*/usr/gen_init_cpio)
V=1.37.0
SRC="$G/src/busybox-$V"
TARBALL="$G/src/busybox-$V.tar.bz2"
URL="https://www.busybox.net/downloads/busybox-$V.tar.bz2"

export LD_LIBRARY_PATH="$TC/usr/lib/x86_64-linux-gnu" # the unpacked binutils' private libs
export PATH="$TC/usr/bin:$PATH"                       # so CROSS_COMPILE finds strip/ar, not ours

[ -x "$CC" ]  || { echo "cross gcc not found at $CC (guest/fetch-toolchain.sh)" >&2; exit 1; }
[ -x "$GEN" ] || { echo "gen_init_cpio not found at $GEN (build usr/gen_init_cpio)" >&2; exit 1; }
HOSTCC=$(command -v cc || command -v gcc) || { echo "no host cc for busybox kconfig" >&2; exit 1; }

# ---- fetch (resumable, integrity-tested -- same discipline as fetch-kernel.sh)
mkdir -p "$G/src" "$G/build"
if [ ! -s "$TARBALL" ]; then
  echo "fetching busybox $V -> $TARBALL"
  curl -fL --retry 4 --retry-delay 3 -C - --connect-timeout 15 --max-time 1200 \
       -o "$TARBALL" "$URL"
fi
bzip2 -t "$TARBALL" || { echo "CORRUPT $TARBALL" >&2; exit 2; }
[ -d "$SRC" ] || tar -C "$G/src" -xjf "$TARBALL"
[ -f "$SRC/Makefile" ] || { echo "source tree missing at $SRC" >&2; exit 2; }

# ---- configure: defconfig plus the three documented deviations
# A config knob the recipe expects but defconfig no longer has is a drift that
# must stop the build loudly -- a silently-skipped `sed` once turned a documented
# workaround into an unbuildable tree nobody could explain.
CFG=$SRC/.config
config_off() {
  if grep -q "^CONFIG_$1=y$" "$CFG"; then
    sed -i "s|^CONFIG_$1=y\$|# CONFIG_$1 is not set|" "$CFG"
    echo "  off: $1"
  elif grep -q "^# CONFIG_$1 is not set$" "$CFG"; then
    echo "  off already: $1"
  else
    echo "recipe drift: CONFIG_$1 absent from defconfig" >&2; exit 2
  fi
}
config_on() {
  if grep -q "^CONFIG_$1=y$" "$CFG"; then
    echo "  on already: $1"
  elif grep -q "^# CONFIG_$1 is not set$" "$CFG"; then
    sed -i "s|^# CONFIG_$1 is not set\$|CONFIG_$1=y|" "$CFG"
    echo "  on: $1"
  else
    echo "recipe drift: CONFIG_$1 absent from defconfig" >&2; exit 2
  fi
}

make -C "$SRC" defconfig >/dev/null
config_off TC
config_off SHA1_HWACCEL
config_off SHA256_HWACCEL
config_on STATIC

# ---- reboot ABI: assert busybox will speak the kernel we boot
# 123e1ef burned the lesson into this repo: a fixture's only faithful reboot ABI
# is the one of the kernel it boots (the hand-written /init had shipped the
# 2.4-era RB_POWERDOWN=0x4321FDA1 and died with -EINVAL). busybox 1.37 is
# already aligned upstream -- its internal init/reboot.h carries
# RB_POWER_OFF 0x4321fedc, and the sysroot's linux/reboot.h agrees -- so the
# recipe here verifies rather than patches, and reads the authority from the
# booted tree instead of hardcoding any constant (a hardcoded "known good"
# value is how the ABI drifts out of sync unnoticed).
KERNEL_H=$(echo "$G"/src/linux-*/include/uapi/linux/reboot.h)
[ -f "$KERNEL_H" ] || { echo "no booted-tree uapi reboot.h to read the ABI from" >&2; exit 2; }
KCMD=$(sed -n 's/^#define[[:space:]]*LINUX_REBOOT_CMD_POWER_OFF[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p' "$KERNEL_H" | head -1)
BBCMD=$(sed -n 's/.*RB_POWER_OFF[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p' "$SRC/init/reboot.h" | head -1)
[ -n "$KCMD" ] && [ -n "$BBCMD" ] || { echo "could not read the poweroff constant from kernel or busybox" >&2; exit 2; }
if [ "$(echo "$KCMD" | tr 'A-F' 'a-f')" != "$(echo "$BBCMD" | tr 'A-F' 'a-f')" ]; then
  echo "ABI mismatch: booted kernel wants $KCMD, busybox would send $BBCMD" >&2
  echo "  (the 123e1ef failure mode; do not paper over it -- adapt busybox's init/reboot.h)" >&2
  exit 2
fi
echo "  poweroff ABI verified: busybox speaks $BBCMD, the booted kernel is $KCMD"

# ---- build (CROSS_COMPILE stays: strip must be the cross one -- oracle docs)
# -fno-stack-protector -fno-tree-vectorize: oemu's ID registers advertise an
# ARMv8.0 baseline with no FP/SIMD (ID_AA64*SR_EL1 all zero), and a guest
# compiled for the CPU it runs on is the contract working, not a workaround.
# Without this, gcc's stack-protector guard init emits `movi v31.4s, #0` and
# init dies of SIGILL on a machine that says no NEON. (Not
# -mgeneral-regs-only: glibc's own headers declare floating-point types, and
# the flag refuses to compile them.) The NEON memset/memcpy glibc selects at
# link time stay -- those instructions are v8.0-mandatory and oemu executes
# them for real (see decode_vec_dup, decode_ldst_vector).
make -C "$SRC" -j"$(nproc)" CROSS_COMPILE=aarch64-linux-gnu- \
     CC="aarch64-linux-gnu-gcc --sysroot=$TC" HOSTCC="$HOSTCC" \
     EXTRA_CFLAGS="-fno-stack-protector -fno-tree-vectorize" >/dev/null
BB="$SRC/busybox"  # the top-level product: _static/ only exists for explicit
                   # `make _static` builds; this one leaves the stripped ELF here
[ -f "$BB" ] || { echo "no $BB (build failed?)" >&2; exit 2; }
READELF="$TC/usr/bin/aarch64-linux-gnu-readelf"
[ -x "$READELF" ] || READELF=$(command -v readelf || true)
[ -n "$READELF" ] || { echo "no readelf: cannot prove the link is static" >&2; exit 2; }
# `if`, not `A && B`: under set -e a *static* binary (no INTERP, grep exits 1)
# would kill this script on its success path.
if "$READELF" -l "$BB" | grep -q INTERP; then
  echo "busybox came out DYNAMIC, expected static" >&2
  exit 2
fi
cp "$BB" "$G/build/busybox-aarch64"

# ---- pack: kernel's own gen_init_cpio, so /dev/console needs no root mknod.
# Layout follows docs/linux-minimal-qemu.md: /bin/sh -> /bin/busybox, /usr/{bin,sbin}
# for --install, /dev/console as a char-node spec entry.
cat >"$G/build/initramfs-busybox.list" <<EOF
dir proc 0755 0 0
dir bin 0755 0 0
dir usr 0755 0 0
dir usr/bin 0755 0 0
dir usr/sbin 0755 0 0
dir dev 0755 0 0
file init $root/tests/guest/init-busybox.sh 0755 0 0
file bin/busybox $G/build/busybox-aarch64 0755 0 0
# A symlink's TARGET resolves against the directory holding the LINK: a target
# of "bin/busybox" on /bin/sh means /bin/bin/busybox and the kernel answers the
# script's #! with "No working init found". Target absolute, as the oracle doc
# spells it: /bin/sh -> /bin/busybox.
slink bin/sh /bin/busybox 0777 0 0
nod dev/console 0600 0 0 c 5 1
EOF
"$GEN" -t 0 "$G/build/initramfs-busybox.list" >"$G/build/initramfs-busybox.cpio"
# gen_init_cpio reports a bad entry line on stderr and STILL exits 0 (a kernel
# host-tool habit), so an incomplete archive can look like success. The newc
# header stores entry names in the clear -- grep them back as the pack check.
CPIO="$G/build/initramfs-busybox.cpio"
for entry in init bin/busybox bin/sh dev/console TRAILER; do
  grep -aq "$entry" "$CPIO" || { echo "cpio is missing '$entry' (a bad list line exits 0!)" >&2; exit 2; }
done

echo "built $G/build/initramfs-busybox.cpio ($(wc -c <"$G/build/initramfs-busybox.cpio") bytes; busybox $(wc -c <"$G/build/busybox-aarch64") bytes)"
echo "gate: scripts/boot-linux-gate.sh --initrd $G/build/initramfs-busybox.cpio"
