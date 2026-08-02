# CMake toolchain file for preenfm3 (STM32H753, Cortex-M7).
# Targets Arm GNU Toolchain 15.x (arm-none-eabi-*).
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake
#
# The toolchain is resolved from PATH by default. If your install is not on PATH,
# point at its bin dir explicitly:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
#         -DPFM3_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m7)

# Target-triple prefix (override only if you renamed the binaries).
if(NOT DEFINED PFM3_TOOLCHAIN_PREFIX)
    set(PFM3_TOOLCHAIN_PREFIX "arm-none-eabi-")
endif()

# Locate every binary we need. When PFM3_TOOLCHAIN_PATH is set, require the
# tool to live there (NO_DEFAULT_PATH) so we never silently mix installations;
# otherwise resolve from PATH (HINTS is empty and harmless).
function(_pfm3_find_program var name)
    if(DEFINED PFM3_TOOLCHAIN_PATH)
        find_program(${var}
            NAMES "${PFM3_TOOLCHAIN_PREFIX}${name}"
            PATHS "${PFM3_TOOLCHAIN_PATH}"
            NO_DEFAULT_PATH)
    else()
        find_program(${var}
            NAMES "${PFM3_TOOLCHAIN_PREFIX}${name}"
            DOC "Path to the ${PFM3_TOOLCHAIN_PREFIX}${name} binary")
    endif()
    if(NOT ${var})
        message(FATAL_ERROR "Could not find ${PFM3_TOOLCHAIN_PREFIX}${name}. "
            "Set -DPFM3_TOOLCHAIN_PATH=<bin-dir> or add the toolchain to PATH.")
    endif()
endfunction()

_pfm3_find_program(CMAKE_C_COMPILER   gcc)
_pfm3_find_program(CMAKE_CXX_COMPILER g++)
_pfm3_find_program(CMAKE_ASM_COMPILER gcc)
_pfm3_find_program(CMAKE_OBJCOPY      objcopy)
_pfm3_find_program(CMAKE_OBJDUMP      objdump)
_pfm3_find_program(CMAKE_SIZE_UTIL    size)
_pfm3_find_program(CMAKE_AR           ar)
_pfm3_find_program(CMAKE_RANLIB       ranlib)

# Validate the toolchain generation once. This build is tuned for Arm GNU 15.x
# (GCC-7-compat -Wno-error flags, the -Ofast/-ffast-math assumption, newlib 15).
if(NOT _PFM3_GCC_CHECKED)
    set(_PFM3_GCC_CHECKED TRUE CACHE INTERNAL "GCC version checked")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -dumpversion
        OUTPUT_VARIABLE _pfm3_gcc_ver
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _pfm3_gcc_ver MATCHES "^15\.")
        message(WARNING "arm-none-eabi-gcc is ${_pfm3_gcc_ver}; this build is tuned "
            "for Arm GNU Toolchain 15.x. Proceeding, but GCC-7-compat assumptions "
            "may not apply.")
    endif()
endif()

# Propagate override vars into try_compile so ABI checks use the same toolchain.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PFM3_TOOLCHAIN_PATH PFM3_TOOLCHAIN_PREFIX)

# CMake's size var is named CMAKE_SIZE in some templates; expose both for safety.
set(CMAKE_SIZE "${CMAKE_SIZE_UTIL}")

# Bare-metal target: never try to run binaries during configure/try_compile.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Keep find_* from drifting into host toolchains.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# STM32H753: Cortex-M7 + double-precision FPU (fpv5-d16), hard ABI.
# Injected into every C / C++ / ASM compile and the final link.
set(PFM3_MCU_FLAGS "-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard")

# GCC 14+ promoted several legacy-C diagnostics from warning to error
# (implicit declarations, implicit int, int conversion, pointer-type
# mismatches, return-without-value). The original code built cleanly under
# GCC 7 where these were warnings, so restore that behavior rather than
# surgically patching legacy vendor-derived C (e.g. adafruit_802_sd.c
# references an unimplemented BSP_SPI_SendRecv_DMA; main.c has unreachable
# `return;` after bootJumpToApplication). C++ keeps its stricter defaults.
# NOTE: caddr_t is NOT defined here via -D (the `*` glob-expands on the
# command line); sysmem.c provides a local typedef instead.
set(PFM3_C_COMPAT_FLAGS
    "-Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=return-mismatch")

set(CMAKE_C_FLAGS_INIT            "${PFM3_MCU_FLAGS} ${PFM3_C_COMPAT_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT          "${PFM3_MCU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT          "${PFM3_MCU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT   "${PFM3_MCU_FLAGS} -Wl,--no-warn-rwx-segments")
