# Shared helpers for STM32 targets.
# Generates a flash-only .bin + .hex from a target's .elf and prints a size report.
# Concrete paths (no generator expressions) — gen-exprs in POST_BUILD errored on
# CMake 4.4, and we target single-config generators (Make / Ninja single-config).
# Multi-config (Ninja Multi-Config, Xcode) is NOT supported by this helper.

function(pfm3_add_flash_artifacts target elf)
    get_filename_component(_dir "${elf}" DIRECTORY)
    set(_bin "${_dir}/${target}.bin")
    set(_hex "${_dir}/${target}.hex")
    # .ram_d* / .instruction_ram are RAM-resident sections declared without
    # (NOLOAD), so they carry LOAD with LMA=VMA in RAM (0x24000000 / 0x30000000 /
    # 0x38000000 / 0x00000004). Because LMA==VMA there is no flash copy in
    # startup, so they are runtime-initialized and must NOT inflate the binary
    # image (objcopy -O binary otherwise pads across the full LMA span).
    #
    # Verified safe to strip: every variable in these sections is an
    # UNINITIALIZED working buffer (e.g. FxBus delay1/2/3/4Buffer, imagePPM,
    # lineBuffer/storageBuffer, dx7BankAlloc/scalaScaleFileAlloc/mixerFile,
    # SeqMidiAction/stepNotes, scalaFrequency) — none has an initializer, and
    # startup_stm32h753vitx.s has NO copy loop for them (only the standard
    # .data _sidata copy). So no flash content is lost.
    # -R silently ignores sections that are absent (e.g. bootloader has no .ram_d3).
    set(_ram_exclude -R .ram_d1 -R .ram_d2 -R .ram_d2b -R .ram_d3 -R .instruction_ram)
    # POST_BUILD: runs on every link, so `cmake --build build --target <name>`
    # produces the .bin/.hex directly (the expected UX for an embedded target).
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_SIZE_UTIL} "${elf}"
        COMMAND ${CMAKE_OBJCOPY} -O binary ${_ram_exclude} "${elf}" "${_bin}"
        COMMAND ${CMAKE_OBJCOPY} -O ihex   ${_ram_exclude} "${elf}" "${_hex}"
        BYPRODUCTS "${_bin}" "${_hex}"
        COMMENT "Generating ${target}.bin / ${target}.hex + size report")
endfunction()
