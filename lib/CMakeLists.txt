# preenfm3lib — static library consumed by firmware and bootloader.
#
# Contains: FatFs, SD/BSP (Adafruit + Nucleo bus), display/input (ili9341,
# fonts, TftDisplay, Encoders, RingBuffer), the FatFs OS hook (syscall.c),
# and the lib public API (preenfm3lib.c: HC165 pin defs, GPIO/SD init,
# TFT backlight).
#
# Excluded (the consumers supply their own copies; compiling them here would
# create hard multiple-definition link errors under -fno-common):
#   syscalls.c, sysmem.c                       (newlib stubs / _sbrk)
#   Drivers/CMSIS/.../system_stm32h7xx_singlecore.c  (SystemInit/SystemCoreClock)

add_library(preenfm3lib STATIC
    # FatFs
    Src/ff.c
    Src/diskio.c
    Src/ff_gen_drv.c
    Src/fatfs.c
    Src/sd_diskio.c
    # SD / board support
    Src/adafruit_802_sd.c
    Src/stm32h7xx_nucleo_bus.c
    Src/syscall.c
    # Display + input
    Src/ili9341.c
    Src/fonts.c
    Src/TftDisplay.cpp
    Src/Encoders.cpp
    Src/RingBuffer.cpp
    # Public API: HC165 pin definitions, GPIO/SD init, TFT backlight
    Src/preenfm3lib.c)

target_include_directories(preenfm3lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32H7xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy)

# Normalized to the real silicon (H753). CubeMX generated lib against H743;
# H753 is a superset (H743 + crypto) and the consuming projects use H753.
target_compile_definitions(preenfm3lib PRIVATE
    USE_HAL_DRIVER STM32H753xx LQFP144)

# 7.6 regression guard (PR #35 follow-up): same -Wcast-align as the preenfm3
# target — flag any cast that increases required alignment (the class that
# HardFaulted on device via gcc-15-fused strd). The vendored SD stack below
# carries a 5-warning baseline (sd_diskio.c x4, adafruit_802_sd.c x1 — FatFs
# BYTE* -> uint32_t* for SDMMC DMA; contract requires aligned buffers, every
# firmware caller passes the aligned static storageBuffer). Suppressed on
# exactly those two files so any remaining -Wcast-align warning is NEW code.
# Baseline table: doc/STATIC-ANALYSIS.md § GCC -Wcast-align.
target_compile_options(preenfm3lib PRIVATE -Wcast-align)
set_source_files_properties(Src/sd_diskio.c Src/adafruit_802_sd.c PROPERTIES
    COMPILE_OPTIONS "-Wno-cast-align")

set_target_properties(preenfm3lib PROPERTIES OUTPUT_NAME preenfm3lib)
