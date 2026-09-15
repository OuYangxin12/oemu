#!/bin/sh
#
# scripts/build-bench-initramfs.sh -- assemble the in-guest benchmark initramfs.
#
# Cross-compiles bench/perf/bench-init.c (the /init driver) and
# bench/perf/oemu-perf.c (the benchmark) to static aarch64 ELFs and packs
# them with the kernel's gen_init_cpio into guest/build/initramfs-bench.cpio
# (gitignored product; this recipe reproduces it). The image runs unchanged
# under both qemu-system-aarch64 and oemu boot -- same kernel Image, same
# markers, same power-off -- which is the whole point: one workload, two
# machines, comparable numbers.
#
# The gate inside this build script: the compiled benchmark is disassembled
# and EVERY mnemonic must live in the integer subset oemu executes. FP/SIMD
# arithmetic is issue #30 (M6 scope); a benchmark that quietly grew a `movi`
# would time out on oemu and accuse the emulator of a bug it never had --
# exactly the busybox lesson. So the scan is not decoration: an unlisted
# mnemonic aborts the build.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
G="$root/guest"
CC="$G/toolchain/usr/bin/aarch64-linux-gnu-gcc"
OD="$G/toolchain/usr/bin/aarch64-linux-gnu-objdump"
STRIP="$G/toolchain/usr/bin/aarch64-linux-gnu-strip"
GEN=$(echo "$G"/src/linux-*/usr/gen_init_cpio)

export LD_LIBRARY_PATH="$G/toolchain/usr/lib/x86_64-linux-gnu"
[ -x "$CC" ] || { echo "cross gcc not found at $CC" >&2; exit 1; }
[ -x "$OD" ] || { echo "cross objdump not found at $OD" >&2; exit 1; }
[ -x "$GEN" ] || { echo "gen_init_cpio not found at $GEN" >&2; exit 1; }

mkdir -p "$G/build"

# -mgeneral-regs-only: the compiler may not emit FP/SIMD at all -- it refuses
# outright rather than trusting -fno-tree-vectorize to hold the line (the
# busybox build learned that glibc headers make that flag moot; here the
# guest is freestanding, so the flag can actually be honoured).
# -march=armv8-a+crc: the one opt-in extension, because phase_crc32 spells
# the CRC32 system instructions as inline asm and the kernel itself leans on
# that feature (oemu decodes crc32/crc32c). gcc spells the extension `crc`;
# clang would call it `crc32`.
# -fno-pic -static: ET_EXEC, fixed load address, no relocation games --
# both sides execute byte-identical instruction streams.
CFLAGS="-static -nostdlib -ffreestanding -fno-stack-protector -fno-pic \
  -march=armv8-a+crc -mgeneral-regs-only -fno-tree-vectorize \
  -O2 -Wall -Wextra -Werror"

CORPUS="$root/bench/corpus"
KERNELS=""
for k in addsub logic muldiv bitfield csel branches movewide hash memops; do
  KERNELS="$KERNELS $CORPUS/k_$k.c"
done

"$CC" $CFLAGS -I "$CORPUS" -o "$G/build/bench-init" "$root/bench/perf/bench-init.c"
"$CC" $CFLAGS -I "$CORPUS" -o "$G/build/oemu-perf" \
  "$root/bench/perf/oemu-perf.c" $KERNELS

# The instruction-subset gate. Object code may only use mnemonics on this
# list; anything else (fp/simd/atomics beyond the subset) fails the build
# with the offending line shown. Aliases are listed beside their canonical
# form (objdump prints sbfx where the decoder says sbfm): the whitelist
# matches the PRINTED mnemonic, the guarantee is about the ENCODING, and
# every alias here has a decoder case or a unit-test line proving oemu
# decodes that encoding.
allow='^(add|adds|sub|subs|cmp|cmn|and|ands|orr|eor|eors|eon|bic|orn|mvn|neg|ngc|adcs|sbcs|adc|sbc|tst|teq|mov|movz|movk|movn|adr|adrp|madd|msub|mul|smaddl|umaddl|smulh|umulh|umull|smull|udiv|sdiv|crc32b|crc32h|crc32w|crc32x|crc32cb|crc32ch|crc32cw|crc32cx|lsl|lslv|lsr|lsrv|asr|asrv|ror|rorv|sbfx|ubfx|ubfiz|sbfiz|extr|ubfm|sbfm|rev|rev16|rev32|clz|cls|rbit|stp|ldp|str|ldr|stur|sturb|sturh|ldur|ldurb|ldurh|ldursb|ldursh|ldursw|ldurw|ldrsb|ldrsh|strb|ldrb|strh|ldrh|bics|ldnr|ldxr|stxr|prfm|nop|yield|b|bl|br|blr|ret|cbz|cbnz|tbz|tbnz|csel|csinc|csinv|csneg|cset|csetm|cneg|ccmp|ccmn|svc|eret|dmb|dsb|isb|sevl|sev|wfi|wfe)$'
for bin in "$G/build/bench-init" "$G/build/oemu-perf"; do
  bad=$("$OD" -d "$bin" \
        | sed -nE 's/^\s*[0-9a-f]+:\s+[0-9a-f]{8}\s+([a-z0-9]+).*/\1/p' \
        | sort -u | grep -vE "$allow" || true)
  if [ -n "$bad" ]; then
    echo "build-bench-initramfs: $bin uses mnemonics outside the integer subset:" >&2
    echo "$bad" | sed 's/^/    /' >&2
    exit 1
  fi
done

"$STRIP" --strip-all "$G/build/bench-init" "$G/build/oemu-perf"

cat >"$G/build/initramfs-bench.list" <<EOF
file init $G/build/bench-init 0755 0 0
file bench $G/build/oemu-perf 0755 0 0
dir dev 0755 0 0
nod dev/console 0600 0 0 c 5 1
EOF

"$GEN" -t 0 "$G/build/initramfs-bench.list" >"$G/build/initramfs-bench.cpio"
echo "built $G/build/initramfs-bench.cpio ($(wc -c <"$G/build/initramfs-bench.cpio") bytes)"
