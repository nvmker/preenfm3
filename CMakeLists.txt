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
# GCC 15 -Ofast / -O3 miscompile this firmware (verified on hardware: both
# hang at boot — GCC 15's -O3 vectorization/inlining trips latent UB in the
# hand-tuned synth DSP; GCC 7 ran -Ofast fine). -O2 is the verified-working
# level (boots + runs). To recover more perf later, bisect -O3 flags
# (e.g. -O3 -fno-tree-vectorize).
set(CMAKE_C_FLAGS_RELEASE      "-O2"  CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE    "-O2"  CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS_RELEASE    ""        CACHE STRING "" FORCE)

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

add_subdirectory(lib)
add_subdirectory(firmware)
add_subdirectory(bootloader)
