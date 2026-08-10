# preenfm3 — top-level Makefile wrapper for the CMake build.
#
# Convenience entry point for local development and CI. Wraps the Arm GNU
# Toolchain 15.x cross-build (see CMakeLists.txt, doc/BUILDING.md). All real
# logic lives in CMake; this just drives configure + build so callers don't
# have to remember the toolchain / target-name incantations.
#
# Common usage:
#   make                 # CLEAN rebuild (Release): clean -> configure -> build firmware + bootloader
#   make debug           # CLEAN rebuild (Debug) into build/debug/ (-Og -g3 -DDEBUG)
#   make firmware        # build just the firmware   -> build/release/firmware/preenfm3.{bin,hex,elf,map}
#   make bootloader      # build just the bootloader -> build/release/bootloader/bootloader.{bin,hex,elf,map}
#   make lib             # build just the static archive -> build/release/lib/libpreenfm3lib.a
#   make clean           # wipe the entire build/ tree (all configs)
#
#   Flashing — `flash` = DFU (dfu-util, USB), `program` = OpenOCD (debug probe,
#   mirrors openocd's own `program` command). Each builds its image first if stale,
#   then flashes from $(BUILD_DIR) (default build/release/):
#   make flash             # DFU firmware        [alias for flash-firmware]
#   make flash-bootloader  # DFU bootloader
#   make program           # OpenOCD firmware    [alias for program-firmware]
#   make program-bootloader # OpenOCD bootloader
#   Append -debug to flash the Debug build (from build/debug/) instead, e.g.
#     make flash-debug  /  make flash-bootloader-debug  /  make program-debug  /  make program-bootloader-debug
#
# Build layout: every config lives under build/ — build/release/ (default) and
# build/debug/. `all` and `debug` each wipe only their OWN config subdir first,
# so the two never clobber each other; `clean` removes the whole build/ tree.
# CMake needs no changes for the nested layout — it is build-dir-agnostic.
#
# Override knobs (e.g. `make BUILD_DIR=build/o2 firmware`):
#   BUILD_DIR=<dir>      build dir (default build/release; build/debug for `-debug` flash targets)
#   BUILD_TYPE=Debug|Release  (default Release; Debug for `debug`/`-debug` targets)
#   FIRMWARE_BIN / BOOTLOADER_BIN  flash a specific artifact
#   OOCD_INTERFACE / OOCD_TARGET   openocd debugger + target scripts (default stlink / stm32h7x)
# For -G Ninja / custom toolchain bin / non-default Release optimization, see doc/BUILDING.md.

BUILD_DIR      ?= build/release
BUILD_TYPE     ?= Release
TOOLCHAIN_FILE ?= cmake/arm-none-eabi-gcc.cmake

# Flash image paths + addresses. FIRMWARE_BIN / BOOTLOADER_BIN track BUILD_DIR.
FIRMWARE_BIN   ?= $(BUILD_DIR)/firmware/preenfm3.bin
BOOTLOADER_BIN ?= $(BUILD_DIR)/bootloader/bootloader.bin
# Firmware: 0x08020000 (128 KiB after the bootloader; firmware region 1920 KiB).
# Bootloader: 0x08000000 (bank-1 start; 128 KiB).
FW_FLASH_ADDR  ?= 0x08020000
BL_FLASH_ADDR  ?= 0x08000000

# Flash tools + OpenOCD config. Override OOCD_INTERFACE / OOCD_TARGET for a
# different debugger (e.g. interface/cmsis-dap.cfg) or target script.
# See doc/BUILDING.md -> Flashing for entering DFU mode / wiring ST-LINK.
DFU_UTIL       ?= dfu-util
OOCD           ?= openocd
OOCD_INTERFACE ?= interface/stlink.cfg
OOCD_TARGET    ?= target/stm32h7x.cfg

# Reconfigure only when the cache is missing or any CMakeLists / toolchain file
# changes. cmake -B is idempotent, so this is safe and skips on incremental builds.
CMAKE_CACHE := $(BUILD_DIR)/CMakeCache.txt

# Default goal: bare `make` does a clean Release rebuild of both images (the
# `all` target), NOT the configure-only $(CMAKE_CACHE) rule below. Without this
# GNU make's default goal would be that first rule and `make` would only run
# `cmake -B` without building anything.
.DEFAULT_GOAL := all

$(CMAKE_CACHE): CMakeLists.txt $(TOOLCHAIN_FILE) \
                firmware/CMakeLists.txt bootloader/CMakeLists.txt lib/CMakeLists.txt \
                cmake/stm32-post-build.cmake
	cmake -B $(BUILD_DIR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

.PHONY: all debug configure firmware bootloader lib clean \
        flash flash-firmware flash-bootloader \
        flash-debug flash-firmware-debug flash-bootloader-debug \
        program program-firmware program-bootloader \
        program-debug program-firmware-debug program-bootloader-debug test \
        test-cov test-asan analyze

# Release: clean rebuild into build/release/ (default BUILD_DIR / BUILD_TYPE=Release).
# Wipes only build/release/ (build/debug/ is left intact), then reconfigures and
# builds both images. preenfm3lib is built transitively (both images link it).
all:
	rm -rf build/release
	$(MAKE) BUILD_DIR=build/release firmware bootloader

# Debug: clean rebuild into build/debug/ with BUILD_TYPE=Debug (-Og -g3 -DDEBUG).
# Wipes only build/debug/, leaving build/release/ intact.
debug:
	rm -rf build/debug
	$(MAKE) BUILD_DIR=build/debug BUILD_TYPE=Debug firmware bootloader

# Just (re)configure — run cmake without building.
configure: $(CMAKE_CACHE)

# Build targets. Each reuses the configured build dir (reconfiguring if stale),
# so `make firmware` works on a fresh clone with no separate configure step.
firmware: $(CMAKE_CACHE)
	cmake --build $(BUILD_DIR) --target preenfm3 -j

bootloader: $(CMAKE_CACHE)
	cmake --build $(BUILD_DIR) --target bootloader -j

# Build just the preenfm3lib static archive -> <BUILD_DIR>/lib/libpreenfm3lib.a.
lib: $(CMAKE_CACHE)
	cmake --build $(BUILD_DIR) --target preenfm3lib -j

# Remove the entire build/ tree (all configs: release, debug, ...).
clean:
	rm -rf build

# --- Host-side unit tests (GoogleTest) --------------------------------------
# Separate CMake project under tests/ — built with the HOST compiler (g++),
# NOT arm-none-eabi-g++. Configured with NO -DCMAKE_TOOLCHAIN_FILE: the Arm
# toolchain file applies tree-wide and can't co-exist with a host test build
# (see tests/README.md). Reuses build/test/ if present (cmake -B is idempotent);
# removed by `make clean` (build/test/ lives under build/).
TEST_DIR ?= build/test

test:
	cmake -B $(TEST_DIR) -S tests
	cmake --build $(TEST_DIR) -j
	ctest --test-dir $(TEST_DIR) --output-on-failure

# --- Coverage (LLVM source-based) ------------------------------------------
# Builds tests/ with clang + -fprofile-instr-generate -fcoverage-mapping, runs
# ctest, merges the per-test .profraw, and prints llvm-cov report scoped to
# firmware/Src. Result: build/test-cov/{pfm3_tests.profdata,coverage-report.txt}.
#
# WHY CLANG IS FORCED: LLVM source-based coverage requires the compiler AND
# llvm-cov/llvm-profdata to come from ONE LLVM distribution. On a dev Mac with
# Homebrew LLVM (v22) alongside Apple's CommandLineTools (v21), the default
# `clang++` on PATH (Homebrew) does NOT match the `llvm-cov`/`llvm-profdata` a
# naive build picks up — a version skew that fails at `llvm-profdata merge`
# ("unsupported instrumentation level"). So: on macOS we pin to Apple's CLT
# pair; on Linux we use the system clang + llvm pair (CI apt-installs both
# together). See tests/README.md -> Coverage run.
#
# The report is scoped to $(CURDIR)/firmware/Src (absolute, so llvm-cov matches
# the coverage mapping) so the TOTAL matches the test-coverage-plan.md 12.45%
# baseline — gtest / *_test.cpp / stub rows are excluded. The CI floor gate
# (scripts/ci/coverage-gate.sh) consumes this same report.
#
# LLVM_PROFILE_FILE uses %p (PID) because ctest invokes each TEST() as its own
# process; without %p the files overwrite. The absolute path keeps every run's
# profraw in one dir regardless of ctest's per-test cwd.
TEST_COV_DIR ?= build/test-cov
COV_FLAGS    ?= -fprofile-instr-generate -fcoverage-mapping -fno-omit-frame-pointer

ifeq ($(shell uname -s),Darwin)
    CLANG_CXX     ?= /usr/bin/clang++
    LLVM_COV      ?= /Library/Developer/CommandLineTools/usr/bin/llvm-cov
    LLVM_PROFDATA ?= /Library/Developer/CommandLineTools/usr/bin/llvm-profdata
else
    CLANG_CXX     ?= clang++
    LLVM_COV      ?= llvm-cov
    LLVM_PROFDATA ?= llvm-profdata
endif

test-cov:
	@[ -x "$(CLANG_CXX)" ]     || (echo "ERR: $(CLANG_CXX) not found" && false)
	@[ -x "$(LLVM_COV)" ]      || (echo "ERR: $(LLVM_COV) not found" && false)
	@[ -x "$(LLVM_PROFDATA)" ] || (echo "ERR: $(LLVM_PROFDATA) not found" && false)
	# Wipe before configure: CMake will NOT override a cached CMAKE_CXX_FLAGS on
	# reconfigure, so a stale build/test-cov/ from a previous (or differently-
	# flagged) run silently builds WITHOUT instrumentation and produces an empty
	# report. Mirrors the `all`/`debug` clean-rebuild contract. Coverage is a
	# deliberate measurement, not a hot loop, so the rebuild cost is acceptable.
	rm -rf $(TEST_COV_DIR)
	cmake -B $(TEST_COV_DIR) -S tests \
	    -DCMAKE_BUILD_TYPE=Debug \
	    -DCMAKE_CXX_COMPILER=$(CLANG_CXX) \
	    -DCMAKE_CXX_FLAGS="$(COV_FLAGS)" \
	    -DCMAKE_EXE_LINKER_FLAGS="$(COV_FLAGS)"
	cmake --build $(TEST_COV_DIR) -j
	LLVM_PROFILE_FILE="$(abspath $(TEST_COV_DIR))/pfm3_tests-%p.profraw" \
	    ctest --test-dir $(TEST_COV_DIR) --output-on-failure
	# Fail loudly if ctest produced no profraw (LLVM_PROFILE_FILE not honored, or
	# every test GTEST_SKIP'd): otherwise the glob below passes a literal path to
	# llvm-profdata and fails with a confusing "No such file".
	@set -- $(TEST_COV_DIR)/pfm3_tests-*.profraw; [ -e "$$1" ] || \
	    { echo "ERR: no profraw in $(TEST_COV_DIR); ctest wrote no coverage data" >&2; false; }
	$(LLVM_PROFDATA) merge -sparse $(TEST_COV_DIR)/pfm3_tests-*.profraw \
	    -o $(TEST_COV_DIR)/pfm3_tests.profdata
	# -ignore-filename-regex excludes headers (.h) so the TOTAL matches the
	# test-coverage-plan.md baseline methodology (12.45% / 12498 lines): inline
	# header code IS instrumented when #included, but the plan scopes to firmware
	# TUs (.cpp/.c). Both the local report and the CI floor gate use this scope.
	$(LLVM_COV) report $(TEST_COV_DIR)/pfm3_tests \
	    -instr-profile=$(TEST_COV_DIR)/pfm3_tests.profdata \
	    -ignore-filename-regex='\.h$$' \
	    -use-color=false $(CURDIR)/firmware/Src \
	    | tee $(TEST_COV_DIR)/coverage-report.txt

# --- ASAN + UBSAN (Debug, reporting only) -----------------------------------
# Builds tests/ under -fsanitize=address,undefined and runs ctest. REPORTING
# target (not a CI gate) — matches the `make analyze` tolerant-triage
# philosophy. The Hexter session found a real global-buffer-overflow via this
# flow under fuzzed input; later coverage phases repeat that deliberately.
# Uses the host default compiler (gcc or clang); both support this flag set.
TEST_ASAN_DIR ?= build/test-asan

test-asan:
	cmake -B $(TEST_ASAN_DIR) -S tests -DCMAKE_BUILD_TYPE=Debug \
	    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
	    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
	cmake --build $(TEST_ASAN_DIR) -j
	ctest --test-dir $(TEST_ASAN_DIR) --output-on-failure

# --- Static analysis (cppcheck + clang-tidy) --------------------------------
# Runs over the firmware cross-build compile_commands.json. Requires the
# cross-build to be configured — the `firmware` dep ensures $(BUILD_DIR)
# exists and its DB is fresh, and ANALYZE_DIR tracks BUILD_DIR by default so
# `make BUILD_DIR=build/o2 analyze` analyzes the DB that was just built there.
# Findings go to $(ANALYZE_DIR)/analyze-*.txt for triage. NOT a CI gate yet —
# reporting only. See _bmad-output/static-analysis/spike-findings.md.
#
# Tools resolved from PATH (override CPPCHECK / CLANG_TIDY). cppcheck is PRIMARY
# (clean cross-compile story — reads the arm-none-eabi-gcc DB natively).
# clang-tidy is SECONDARY and may emit compile-error noise from arm flags it
# doesn't model (-mcpu/-mfpu/etc.); that is expected on the first spike run and
# noted in the triage report. Both tools must be runnable — a missing binary or
# unreadable DB fails `make analyze` rather than silently skipping (see the
# cppcheck `bash -o pipefail -c` wrapper in the recipe, and the clang-tidy
# setup guards in scripts/analyze-tidy.sh). pipefail is invoked inline there
# rather than via a global .SHELLFLAGS because macOS ships GNU make 3.81,
# which predates .SHELLFLAGS support (added in make 4.0).
ANALYZE_DIR ?= $(BUILD_DIR)
ANALYZE_DB  ?= $(ANALYZE_DIR)/compile_commands.json
CPPCHECK    ?= cppcheck
CLANG_TIDY  ?= clang-tidy

analyze: firmware
	@[ -f $(ANALYZE_DB) ] || (echo "ERR: $(ANALYZE_DB) missing — run 'make firmware' first" && false)
	@echo "=== cppcheck (primary) ==="
	# `bash -o pipefail -c` so cppcheck's exit propagates through the `| tee`
	# pipeline (otherwise tee's exit 0 masks a missing/crashed cppcheck and
	# `make analyze` silently skips the primary analyzer).
	bash -o pipefail -c '$(CPPCHECK) --quiet --enable=warning,style --inline-suppr \
	    --suppressions-list=scripts/cppcheck-suppressions.txt \
	    --platform=arm32-wchar_t2 \
	    --project=$(ANALYZE_DB) \
	    -ifirmware/Drivers -ifirmware/Middlewares \
	    2>&1 | tee $(ANALYZE_DIR)/analyze-cppcheck.txt'
	@echo "=== clang-tidy (secondary — expect some cross-compile noise) ==="
	./scripts/analyze-tidy.sh $(ANALYZE_DB) $(ANALYZE_DIR) $(ANALYZE_DIR)/analyze-clang-tidy.txt $(CLANG_TIDY)

# --- Flashing ---------------------------------------------------------------
# `flash` = DFU (dfu-util, USB DFU mode); `program` = OpenOCD (debug probe; the
# name mirrors openocd's own `program` command). Each builds its image first if
# stale, then flashes from $(BUILD_DIR) (default build/release/). DFU needs the
# STM32H7 ROM bootloader (set BOOT0, power on); OpenOCD needs an ST-LINK.
# Append `-debug` to any target below to flash the Debug build from build/debug/.

# --- DFU (dfu-util) ---
flash: flash-firmware

flash-firmware: firmware
	$(DFU_UTIL) -a 0 -s $(FW_FLASH_ADDR):leave -D $(FIRMWARE_BIN)

flash-bootloader: bootloader
	$(DFU_UTIL) -a 0 -s $(BL_FLASH_ADDR):leave -D $(BOOTLOADER_BIN)

flash-debug: flash-firmware-debug
flash-firmware-debug:
	$(MAKE) BUILD_DIR=build/debug BUILD_TYPE=Debug flash-firmware
flash-bootloader-debug:
	$(MAKE) BUILD_DIR=build/debug BUILD_TYPE=Debug flash-bootloader

# --- OpenOCD (program) ---
program: program-firmware

program-firmware: firmware
	$(OOCD) -f $(OOCD_INTERFACE) -f $(OOCD_TARGET) -c "program $(FIRMWARE_BIN) $(FW_FLASH_ADDR) verify reset exit"

program-bootloader: bootloader
	$(OOCD) -f $(OOCD_INTERFACE) -f $(OOCD_TARGET) -c "program $(BOOTLOADER_BIN) $(BL_FLASH_ADDR) verify reset exit"

program-debug: program-firmware-debug
program-firmware-debug:
	$(MAKE) BUILD_DIR=build/debug BUILD_TYPE=Debug program-firmware
program-bootloader-debug:
	$(MAKE) BUILD_DIR=build/debug BUILD_TYPE=Debug program-bootloader
