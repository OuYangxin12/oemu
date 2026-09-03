# Host-side benchmarks.
#
# A benchmark is a consumer of the library, not a test: it links against
# `oemu` like any user would, and it deliberately pulls in no GoogleTest and
# no google-benchmark. Two targets live here: one times oemu_decode() over real
# AArch64 corpus blobs, the other times the whole executor running a
# self-contained guest. See bench/README.md for both, and for the cross-built
# freestanding guest phase under bench/guest/.

add_executable(oemu_bench_decode c/decode_bench.c)

target_link_libraries(oemu_bench_decode PRIVATE
  oemu
  oemu_compiler_flags
)

# Default corpus location: the blobs are committed, so `make bench` never
# requires the cross toolchain that produced them.
target_compile_definitions(oemu_bench_decode PRIVATE
  OEMU_BENCH_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus/blob"
)

set_target_properties(oemu_bench_decode PROPERTIES OUTPUT_NAME oemu-bench-decode)

# End-to-end executor throughput: generates a self-contained AArch64 compute
# loop and times oemu_exec_run() retiring it. Needs no corpus and no cross
# toolchain -- the guest is emitted by encoders in the source (see the file).
add_executable(oemu_bench_exec c/exec_bench.c)

target_link_libraries(oemu_bench_exec PRIVATE
  oemu
  oemu_compiler_flags
)

set_target_properties(oemu_bench_exec PROPERTIES OUTPUT_NAME oemu-bench-exec)
