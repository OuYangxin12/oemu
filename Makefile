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
run: build ## Run the demo executable
	$(BUILD)/bin/oemu

.PHONY: compile-db
compile-db: $(BUILD)/CMakeCache.txt ## Symlink compile_commands.json for clangd
	ln -sf $(BUILD)/compile_commands.json compile_commands.json
	@echo "compile_commands.json -> $(BUILD)/compile_commands.json"

# --- code quality -------------------------------------------------------------

# Tracked sources only; keeps generated build trees out of the way.
SOURCES := $(shell find src include tests -type f \
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
	clang-tidy -p $(BUILD) $(shell find src -name '*.c')

# --- housekeeping -------------------------------------------------------------

.PHONY: clean
clean: ## Remove build artefacts of the current preset
	$(CMAKE) -E rm -rf $(BUILD)

.PHONY: distclean
distclean: ## Remove every build tree and the compile-db symlink
	$(CMAKE) -E rm -rf build compile_commands.json
