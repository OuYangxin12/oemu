#!/usr/bin/env bash
#
# Regenerate the decoder corpus.
#
# Cross-compiles each k_*.c kernel in this directory to an AArch64 object and
# extracts its .text into a raw little-endian word stream under blob/. The
# host benchmark (bench/c/decode_bench.c) feeds those words to oemu_decode().
#
# Only clang is needed: --target switches the backend without a full cross
# toolchain, and -ffreestanding keeps the objects free of any libc reference.
# The blobs are committed, so running the benchmark never requires this
# script; re-run it after changing a kernel, and expect the manifest hashes
# to move with the compiler version.
#
# Two optimisation levels on purpose: -O0 keeps the naive spill-heavy shape
# (dense STP/LDP/MOV around a big frame, what unoptimised guest code looks
# like), -O2 is the dense mixed form real guests are usually compiled with.
set -euo pipefail

here="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
blob_dir="$here/blob"
manifest="$here/manifest.txt"

clang="${CC:-clang}"
objcopy="${OBJCOPY:-llvm-objcopy}"
opt_levels=(-O0 -O2)

# -fno-stack-protector: keep the prologue shape deterministic; a guard
#   variable load would make every blob depend on TLS layout.
# -fno-builtin: stop the loop-idiom pass from replacing our byte loops with
#   calls to memcpy -- the loops themselves are the corpus.
# -fno-asynchronous-unwind-tables: the .eh_frame content is irrelevant to
#   the blob and only bloats the object.
# -mcpu=generic+nosimd: oemu's subset has no AdvSIMD/FP, and without this the
#   compiler helpfully spills __int128 through FP registers and expands
#   constant struct stores through NEON ldr q0 -- genuinely out-of-scope
#   encodings. +nosimd makes the pure-integer corpus contract something the
#   compiler enforces rather than a hope.
compile_flags=(
  --target=aarch64-unknown-none
  -mcpu=generic+nosimd
  -ffreestanding
  -nostdlib
  -fno-stack-protector
  -fno-builtin
  -fno-asynchronous-unwind-tables
  -fno-strict-aliasing
  -I "$here"
  -Wall -Wextra -Werror
  -c
)

command -v "$clang" >/dev/null 2>&1 || {
  echo "gen.sh: '$clang' not found (set CC=...)" >&2
  exit 1
}
command -v "$objcopy" >/dev/null 2>&1 || {
  echo "gen.sh: '$objcopy' not found (set OBJCOPY=...)" >&2
  exit 1
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$blob_dir"
rm -f "$blob_dir"/*.bin

clang_version="$("$clang" --version | head -1)"
manifest_new="$tmp/manifest.txt"
{
  printf '# oemu decode corpus\n'
  printf '# compiler: %s\n' "$clang_version"
  printf '# name\topt\tbytes\tinsns\tsha256\n'
} >"$manifest_new"

count=0
words_total=0

for src in "$here"/k_*.c; do
  base="$(basename "$src" .c)"
  for opt in "${opt_levels[@]}"; do
    tag="${opt#-}"                     # -O0 -> O0
    obj="$tmp/$base-$tag.o"
    blob="$blob_dir/$base-$tag.bin"

    "$clang" "${compile_flags[@]}" "$opt" -o "$obj" "$src"
    "$objcopy" -O binary --only-section=.text "$obj" "$blob"

    bytes="$(stat -c '%s' "$blob")"
    insns=$((bytes / 4))
    sha="$(sha256sum "$blob" | cut -d ' ' -f 1)"

    printf '%s\t%s\t%s\t%s\t%s\n' "$(basename "$blob")" "$tag" "$bytes" "$insns" "$sha" \
      >>"$manifest_new"
    count=$((count + 1))
    words_total=$((words_total + insns))
  done
done

mv "$manifest_new" "$manifest"
echo "gen.sh: $count blobs, $words_total words, into $blob_dir"
