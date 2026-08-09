# Building preenfm3 (CMake + Arm GNU Toolchain 15)

The firmware, bootloader, and shared library build with **CMake** and the
**Arm GNU Toolchain 15.x** (`arm-none-eabi-*`). STM32CubeIDE is no longer
required.

## Prerequisites

| Tool          | Minimum  | Notes |
|---------------|----------|-------|
| Arm GNU Toolchain | 15.x | `arm-none-eabi-gcc` on `PATH` (verify: `arm-none-eabi-gcc --version`) |
| CMake         | 3.20     | |
| Ninja *(optional)* | 1.10 | Faster builds: `brew install ninja`. Without it, Unix Makefiles is used. |

If the toolchain is not on `PATH`, point at its `bin` directory:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake \
      -DPFM3_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin
```

## Configure & build

```sh
# Release (default; -Ofast). To use a lower optimization level instead, pass
# -DPFM3_RELEASE_OPT=-O2 (or -O3) at configure time:
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake -DCMAKE_BUILD_TYPE=Release \
      -DPFM3_RELEASE_OPT=-O2
cmake --build build -j            # add -G Ninja at configure time for faster builds

# Debug (-Og -g3 -DDEBUG)
cmake -B build-debug -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
```

## Artifacts

| Target       | ELF | BIN (flash) | HEX | Map |
|--------------|-----|-------------|-----|-----|
| firmware     | `build/firmware/preenfm3.elf`     | `build/firmware/preenfm3.bin`     | `…/preenfm3.hex`     | `…/preenfm3.map` |
| bootloader   | `build/bootloader/bootloader.elf` | `build/bootloader/bootloader.bin` | `…/bootloader.hex`   | `…/bootloader.map` |
| lib          | `build/lib/libpreenfm3lib.a`      | — | — | — |

The build prints an `arm-none-eabi-size` report per target after linking.

## Memory layout (unchanged)

- **Bootloader**: FLASH `0x08000000`, 128 KiB, `_estack = 0x24080000` (end of RAM_D1).
- **Firmware**: FLASH `0x08020000`, 1920 KiB (after the bootloader), `_estack = 0x20020000` (end of DTCMRAM).
- Boot flow: bootloader → `bootJumpToApplication(0x08020000)` → firmware.

## Flashing

The build produces **flash-only `.bin` files** (the `.elf` also carries RAM/ITCM
`LOAD` segments that some programmers would write to RAM, masking cold-boot init —
so prefer the `.bin`). There is no `cmake --target flash`; pick one of:

Flash regions:

| Image | Address |
| --- | --- |
| Bootloader | `0x08000000` (128 KiB, bank 1) |
| Firmware | `0x08020000` (after the bootloader) |

### A. ST-LINK (OpenOCD)

Bring your own OpenOCD config (the old `.launch`/`.cfg` files are gone). Bootloader
first, then firmware:

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "program build/bootloader/bootloader.bin 0x08000000 verify reset exit"
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "program build/firmware/preenfm3.bin 0x08020000 verify reset exit"
```

### B. DFU (dfu-util, no ST-LINK required)

Enter the STM32H7 ROM DFU bootloader: set **BOOT0** (the board has a BOOT0
header — `hardware/lib/pfm_lib.pretty/Boot0_*`), power on, then:

```sh
dfu-util -a 0 -s 0x08000000:leave -D build/bootloader/bootloader.bin   # bootloader
dfu-util -a 0 -s 0x08020000:leave -D build/firmware/preenfm3.bin        # firmware
```

(`:leave` jumps to the new code after writing.) The preenfm3 bootloader can
also hand off to ROM DFU from software: in the bootloader menu, **Button 2** =
jump to bootloader (DFU) — useful once the bootloader is installed.

### C. preenfm3 bootloader (SD card, no host tools required)

The standard field-update path once the bootloader is installed:

1. Copy `preenfm3.bin` to the **root of the SD card** (the bootloader scans `0:/`
   for any `*.bin`). To load it over USB: hold **MENU** at power-on → bootloader
   menu → **Button 3** mounts the SD card as a USB drive; copy the file, then
   **MENU** to exit the SD view. (Or put the SD card in a card reader.)
2. Hold **MENU** at power-on to enter the bootloader menu.
3. Select the firmware with the encoder / buttons 7–8.
4. **Button 1** flashes the selected file into `0x08020000`, then **Button 4**
   reboots into it.

This updates **firmware only**; to change the bootloader, use method A or B.

## Tests (host-side unit tests)

Host-side C++ unit tests live under [`tests/`](../tests), built with
**GoogleTest** and run with **CTest**. They compile with the **host compiler**
(`g++`/`clang++`), not `arm-none-eabi-gcc` — so `tests/` is a standalone CMake
project you configure *without* `-DCMAKE_TOOLCHAIN_FILE`. (The toolchain file
applies to the whole CMake tree; it can't co-exist with a host-built test
target, so tests/ is not `add_subdirectory`-ed into the firmware build.)

```sh
make test          # configure + build + run, into build/test/

# or raw CMake:
cmake -B build/test -S tests
cmake --build build/test -j
ctest --test-dir build/test --output-on-failure
```

See [`tests/README.md`](../tests/README.md) for the scope (currently
scaffolding + a smoke test), conventions, and the coverage roadmap.

## Static analysis (cppcheck + clang-tidy)

`make analyze` runs **cppcheck** (primary) and **clang-tidy** (secondary) over
the firmware cross-build `compile_commands.json`, writing
`build/release/analyze-{cppcheck,clang-tidy}.txt`. It builds the firmware first
(depends on the `firmware` target), so it works on a fresh clone.

```sh
make analyze                                # build firmware, then run both analyzers
make analyze CPPCHECK=/opt/homebrew/bin/cppcheck CLANG_TIDY=/opt/homebrew/opt/llvm/bin/clang-tidy
make BUILD_DIR=build/o2 analyze             # analyze a different build's DB
```

| Tool | Role | Config |
| --- | --- | --- |
| **cppcheck** (primary) | reads the arm-none-eabi-gcc DB natively; cleanest cross-compile story | [`scripts/cppcheck-suppressions.txt`](../scripts/cppcheck-suppressions.txt) |
| **clang-tidy** (secondary) | `bugprone-*` / `cert-*` checks; cross-compile include resolution is handled by the wrapper | [`.clang-tidy`](../.clang-tidy), [`scripts/analyze-tidy.sh`](../scripts/analyze-tidy.sh) |

The two tools cover **different** scopes:

- **cppcheck** — the full cross-build `compile_commands.json` (161 TUs: firmware + lib + bootloader) **minus** `firmware/Drivers` and `firmware/Middlewares` (vendor HAL/Middleware), excluded via the Makefile's `-i` flags. So cppcheck *does* analyze `bootloader/` (Src + Drivers) — that's why the (now-suppressed) CMSIS finding included the bootloader copy.
- **clang-tidy** — `firmware/Src` + `lib/Src` only, filtered by `scripts/analyze-tidy.sh` (vendor + bootloader excluded).

`tests/` is a separate host-built CMake project and isn't in the cross-build DB.
Findings are **advisory — not a CI gate**; `make analyze` never fails the build
on findings, only on setup errors (missing binary, unreadable DB).

## Project structure

```
CMakeLists.txt                      top-level project (configs, standards)
cmake/arm-none-eabi-gcc.cmake       cross-compilation toolchain (STM32H753, Cortex-M7, fpv5-d16, hard-ABI)
cmake/stm32-post-build.cmake        .bin/.hex + size helper
lib/CMakeLists.txt                  preenfm3lib (static archive: FatFs, display, input, SD/BSP)
firmware/CMakeLists.txt             preenfm3 executable (links preenfm3lib; preenfm3_2MB.ld)
bootloader/CMakeLists.txt           bootloader executable (links preenfm3lib; STM32H753VITX_FLASH.ld)
tests/CMakeLists.txt                host-side GoogleTest unit tests (standalone project, host compiler)
```
