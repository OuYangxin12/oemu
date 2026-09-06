#!/usr/bin/env bash
#
# build-guest.sh -- assemble one freestanding AArch64 guest into a raw Image.
#
# usage: scripts/build-guest.sh <guest.S> [more.S ...]
#
# Produces build/guest/<basename>.bin next to nothing else; build/ is
# gitignored. Stopgap tooling for the pre-staged M3/M4 guests until the
# tests/guest harness from M2c lands and absorbs them; the toolchain
# detection contract is the same as bench/guest/build.sh: no AArch64-capable
# toolchain -> exit 3 (a clean skip, never a false failure).
#
# Two steps on purpose: clang only assembles (Ubuntu's clang driver routes
# linking through the host gcc, which trips over the AArch64 object), and
# ld.lld links with --oformat=binary so the file is the exact byte image the
# AArch64 Image header describes.
set -u

clang_bin=$(command -v clang || true)
lld_bin=$(command -v ld.lld || true)
if [ -z "$clang_bin" ] || [ -z "$lld_bin" ]; then
  echo "build-guest: skipped -- need clang and ld.lld for AArch64 guests" >&2
  exit 3
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
out_dir="$repo_root/build/guest"
mkdir -p "$out_dir"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

rc=0
for src in "$@"; do
  name=$(basename "$src" .S)
  obj="$tmpdir/$name.o"
  bin="$out_dir/$name.bin"

  if ! "$clang_bin" --target=aarch64-unknown-none -march=armv8-a -c "$src" -o "$obj"; then
    echo "build-guest: $name: clang failed" >&2
    rc=1
    continue
  fi
  # The Image header sits at the start of .text; loading at text_offset puts
  # it at 0x40080000, the same place `oemu boot` maps a raw image.
  if ! "$lld_bin" -Ttext=0x40080000 -e _start --oformat=binary -o "$bin" "$obj"; then
    echo "build-guest: $name: ld.lld failed" >&2
    rc=1
    continue
  fi
  echo "build-guest: $name -> $bin ($(wc -c < "$bin") bytes)"
done
exit "$rc"
