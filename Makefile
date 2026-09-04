# Thin convenience wrapper around CMake presets.
#
# Nothing here is required to build the project -- `cmake --preset debug` works
# on its own. These targets just shorten the common local loops.

CMAKE   ?= cmake
CTEST   ?= ctest
PRESET  ?= debug
BUILD   := build/$(PRESET)
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)

# Extra arguments forwarded to ctest, e.g. `make test ARGS="-R Buffer"`.
ARGS ?=

# The ELF that `make run` / `make bench-e2e` boot. Defaults to the freestanding
# guest benchmark; build it first with `make bench-guest` (needs an AArch64
# linker), or point GUEST at any static ET_EXEC AArch64 image.
GUEST ?= bench/guest/build/oemu-guest-bench

.DEFAULT_GOAL := test

.PHONY: help
help: ## Show this help
	@printf 'oemu development targets\n\n'
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "} {printf "  \033[36m%-16s\033[0m %s\n", $$1, $$2}'
	@printf '\nVariables: PRESET=%s JOBS=%s ARGS="%s"\n' '$(PRESET)' '$(JOBS)' '$(ARGS)'
	@printf 'Presets:   debug release asan tsan coverage clang\n'

.PHONY: configure
configure: ## Configure the current preset
	$(CMAKE) --preset $(PRESET)

# Order-only style guard: configure whenever the cache is missing.
$(BUILD)/CMakeCache.txt:
	$(CMAKE) --preset $(PRESET)

.PHONY: build
build: $(BUILD)/CMakeCache.txt ## Build the current preset
	$(CMAKE) --build $(BUILD) --parallel $(JOBS)

.PHONY: test
test: build ## Build then run the test suite
	$(CTEST) --test-dir $(BUILD) --output-on-failure --parallel $(JOBS) $(ARGS)

.PHONY: retest
retest: build ## Re-run only the tests that failed last time
	$(CTEST) --test-dir $(BUILD) --output-on-failure --rerun-failed

.PHONY: list-tests
list-tests: build ## List every registered test case
	$(CTEST) --test-dir $(BUILD) -N

.PHONY: asan
asan: ## Run the suite under ASan + UBSan
	$(MAKE) test PRESET=asan

.PHONY: tsan
tsan: ## Run the suite under ThreadSanitizer
	$(MAKE) test PRESET=tsan

.PHONY: release
release: ## Build and test the release configuration
	$(MAKE) test PRESET=release

.PHONY: clang
clang: ## Build and test using clang
	$(MAKE) test PRESET=clang

.PHONY: coverage
coverage: ## Generate the HTML coverage report (needs lcov)
	$(CMAKE) --preset coverage
	$(CMAKE) --build build/coverage --parallel $(JOBS)
	$(CMAKE) --build build/coverage --target coverage

.PHONY: coverage-summary
coverage-summary: ## Print a text coverage summary (gcov only, no lcov needed)
	$(CMAKE) --preset coverage
	$(CMAKE) --build build/coverage --parallel $(JOBS)
	$(CMAKE) --build build/coverage --target coverage-summary

.PHONY: test-fast
test-fast: build ## Run the suite without the slower death tests
	$(CTEST) --test-dir $(BUILD) --output-on-failure --parallel $(JOBS) -LE death

.PHONY: test-death
test-death: build ## Run only the death tests
	$(CTEST) --test-dir $(BUILD) --output-on-failure -L death

.PHONY: run
run: build ## Run the CLI on $(GUEST)
	$(BUILD)/bin/oemu run $(GUEST)

# --- benchmarks ---------------------------------------------------------------

.PHONY: bench
bench: build ## Run the decode-throughput benchmark over bench/corpus blobs
	$(BUILD)/bin/oemu-bench-decode

.PHONY: bench-exec
bench-exec: build ## Run the end-to-end executor-throughput benchmark
	$(BUILD)/bin/oemu-bench-exec

# Boots the freestanding guest under the oemu CLI and lets its stdout through.
# The guest is cross-built; build.sh exits 3 when no AArch64 linker is present,
# which is treated here as a clean skip so this target never reports a false
# failure on a host (or CI runner) that lacks one. Byte-comparing the guest's
# stdout against a recorded golden is the follow-up step and needs the golden
# generated on a machine that has the linker.
.PHONY: bench-e2e
bench-e2e: build ## Run the guest under oemu (skips cleanly with no AArch64 linker)
	@bash bench/guest/build.sh; rc=$$?; \
	if [ $$rc -eq 3 ]; then \
		echo "bench-e2e: skipped -- no AArch64 linker (see bench/guest/build.sh)"; \
		exit 0; \
	fi; \
	if [ $$rc -ne 0 ]; then echo "bench-e2e: guest build failed (exit $$rc)" >&2; exit $$rc; fi; \
	$(BUILD)/bin/oemu run $(GUEST)

.PHONY: bench-corpus
bench-corpus: ## Regenerate the corpus blobs (needs clang with an AArch64 backend)
	bash bench/corpus/gen.sh

.PHONY: bench-guest
bench-guest: ## Cross-build the freestanding guest benchmark (needs an AArch64 linker)
	bash bench/guest/build.sh

.PHONY: compile-db
compile-db: $(BUILD)/CMakeCache.txt ## Symlink compile_commands.json for clangd
	ln -sf $(BUILD)/compile_commands.json compile_commands.json
	@echo "compile_commands.json -> $(BUILD)/compile_commands.json"

# --- code quality -------------------------------------------------------------

# Tracked sources only; keeps generated build trees out of the way. The bench
# corpus/guest kernels are included: they are cross-compiled code, but they
# live in this repository and must still satisfy clang-format.
SOURCES := $(shell find src include tests bench -type f \
	\( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null)

.PHONY: format
format: ## Rewrite sources with clang-format
	@command -v clang-format >/dev/null 2>&1 || \
		{ echo "clang-format not found (apt install clang-format)"; exit 1; }
	clang-format -i $(SOURCES)

.PHONY: format-check
format-check: ## Fail if any source deviates from clang-format
	@command -v clang-format >/dev/null 2>&1 || \
		{ echo "clang-format not found (apt install clang-format)"; exit 1; }
	clang-format --dry-run --Werror $(SOURCES)

.PHONY: tidy
tidy: compile-db ## Run clang-tidy over the C sources
	@command -v clang-tidy >/dev/null 2>&1 || \
		{ echo "clang-tidy not found (apt install clang-tidy)"; exit 1; }
	@# compile_commands.json is generated by GCC and carries GCC-only warning
	@# flags. Clang reports each as an unknown-warning-option *error*, which has
	@# nothing to do with the code under analysis, so drop that diagnostic.
	clang-tidy -p $(BUILD) \
		--extra-arg=-Wno-unknown-warning-option \
		$(shell find src -name '*.c')

# --- housekeeping -------------------------------------------------------------

.PHONY: clean
clean: ## Remove build artefacts of the current preset
	$(CMAKE) -E rm -rf $(BUILD)

.PHONY: distclean
distclean: ## Remove every build tree and the compile-db symlink
	$(CMAKE) -E rm -rf build compile_commands.json
