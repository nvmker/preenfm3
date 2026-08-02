# preenfm3 bootloader — executable linked at FLASH 0x08000000 (128 KiB).
# STM32H753VITX_FLASH.ld is the active script in both configs; the *_RAM.ld
# script in this folder is a legacy manual-debug alternative and is not wired.

file(GLOB_RECURSE BOOTLOADER_C CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32_USB_Device_Library/Core/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32_USB_Device_Library/Class/MSC/Src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/USB_DEVICE/App/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/USB_DEVICE/Target/*.c")
file(GLOB_RECURSE BOOTLOADER_CXX CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Src/*.cpp")

add_executable(bootloader
    ${BOOTLOADER_C}
    ${BOOTLOADER_CXX}
    "${CMAKE_CURRENT_SOURCE_DIR}/Startup/startup_stm32h753vitx.s")

target_include_directories(bootloader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Src"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32H7xx/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32_USB_Device_Library/Core/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32_USB_Device_Library/Class/MSC/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/USB_DEVICE/App"
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/USB_DEVICE/Target"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/Inc")

target_compile_definitions(bootloader PRIVATE USE_HAL_DRIVER STM32H753xx LQFP144)

target_link_libraries(bootloader PRIVATE preenfm3lib)

set_target_properties(bootloader PROPERTIES OUTPUT_NAME bootloader SUFFIX ".elf")
set_property(TARGET bootloader APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/STM32H753VITX_FLASH.ld")
target_link_options(bootloader PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/STM32H753VITX_FLASH.ld
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/bootloader.map
    --specs=nosys.specs
    --specs=nano.specs)

pfm3_add_flash_artifacts(bootloader "${CMAKE_CURRENT_BINARY_DIR}/bootloader.elf")
