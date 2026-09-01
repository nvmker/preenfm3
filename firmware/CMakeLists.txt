# preenfm3 firmware — executable linked at FLASH 0x08020000 (after bootloader).

file(GLOB_RECURSE FIRMWARE_C CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Core/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Class/MIDI/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Src/*.c")
file(GLOB_RECURSE FIRMWARE_CXX CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cpp")

add_executable(preenfm3
    ${FIRMWARE_C}
    ${FIRMWARE_CXX}
    "${CMAKE_CURRENT_SOURCE_DIR}/Startup/startup_stm32h753vitx.s")

target_include_directories(preenfm3 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/hardware"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/midi"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/MidiController"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/midipal"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/SimpleEffect"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/synth"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/utils"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32H7xx/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy"
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Core/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Class/MIDI/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/Inc")

target_compile_definitions(preenfm3 PRIVATE USE_HAL_DRIVER STM32H753xx LQFP144)
target_compile_options(preenfm3 PRIVATE -mslow-flash-data)

# Static-analysis baseline warnings on the firmware's own code. -Wall -Wextra
# covers -Wsequence-point (gcc) / -Wunsequenced (clang), -Wimplicit-fallthrough,
# -Wswitch, -Wunused-*, etc. on BOTH compilers — no need to name the
# compiler-specific flags. No -Werror (noise floor unknown). -Wshadow deferred
# (noisy on legacy C++); -Wconversion excluded (implicit-int-promotion noise on
# every byte op — a known embedded trap).
#
# CAVEAT: vendor C (firmware/Drivers/STM32H7xx_HAL_Driver, firmware/Middlewares)
# is compiled into this same target and will emit non-fatal warnings under
# -Wall -Wextra. A follow-up could split vendor TUs into a no-warning OBJECT
# library if the noise dominates. See _bmad-output/static-analysis/spike-findings.md.
target_compile_options(preenfm3 PRIVATE -Wall -Wextra)
# Silence two -Wextra style flags that carry no signal on legacy embedded C++:
# -Wunused-parameter (callback/handler signatures fixed by HAL/MIDI contracts)
# and -Wmissing-field-initializers ({0}-style partial struct init). These two
# are ~521 of the firmware/Src warnings and ~80% of the -Wall -Wextra noise;
# the bug-shaped -Wextra warnings (-Wswitch, -Wsign-compare, -Wstrict-aliasing,
# -Wmaybe-uninitialized, ...) stay visible. Per the static-analysis spike.
target_compile_options(preenfm3 PRIVATE
    -Wno-unused-parameter -Wno-missing-field-initializers)
# 7.6 regression guard (PR #35 follow-up): flag every cast that increases the
# required alignment of the target type — the class that HardFaulted on device
# (TftAlgo::highlightOperator's int32/int16 cast-stores into the uint8_t algo
# mask buffer, fused to strd by gcc 15 — strd never tolerates misalignment on
# ARMv7-M). Non-fatal: a new project-code -Wcast-align warning is triage-
# before-merge material, not a build breaker. lib/Src (preenfm3lib) carries
# the same flag with two vendor files suppressed — baseline documented in
# doc/STATIC-ANALYSIS.md § GCC -Wcast-align.
target_compile_options(preenfm3 PRIVATE -Wcast-align)
# Vendor C trees compiled into this same target (STM32 HAL + ST USB
# middleware) carry a 34-warning cast-align baseline (hal_spi x26, hal_uart
# x6, ll_usb x2 — register/DMA overlay idioms, ST-maintained). Suppress the
# flag on exactly those files so the only visible -Wcast-align warnings are
# PROJECT code. Current 10-warning project baseline (bank-serialization
# struct-overlay idiom) is documented in doc/STATIC-ANALYSIS.md.
set(PFM3_VENDOR_C ${FIRMWARE_C})
list(FILTER PFM3_VENDOR_C INCLUDE REGEX "/(Drivers|Middlewares)/")
set_source_files_properties(${PFM3_VENDOR_C} PROPERTIES
    COMPILE_OPTIONS "-Wno-cast-align")

target_link_libraries(preenfm3 PRIVATE preenfm3lib)

set_target_properties(preenfm3 PROPERTIES OUTPUT_NAME preenfm3 SUFFIX ".elf")
set_property(TARGET preenfm3 APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/preenfm3_2MB.ld")
target_link_options(preenfm3 PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/preenfm3_2MB.ld
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/preenfm3.map
    --specs=nosys.specs
    --specs=nano.specs)

pfm3_add_flash_artifacts(preenfm3 "${CMAKE_CURRENT_BINARY_DIR}/preenfm3.elf")
