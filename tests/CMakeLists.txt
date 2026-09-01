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
    LANGUAGES C CXX)
# C is enabled (alongside CXX) because firmware/Src/synth/waves.c — the
# wave-table data TU that Osc.cpp's waveTables[] initializer references — is a
# plain-C data file. Compiling it as C (not CXX) keeps it byte-identical to the
# firmware Arm build. See Target #3 in the target_sources block below.

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

# Golden-master full-render tier (Phase G0): tests/golden_master_test.cpp drives
# the real Synth::buildNewSampleBlock and compares the int32 output against a
# committed fixture under tests/golden/. PFM3_GOLDEN_DIR resolves the fixture
# directory to an absolute source path so file I/O is cwd-independent under
# ctest (which runs each TEST() from an arbitrary cwd). See
# _bmad-output/planning-artifacts/golden-master-test-plan.md.
target_compile_definitions(pfm3_tests PRIVATE
    PFM3_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# Firmware statics carry __attribute__((section(".ram_d3" / ...))) for the Arm
# linker script; on the host these are either accepted-with-note (ELF) or a
# hard error (Mach-O, where the section guard in Sequencer.cpp gates them out).
# Also silence one benign firmware-header smell that would otherwise spam the
# host build: UINT_MAX redefinition (Synth.h). (The `return "##"` writable-string
# warning in Sequencer.cpp is left visible — it's clang-only to suppress and a
# real firmware smell worth seeing.) Both flags are portable across gcc/clang.
target_compile_options(pfm3_tests PRIVATE
    -Wno-attributes
    -Wno-macro-redefined
    # Forbid FMA contraction (a*b+c fused into one FMA instruction) so clang and
    # gcc compute float mul/add separately per IEEE. Without this the two
    # compilers' default contraction policies differ and the golden-master full
    # render (chaotic FM feedback) diverges across hosts within ~1 block. See
    # tests/golden/README.md + golden-master-test-plan.md §2 (cross-host drift).
    -ffp-contract=off)

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
#
# Target #3: synth math (silent-audio-regression guard). Osc.cpp / Env.cpp /
# Matrix.cpp compile host-clean EXCEPT for three section attributes that are a
# Mach-O hard error (see Target #1 appendix Correction 2); those are gated with
# #ifndef PFM3_HOST in the firmware TUs themselves (Osc.cpp:38, Env.cpp:21,
# Env.cpp:49 — same pattern as Sequencer.cpp). Link deps, all REAL firmware TUs
# (no stubs, per the Hexter precedent of preferring real data):
#   * Osc.cpp's waveTables[] initializer references sinTable/sawTable/... which
#     live in waves.c (plain-C data TU -> needs the C language enabled above).
#   * Env::init() calls checkIsLoop(), whose body indexes algoOpInformation;
#     that table is defined in Common.cpp (pure data). Pulling the real
#     Common.cpp (not a stub) keeps the carrier/modulator routing faithful.
#   * Matrix.cpp has no extern deps beyond its own members.
target_sources(pfm3_tests PRIVATE
    "${PFM3_FW_ROOT}/Src/midi/Sequencer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/sequencer_collaborators_stub.cpp"
    "${PFM3_FW_ROOT}/Src/utils/Hexter.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Presets.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Osc.cpp"
    "${PFM3_FW_ROOT}/Src/synth/waves.c"
    "${PFM3_FW_ROOT}/Src/synth/Env.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Matrix.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Common.cpp"
    # Target #4: MidiDecoder host coverage (decode + routing). The decode
    # state machine (Tier 1) + NRPN assembly (Tier 2) need MidiDecoder.cpp; the
    # routing capstone (Tier 3) needs the REAL Synth graph — Synth/Timbre/
    # Voice/FxBus/Lfo* — plus midipal/ (NoteStack, EventScheduler) and
    # SimpleEffect/ (SimpleComp + SimpleEnvelope) for the Synth's compressor
    # members. SynthState.cpp is deliberately NOT pulled (its closure drags the
    # FMDisplay family + HAL); the SynthState ctor is never called (the fixture
    # memsets+patches), and its out-of-line non-virtual methods that Synth/
    # Timbre/Voice reference are stubbed in midi_decoder_collaborators_stub.cpp.
    # See tests/SEAM.md Target #4 appendix for the GO/NO-GO gate result + the
    # full new seam surface.
    "${PFM3_FW_ROOT}/Src/midi/MidiDecoder.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Synth.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Timbre.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Voice.cpp"
    "${PFM3_FW_ROOT}/Src/synth/FxBus.cpp"
    "${PFM3_FW_ROOT}/Src/synth/Lfo.cpp"
    "${PFM3_FW_ROOT}/Src/synth/LfoOsc.cpp"
    "${PFM3_FW_ROOT}/Src/synth/LfoEnv.cpp"
    "${PFM3_FW_ROOT}/Src/synth/LfoEnv2.cpp"
    "${PFM3_FW_ROOT}/Src/synth/LfoStepSeq.cpp"
    "${PFM3_FW_ROOT}/Src/synth/MixerState.cpp"
    "${PFM3_FW_ROOT}/Src/synth/SynthStateAware.cpp"
    "${PFM3_FW_ROOT}/Src/synth/SynthParamListener.cpp"
    # Synth-graph closure: NoteStack + EventScheduler (midipal/) and SimpleComp
    # + SimpleEnvelope (SimpleEffect/) are real firmware TUs the Synth/Timbre/
    # Voice bodies reference. Pulled for real (not stubbed) so the note-on/off +
    # compressor routing is faithful.
    "${PFM3_FW_ROOT}/Src/midipal/note_stack.cpp"
    "${PFM3_FW_ROOT}/Src/midipal/event_scheduler.cpp"
    "${PFM3_FW_ROOT}/Src/SimpleEffect/SimpleComp.cpp"
    "${PFM3_FW_ROOT}/Src/SimpleEffect/SimpleEnvelope.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/midi_decoder_collaborators_stub.cpp"
    # Golden-master render harness. NOT a *_test.cpp, so the GLOB above does
    # not pick it up — listed explicitly. The render reuses the midi_decoder
    # fixture's memset+patched SynthState and the already-linked Synth graph;
    # no new firmware TUs, shims, or stubs are required. See
    # tests/golden_harness.{h,cpp} + tests/golden_master_test.cpp.
    "${CMAKE_CURRENT_SOURCE_DIR}/golden_harness.cpp"
    # --- Phase 4: filesystem parser tier (spec-test-coverage-phase4.md) ------
    # In-memory FatFs implementation backing the tests/host_shims/fatfs.h shim
    # (see the shim header comment for the fidelity contract), plus the stub
    # for the only hardware symbols the filesystem TUs touch (TftDisplay tft,
    # tftMemory framebuffer, HAL_Delay no-op, and the FIXTURE-DATA midiConfig
    # table with Menu.h struct/count parity — real table lives in the
    # unhostable hardware/Menu.cpp).
    "${CMAKE_CURRENT_SOURCE_DIR}/host_shims/fatfs_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs/tft_display_stub.cpp"
    # The 12 firmware/Src/filesystem TUs. All FatFs calls resolve to the shim;
    # section attributes + the HAL_Delay(1) in SequenceBank.cpp are
    # PFM3_HOST-guarded (zero firmware behavior change; see spec + SEAM.md).
    "${PFM3_FW_ROOT}/Src/filesystem/PreenFMFileType.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/FileSystemUtils.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/Storage.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/ScalaFile.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/ConfigurationFile.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/DX7SysexFile.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/UserWaveform.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/UserEnvCurve.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/MixerBank.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/PatchBank.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/SequenceBank.cpp"
    "${PFM3_FW_ROOT}/Src/filesystem/PPMImage.cpp"
    # --- Phase 5: MIDI controller state + persistence -----------------------
    # Real firmware TUs. MidiControllerState keeps ring-buffer writes intact
    # and gates only the UART register kick; MidiControllerFile reuses the
    # Phase-4 FatFs shim and PreenFMFileType implementation above.
    "${PFM3_FW_ROOT}/Src/MidiController/MidiControllerState.cpp"
    "${PFM3_FW_ROOT}/Src/MidiController/MidiControllerFile.cpp"
    # --- Finding 7.6: TFT algo-drawing tier --------------------------------
    # Pure-C++ drawing TU (no HAL calls): only the stm32h7xx_hal.h include
    # needs the PFM3_HOST seam (lib/Inc/TftAlgo.h resolves to <stdint.h>).
    # highlightOperator's former int32/int16 cast-stores into the uint8_t
    # algo mask buffer were misaligned for middle-column operator cells and
    # HardFaulted on device (strd requires word alignment); the byte-store
    # rewrite is exercised by tests/tft_algo_test.cpp, and the UBSan
    # alignment check (test-asan target) is the regression guard.
    "${PFM3_FW_ROOT}/Src/hardware/TftAlgo.cpp")

# 7.6 regression-guard NOTE: UBSan's alignment check (the test-asan flag set)
# only REPORTS by default (exit 0), and per-test ENVIRONMENT properties can't
# reach gtest_discover_tests results at configure time (discovery is deferred
# to build time). The enforcement therefore lives IN-PROCESS in
# tests/tft_algo_test.cpp: a __ubsan_on_report monitor-hook override aborts
# when a report fires from TftAlgo.cpp, turning a reintroduced misaligned
# cast-store into a hard test failure. Other TUs keep the documented
# reporting-only behavior.
gtest_discover_tests(pfm3_tests)
