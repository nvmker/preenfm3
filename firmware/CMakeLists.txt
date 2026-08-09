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
