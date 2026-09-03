#!/usr/bin/env bash
#
# Cross-build the freestanding guest benchmark into static AArch64 ELFs.
#
# Deliberately linker-agnostic and toolchain-optional: this machine's clang
# can compile AArch64 but its lld/gold builds are trimmed and lack the AArch64
# emulation, so the script probes what actually links rather than assuming.
# When nothing links it prints the one-time apt fix and exits 3 -- the decode
# corpus path (gen.sh + decode_bench) works without any of this.
#
# Outputs (gitignored):
#   build/oemu-guest-bench          deterministic stdout, for E2E correctness
#   build/oemu-guest-bench-timing   adds an elapsed_ns line, for throughput
set -euo pipefail

here="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/build"

common_cflags=(
  -ffreestanding -O2
  -fno-stack-protector -fno-asynchronous-unwind-tables
  -Wall -Wextra -Werror
)

# Each candidate is a driver plus the link flags that route it through an
# AArch64-capable linker. The probe (not the name) decides.
candidates=(
  "aarch64-linux-gnu-gcc|"
  "clang|-fuse-ld=mold"
  "clang|-fuse-ld=lld -Wl,-m,elf_aarch64"
  "clang|-fuse-ld=gold -Wl,-m,aarch64linux"
  "clang|"
)

mkdir -p "$out"
probe="$(mktemp -d)"
trap 'rm -rf "$probe"' EXIT

printf 'void _start(void){ __asm__ volatile("mov x8, #94\\nmov x0, #0\\nsvc #0"); }\n' \
  >"$probe/probe.c"

chosen=""
chosen_flags=""
arch_flag=""
for entry in "${candidates[@]}"; do
  driver="${entry%%|*}"
  flags="${entry#*|}"
  command -v "$driver" >/dev/null 2>&1 || continue

  case "$driver" in
    clang) try_arch="--target=aarch64-unknown-none" ;;
    *)     try_arch="" ;;
  esac

  # shellcheck disable=SC2086 -- candidate flags are intentionally word-split
  if "$driver" $try_arch "${common_cflags[@]}" -c -o "$probe/probe.o" "$probe/probe.c" 2>/dev/null &&
     "$driver" $flags -static -nostdlib -T "$here/guest.ld" -o "$probe/probe" "$probe/probe.o" 2>/dev/null; then
    chosen="$driver"
    chosen_flags="$flags"
    arch_flag="$try_arch"
    break
  fi
done

if [ -z "$chosen" ]; then
  cat >&2 <<'MSG'
build.sh: no working AArch64 link path found.
Tried: aarch64-linux-gnu-gcc, clang+{mold,lld,gold}, bare clang.

This host's stock lld and gold were built without AArch64 emulation, so one
of the following installs is needed (one-time):

  sudo apt-get install -y binutils-aarch64-linux-gnu   # GNU cross ld + as
  # or
  sudo apt-get install -y mold                        # drop-in multi-arch ld

The decode-side benchmark (make bench) needs none of this.
MSG
  exit 3
fi

echo "build.sh: link mode: ${chosen} ${chosen_flags:-<driver default>}"

compile() { # compile <src-base> <out-obj> [extra-flags...]
  local src="$1" obj="$2"
  shift 2
  # shellcheck disable=SC2086
  "$chosen" $arch_flag "${common_cflags[@]}" "$@" -c -o "$out/$obj" "$here/$src.c"
}

link() { # link <out-bin> <objs...>
  local bin="$1"
  shift
  # shellcheck disable=SC2086
  "$chosen" $chosen_flags -static -nostdlib -T "$here/guest.ld" \
    -o "$out/$bin" "$@"
}

compile guest_syscalls guest_syscalls.o
compile crt0 crt0.o
compile guest_bench guest_bench.o
compile guest_bench guest_bench_timing.o -DGUEST_TIMING

link oemu-guest-bench "$out/guest_syscalls.o" "$out/crt0.o" "$out/guest_bench.o"
link oemu-guest-bench-timing "$out/guest_syscalls.o" "$out/crt0.o" "$out/guest_bench_timing.o"

if command -v llvm-readelf >/dev/null 2>&1; then
  llvm-readelf -h "$out/oemu-guest-bench" | grep -E 'Machine|Type:|Entry' | sed 's/^/  /'
fi

size="$(stat -c '%s' "$out/oemu-guest-bench")"
echo "build.sh: $out/oemu-guest-bench ($size bytes) and -timing variant built"
