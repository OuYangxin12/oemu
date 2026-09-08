#!/bin/sh
#
# scripts/build-linux-initramfs.sh -- assemble the M5 initramfs.
#
# Cross-compiles tests/guest/init.c to a static aarch64 /init and packs it with
# the kernel's own gen_init_cpio (which lets us carry a character-node /dev/console
# without needing root to mknod). The product -- guest/build/initramfs.cpio -- is a
# gitignored image (the roadmap keeps kernel Images and rootfs out of the tree);
# this recipe is what reproduces it.
#
# Usage: scripts/build-linux-initramfs.sh
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
G="$root/guest"
CC="$G/toolchain/usr/bin/aarch64-linux-gnu-gcc"
STRIP="$G/toolchain/usr/bin/aarch64-linux-gnu-strip"
GEN=$(echo "$G"/src/linux-*/usr/gen_init_cpio)

export LD_LIBRARY_PATH="$G/toolchain/usr/lib/x86_64-linux-gnu"
[ -x "$CC" ] || { echo "cross gcc not found at $CC" >&2; exit 1; }
[ -x "$GEN" ] || { echo "gen_init_cpio not found at $GEN" >&2; exit 1; }

mkdir -p "$G/build"
"$CC" -static -nostdlib -ffreestanding -fno-stack-protector -Os \
  -o "$G/build/init" "$root/tests/guest/init.c"
"$STRIP" --strip-all "$G/build/init"

cat >"$G/build/initramfs.list" <<EOF
file init $G/build/init 0755 0 0
dir dev 0755 0 0
nod dev/console 0600 0 0 c 5 1
EOF

"$GEN" -t 0 "$G/build/initramfs.list" >"$G/build/initramfs.cpio"
echo "built $G/build/initramfs.cpio ($(wc -c <"$G/build/initramfs.cpio") bytes)"
