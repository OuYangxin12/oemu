/*
 * Shared types for the decoder corpus kernels.
 *
 * The kernels under bench/corpus/ are never part of the host build: they are
 * cross-compiled to AArch64 objects by gen.sh and their .text is extracted
 * into raw decode-corpus blobs. They must therefore stay freestanding --
 * stdint/stddef only, no libc calls, no relocations the blob would lose.
 */
#ifndef OEMU_BENCH_CORPUS_COMMON_H
#define OEMU_BENCH_CORPUS_COMMON_H

#include <stddef.h>
#include <stdint.h>

#endif /* OEMU_BENCH_CORPUS_COMMON_H */
