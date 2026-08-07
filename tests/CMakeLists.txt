# preenfm3 host-side unit tests (GoogleTest).
#
# This is a STANDALONE CMake project. Do NOT configure it through
# cmake/arm-none-eabi-gcc.cmake. The firmware/bootloader/lib build with the
# Arm cross-toolchain; tests compile with the HOST compiler (g++/clang++) and
# link GoogleTest. CMake's toolchain file applies to the whole build tree, so
# tests/ cannot live inside the firmware's add_subdirectory() graph — a single
# configure invocation can only use ONE toolchain (cross OR host, not both).
#
# Configure + run (host toolchain, no -DCMAKE_TOOLCHAIN_FILE):
#   cmake -B build/test -S tests
#   cmake --build build/test -j
#   ctest --test-dir build/test --output-on-failure
# or via the Makefile wrapper:  make test
#
# Scope of THIS scaffolding: wires GoogleTest + CTest + a smoke test that
# proves the harness compiles, links, and reports. Actual coverage of firmware
# units (Sequencer serialization, Hexter DX7 import, synth math, MIDI decode)
# lands in a follow-up — those need the firmware's pure logic extracted behind
# a host-compilable shim (HAL_* / hardware stubs behind a PFM3_HOST define).
# See tests/README.md.

cmake_minimum_required(VERSION 3.20)

project(preenfm3_tests
    VERSION 1.0.0
    LANGUAGES CXX)

# C++14 to match firmware/ + bootloader/ + lib/ standards, so any firmware code
# pulled under test compiles under identical language rules on both sides.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)        # gnu++14, matching the firmware

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- GoogleTest (pinned for reproducible CI) ---------------------------------
# v1.17.0 is the current release: https://github.com/google/googletest/releases
# Bump the tag here AND the cache key in .github/workflows/tests.yml together.
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0)
# gtest_force_shared_crt avoids MSVC CRT mismatches; harmless on Linux/macOS.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Export compile_commands.json so clangd-based editors (and pi-lens) can
# resolve the FetchContent include paths (gtest/gtest.h etc.) — without this,
# the GoogleTest headers are invisible to static analysis until configure runs.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()
include(GoogleTest)

# --- The test executable -----------------------------------------------------
# One executable aggregates every *_test.cpp under tests/. Future coverage
# sessions just drop in a new *_test.cpp — GLOB CONFIGURE_DEPENDS picks it up
# on the next configure. gtest_discover_tests() registers each TEST() as its
# own ctest entry (parallel, individually runnable via `ctest -R <name>`).
file(GLOB PFM3_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

add_executable(pfm3_tests ${PFM3_TESTS})
target_compile_features(pfm3_tests PRIVATE cxx_std_14)
# gtest_main supplies main() AND transitively links gtest (PUBLIC dep) —
# linking both trips a "duplicate libraries" warning on Apple's linker.
target_link_libraries(pfm3_tests PRIVATE GTest::gtest_main)

# --- Firmware host-testability seam (see tests/SEAM.md) ----------------------
# Pulls host-compilable firmware TUs in behind the PFM3_HOST define. This block
# is the ENTIRE coupling between the test build and the firmware: no toolchain
# file, no firmware/CMakeLists.txt edit, no fake HAL headers.
set(PFM3_FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../firmware")

# Firmware headers are included flat ("Sequencer.h", "Common.h", ...), so mirror
# firmware/CMakeLists.txt's include set. BEFORE keeps firmware headers ahead of
# any future test-local header with a clashing basename.
#
# tests/host_shims goes FIRST on the path: it shadows lib/Inc/fatfs.h with a
# minimal host shim that forward-declares FIL and omits the SD-disk/HAL chain
# the real fatfs.h pulls in (see tests/host_shims/fatfs.h + SEAM.md). Every
# other firmware header resolves to the real file.
target_include_directories(pfm3_tests BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host_shims"
    "${PFM3_FW_ROOT}/Inc"
    "${PFM3_FW_ROOT}/Src"
    "${PFM3_FW_ROOT}/Src/filesystem"
    "${PFM3_FW_ROOT}/Src/hardware"
    "${PFM3_FW_ROOT}/Src/midi"
    "${PFM3_FW_ROOT}/Src/MidiController"
    "${PFM3_FW_ROOT}/Src/midipal"
    "${PFM3_FW_ROOT}/Src/SimpleEffect"
    "${PFM3_FW_ROOT}/Src/synth"
    "${PFM3_FW_ROOT}/Src/utils"
    "${PFM3_FW_ROOT}/../lib/Inc")

# Activates the in-TU seam guards in the firmware TUs (e.g. the LED/HAL block in
# Sequencer.cpp). Defined ONLY here — never in firmware/CMakeLists.txt.
target_compile_definitions(pfm3_tests PRIVATE PFM3_HOST)

# Firmware statics carry __attribute__((section(".ram_d3" / ...))) for the Arm
# linker script; on the host these are either accepted-with-note (ELF) or a
# hard error (Mach-O, where the section guard in Sequencer.cpp gates them out).
# Also silence one benign firmware-header smell that would otherwise spam the
# host build: UINT_MAX redefinition (Synth.h). (The `return "##"` writable-string
# warning in Sequencer.cpp is left visible — it's clang-only to suppress and a
# real firmware smell worth seeing.) Both flags are portable across gcc/clang.
target_compile_options(pfm3_tests PRIVATE
    -Wno-attributes
    -Wno-macro-redefined)

# Coverage sessions add their firmware TU + any collaborator stubs here.
#
# Target #1: Sequencer serialization (regression guard for the -Ofast
# unaligned-float hard-fault). The stub supplies empty bodies for the
# out-of-line Synth/FMDisplaySequencer methods Sequencer.cpp compiles-in but the
# serialization tests never invoke — see tests/SEAM.md "Contact with the code".
#
# Target #2: DX7 sysex import (crash/corruption guard on malformed sysex).
# Hexter.cpp is host-compilable verbatim (no HAL, no source guards needed). It
# references `defaultPreset` in voiceSetData()'s params-seeding memcpy; that
# symbol is defined in Presets.cpp (pure const data, clean Common.h+
# SynthState.h closure). We pull the REAL Presets.cpp rather than stubbing, so
# the golden snapshot reflects actual firmware defaults (a zero-stub would
# silently alter every field voiceSetData does not explicitly overwrite). No
# collaborator stub is needed — Hexter's functions are pure, operating only on
# the OneSynthParams POD.
target_sources(pfm3_tests PRIVATE
    "${PFM3_FW_ROOT}/Src/midi/Sequencer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/sequencer_collaborators_stub.cpp"
    "${PFM3_FW_ROOT}/Src/utils/Hexter.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Presets.cpp")

gtest_discover_tests(pfm3_tests)
