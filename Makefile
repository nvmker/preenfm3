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
        analyze

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

# --- Static analysis (cppcheck + clang-tidy) --------------------------------
# Runs over the firmware cross-build compile_commands.json. Requires the
# cross-build to be configured — the `firmware` dep ensures build/release/
# exists and its DB is fresh. Findings go to build/release/analyze-*.txt for
# triage. NOT a CI gate yet — reporting only. See
# _bmad-output/static-analysis/spike-findings.md.
#
# Tools resolved from PATH. cppcheck is PRIMARY (clean cross-compile story —
# reads the arm-none-eabi-gcc DB natively). clang-tidy is SECONDARY and may emit
# compile-error noise from arm flags it doesn't model (-mcpu/-mfpu/etc.) — that
# is expected on the first spike run and noted in the triage report.
ANALYZE_DIR ?= build/release
ANALYZE_DB  ?= $(ANALYZE_DIR)/compile_commands.json
CPPCHECK    ?= cppcheck
CLANG_TIDY  ?= clang-tidy

analyze: firmware
	@[ -f $(ANALYZE_DB) ] || (echo "ERR: $(ANALYZE_DB) missing — run 'make firmware' first" && false)
	@echo "=== cppcheck (primary) ==="
	$(CPPCHECK) --quiet --enable=warning,style --inline-suppr \
	    --suppressions-list=scripts/cppcheck-suppressions.txt \
	    --platform=arm32-wchar_t2 \
	    --project=$(ANALYZE_DB) \
	    -ifirmware/Drivers -ifirmware/Middlewares \
	    2>&1 | tee $(ANALYZE_DIR)/analyze-cppcheck.txt
	@echo "=== clang-tidy (secondary — expect some cross-compile noise) ==="
	./scripts/analyze-tidy.sh $(ANALYZE_DB) $(ANALYZE_DIR) $(ANALYZE_DIR)/analyze-clang-tidy.txt

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
