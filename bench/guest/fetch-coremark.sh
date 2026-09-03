#!/usr/bin/env bash
#
# Fetch CoreMark for the end-to-end benchmark phase.
#
# CoreMark (https://github.com/eembc/coremark, GPL-2.0 WITH GCC-exception-v3)
# is the standard integer benchmark for interpreter/emulator measurements.
# It is fetched, never vendored: the GPL guest image is fine as benchmark
# input, but keeping it out of the repository keeps the project's own
# licensing story simple.
#
# CoreMark's stock linux port needs a libc; running it under oemu means
# either linking it against a static musl sysroot once one exists, or
# writing an oemu guest port (a thin variant of core_portme.c using the
# three SVC wrappers in this directory). That port is the E2E milestone,
# not this script.
set -euo pipefail

here="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dest="$here/coremark-src"

if [ -d "$dest/.git" ]; then
  echo "fetch-coremark.sh: already present at $dest ($(git -C "$dest" rev-parse --short HEAD))"
  exit 0
fi

git clone --depth 1 https://github.com/eembc/coremark.git "$dest"
echo "fetch-coremark.sh: cloned to $dest"
echo "next: build.sh's link probe covers the linker; a static musl sysroot or" \
     "an oemu guest port of core_portme.c is still required to run it."
