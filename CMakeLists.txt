# preenfm3 — CMake build (Arm GNU Toolchain 15.x)
#
# Configure (one of):
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake -DCMAKE_BUILD_TYPE=Debug
# Build:
#   cmake --build build -j
#
# Generator: pass -G Ninja for faster builds; otherwise the default generator
# (Unix Makefiles) is used. Ninja is optional — see doc/BUILDING.md.

cmake_minimum_required(VERSION 3.20)

project(preenfm3
    VERSION 1.02.0
    LANGUAGES C CXX ASM)

# Language standards matching the original GCC-7 managed-build defaults.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)          # gnu11
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)        # gnu++14

# Per-config flags matching the CubeIDE DebugLQFP144 / Release configs.
set(CMAKE_C_FLAGS_DEBUG        "-Og -g3" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG      "-Og -g3" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_DEBUG      "-g3"     CACHE STRING "" FORCE)
# Historically GCC 15 -O3/-Ofast hard-faulted at boot. Root cause (found on
# hardware via SWD, branch fix/o3-and-config-save): unaligned float stores in
# Sequencer state serialization (*(float*)&buf[i] at odd offsets). At -O3 GCC
# vectorizes those into `vstr`, which traps on misalignment on Cortex-M7
# (CFSR.UNALIGNED -> HardFault). Fixed with unaligned-safe memcpy helpers;
# -O3 and -Ofast now boot and run on hardware (verified: SysTick advances,
# CFSR/HFSR clear). -Ofast is the default Release level.
#
# The Release optimization level is exposed as PFM3_RELEASE_OPT, so a lower
# level (-O2, -O3, -O3 -fno-tree-vectorize, ...) can be built without editing
# this file: configure a separate build dir with -DPFM3_RELEASE_OPT=-O2.
set(PFM3_RELEASE_OPT "-Ofast" CACHE STRING "Release optimization level for C/C++ (default -Ofast; -O2/-O3 also supported)")
set(CMAKE_C_FLAGS_RELEASE      "${PFM3_RELEASE_OPT}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE    "${PFM3_RELEASE_OPT}" CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_RELEASE    ""                     CACHE STRING "" FORCE)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug|Release)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

# Reject unsupported build types (e.g. RelWithDebInfo, MinSizeRel, typos) so the
# Debug/Release flag contract can't be silently bypassed.
if(NOT CMAKE_CONFIGURATION_TYPES)
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
        message(FATAL_ERROR "Unsupported CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. Use -DCMAKE_BUILD_TYPE=Debug|Release.")
    endif()
endif()

# DEBUG macro in Debug builds only (matches original firmware/bootloader).
add_compile_definitions("$<$<CONFIG:Debug>:DEBUG>")

# Section splitting so --gc-sections can drop unused code/data.
add_compile_options(-ffunction-sections -fdata-sections)

# NOTE: to build with Ninja, pass -G Ninja at configure time (it is not installed
# by default on this host; Unix Makefiles is used otherwise). See doc/BUILDING.md.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(stm32-post-build)

# Export compile_commands.json for the Arm cross-build so clangd / cppcheck /
# clang-tidy / pi-lens can resolve the firmware include set. The host test build
# exports its own DB (tests/CMakeLists.txt); this one covers firmware +
# bootloader + lib. `make analyze` consumes it (see Makefile).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(lib)
add_subdirectory(firmware)
add_subdirectory(bootloader)
