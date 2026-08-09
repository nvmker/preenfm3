# preenfm3 (nvmker fork)

Sources for the preenfm3 bootloader and firmware — an open-source FM
synthesizer by Xavier ([Ixox/preenfm3](https://github.com/Ixox/preenfm3)),
running on an STM32H7.

This is an active development fork maintained at
[nvmker/preenfm3](https://github.com/nvmker/preenfm3).

> Hardware docs, quick-start guide, and the user wiki still live with the
> original project:
>
> - Quick start (PDF): [`doc/pfm3_qs.pdf`](doc/pfm3_qs.pdf)
> - Wiki: <https://github.com/Ixox/preenfm3/wiki>
> - Forum: <http://ixox.fr/forum/index.php?board=8.0>

---

## Why this fork exists

I created this fork to be able to freely implement the features I have in mind.
The upstream project appears abandoned, and its build is locked to a very old,
hard-to-maintain toolchain. Rather than fight that, this fork modernizes the
build and toolchain, and moves forward independently.

**There are no plans to push these changes back upstream** — this fork is its
own line of development. I intend to keep developing the firmware here.

### What changed from upstream

- **Dropped STM32CubeIDE.** CubeIDE (especially the version the firmware was
  developed against, built on GCC 7) is outdated and a poor platform to
  build on. It has been removed entirely.
- **New build system: CMake + Arm GNU Toolchain 15.** The firmware, bootloader,
  and shared library now build with CMake and `arm-none-eabi-gcc` 15.x
  (CI pins `15.2.rel1`). No IDE required — configure and build from the CLI, or
  via the convenience `Makefile` wrapper.
- **CI/CD via GitHub Actions.** Added workflows for build verification and
  automated releases (see [CI & releases](#ci--releases) below).
- **`-O3`/`-Ofast` boot hard-fault fixed.** The original code miscompiled at
  high optimization with the new Arm GNU Toolchain 15 (unaligned float stores in the Sequencer state
  serialization, vectorized into trapping `vstr` on Cortex-M7). This was fixed
  with unaligned-safe helpers, so `-Ofast` now boots and runs on hardware.

Current firmware version: v1.10 (see [`firmware/Inc/version.h`](firmware/Inc/version.h)).

---

## Building

Full instructions live in [`doc/BUILDING.md`](doc/BUILDING.md). Quick version:

**Prerequisites:** Arm GNU Toolchain 15.x, CMake ≥ 3.20, and optionally
Ninja for faster builds.

```sh
# Configure + build (Release, -Ofast by default)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Or use the Makefile wrapper (clean Release rebuild of firmware + bootloader):
make            # clean Release rebuild of firmware + bootloader
make debug      # clean Debug rebuild (-Og -g3 -DDEBUG)
make firmware   # just the firmware
make bootloader # just the bootloader
make lib        # just the static archive (build/release/lib/libpreenfm3lib.a)
make test       # host-side GoogleTest unit tests (host compiler) -> build/test/
make analyze    # cppcheck + clang-tidy over the firmware DB -> build/release/analyze-*.txt
make clean      # wipe the entire build/ tree
```

Artifacts land in `build/release/`:

- Firmware → `build/release/firmware/preenfm3.{bin,hex,elf,map}`
- Bootloader → `build/release/bootloader/bootloader.{bin,hex,elf,map}`

**Flashing** — the `Makefile` wraps both common paths:

```sh
make flash             # DFU (dfu-util, USB)            -> firmware
make program           # OpenOCD (debug probe / ST-LINK) -> firmware
make flash-bootloader  # DFU the bootloader
```

See [`doc/BUILDING.md`](doc/BUILDING.md) for memory layout, DFU entry, and
probe wiring.

### Tests & static analysis

```sh
make test       # host-side GoogleTest unit tests (host compiler, not arm-none-eabi)
make analyze    # cppcheck + clang-tidy over the firmware cross-build DB
```

`make analyze` runs **cppcheck** (primary) and **clang-tidy** (secondary) over
`build/release/compile_commands.json`, writing
`build/release/analyze-{cppcheck,clang-tidy}.txt`. Findings are advisory —
**not a CI gate**. Config lives in [`.clang-tidy`](.clang-tidy) and
[`scripts/cppcheck-suppressions.txt`](scripts/cppcheck-suppressions.txt);
clang-tidy's cross-compile include resolution is in
[`scripts/analyze-tidy.sh`](scripts/analyze-tidy.sh). Override the binaries with
`make analyze CPPCHECK=... CLANG_TIDY=...`, or analyze a different build with
`make BUILD_DIR=build/o2 analyze`.

See [`doc/BUILDING.md`](doc/BUILDING.md) → *Static analysis* for the full
target reference.

---

## CI & releases

Three GitHub Actions workflows under [`.github/workflows/`](.github/workflows):

| Workflow | Trigger | Purpose |
| ---------- | --------- | --------- |
| [`ci.yml`](.github/workflows/ci.yml) | every push / pull request | Cross-compile + link check (no hardware in the loop) |
| [`build.yml`](.github/workflows/build.yml) | reusable | Installs Arm GNU Toolchain 15.x (cached), builds Release, parses versions, uploads `.bin`/`.hex` |
| [`release.yml`](.github/workflows/release.yml) | `v*` tag push *or* manual dispatch | Builds, packages `pfm3_firmware_<ver>.zip`, publishes a GitHub Release with auto-generated notes |

Releases are published on this fork's
[Releases page](https://github.com/nvmker/preenfm3/releases). A release run
asserts the tag matches `version.h` before building, so a mismatch fails fast.

---

## Roadmap

This fork is under development. The immediate direction is continued
firmware work on top of the modernized build — new features and fixes will land
here. Upstream may be tracked for backports if the original project comes back to life.

---

## A note on how this is developed (LLM disclosure)

I heavily use LLMs and coding agents to develop this fork. To be clear
about what that means in practice: the agents work under my
supervision. I do not merge generated code blindly — I read the code that
gets produced, and I test the implemented features (including on real
hardware) before it ships. The intent is to move faster with agents assistance
while keeping a human accountable for what goes in.

— nvmker
