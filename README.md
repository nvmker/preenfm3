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

- **Modernized build.** Dropped STM32CubeIDE; the firmware, bootloader, and
  shared library now build with CMake + `arm-none-eabi-gcc` 15.x from the CLI
  (or the `Makefile` wrapper). No IDE required.
- **CI/CD via GitHub Actions.** Build verification, automated releases, and a
  host test / coverage / static-analysis pipeline on every push and pull
  request (see [CI & releases](#ci--releases)).
- **Host-side test suite + full-render golden master.** The real firmware synth
  graph (`Synth → Timbre → Voice → Matrix → FxBus → Osc/Env`, no mocks) runs
  under a `PFM3_HOST` seam in GoogleTest — per-unit goldens plus full-render
  fixtures locking `Synth::buildNewSampleBlock` end-to-end. Upstream ships no
  test harness; this fork covers the whole render path as a regression net. See
  [`tests/README.md`](tests/README.md) + [`tests/golden/README.md`](tests/golden/README.md).

**Features added:**

- **DX7 import folder picker.** When loading DX7 patches you can now browse and
  select a folder (bank directory) on the SD card, then pick the bank and preset
  within it — previously the loader scanned a single fixed directory. The bank
  and preset cursor persist across navigation and reboot.

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

The host test build runs the **real firmware synth graph** — `Synth → Timbre →
Voice → Matrix → FxBus → Osc/Env`, the actual firmware translation units — on the
host compiler behind a `PFM3_HOST` seam. No mocks, no hardware in the loop. See
[`tests/SEAM.md`](tests/SEAM.md) for how the firmware TUs are pulled in, and
[`tests/README.md`](tests/README.md) for the test roadmap.

```sh
make test         # host GoogleTest suite (host compiler, not arm-none-eabi)
make test-cov     # + llvm-cov coverage report on firmware/Src (floor-gated)
make test-asan    # + ASAN/UBSAN over the render path (verifies no UB)
make golden-regen # regenerate the full-render golden fixtures (deliberate; see below)
make analyze      # cppcheck + clang-tidy over the firmware cross-build DB
```

**Two tiers of audio-path coverage:**

- **Per-unit goldens** — `Osc::getNextBlock`, `Env` ADSR traces, DX7-import
  snapshots, Matrix routing, note-stack allocation, etc. Lock individual DSP /
  param units (`synth_math_test.cpp`, `hexter_test.cpp`, `note_stack_test.cpp`,
  `sequencer_test.cpp`, …).
- **Full-render golden master** — snapshots the complete 6-output mix of
  `Synth::buildNewSampleBlock` across scripted note / parameter sequences,
  locking the entire render chain end-to-end as a regression guard. The
  committed fixtures cover static sustains, full envelopes, the FM algorithms,
  multi-timbre mixes, live mid-render parameter changes (the CC-routing path),
  and the time-advanced paths (arpeggiator note-cycling + sequencer external-
  MIDI-clock playback). A mismatch is a signal to investigate what changed in
  the render path, not an automatic "fix." See
  [`tests/golden/README.md`](tests/golden/README.md) for the fixture catalog,
  comparison model, and design rationale.

The full render is **libm-specific** — `Osc::init`/`Env::init` precompute their
tables via `sinf`/`expf`/`logf`, and the preenfm oscillators' FM feedback
amplifies the ~1-ULP differences between platform libms (macOS libsystem vs
Linux glibc) into large render divergence within a block. So each platform
commits its **own** fixture (selected at test time by `__APPLE__`); the compare
gate is ±1 audio-LSB (±256 stored units). The
[`regenerate-linux-goldens`](.github/workflows/regenerate-linux-goldens.yml)
workflow regenerates the Linux (glibc) fixtures on dispatch when a render change
legitimately alters them.

`make analyze` runs **cppcheck** (primary) and **clang-tidy** (secondary) over
`build/release/compile_commands.json` →
`build/release/analyze-{cppcheck,clang-tidy}.txt`. Findings are advisory —
**not a CI gate**. Config in [`.clang-tidy`](.clang-tidy) +
[`scripts/cppcheck-suppressions.txt`](scripts/cppcheck-suppressions.txt);
override with `make analyze CPPCHECK=... CLANG_TIDY=...`, or analyze a different
build with `make BUILD_DIR=build/o2 analyze`. See [`doc/BUILDING.md`](doc/BUILDING.md)
→ *Static analysis* for the full target reference.

---

## CI & releases

GitHub Actions workflows under [`.github/workflows/`](.github/workflows).
[`ci.yml`](.github/workflows/ci.yml) is the orchestrator (every push / pull
request) and calls the reusable build / test / coverage / static-analysis
workflows:

| Workflow | Trigger | Purpose |
| ---------- | --------- | --------- |
| [`ci.yml`](.github/workflows/ci.yml) | every push / pull request | Orchestrator: calls build + tests + coverage + static-analysis |
| [`build.yml`](.github/workflows/build.yml) | reusable | Installs Arm GNU Toolchain 15.x (cached), builds Release, uploads `.bin`/`.hex` |
| [`tests.yml`](.github/workflows/tests.yml) | reusable | Host GoogleTest suite (ubuntu gcc) — incl. the full-render golden master |
| [`coverage.yml`](.github/workflows/coverage.yml) | reusable | llvm-cov coverage report on `firmware/Src` (floor gate) |
| [`static-analysis.yml`](.github/workflows/static-analysis.yml) | reusable | cppcheck (baseline gate) + clang-tidy |
| [`regenerate-linux-goldens.yml`](.github/workflows/regenerate-linux-goldens.yml) | manual dispatch | Regenerates the Linux (glibc) golden fixtures + self-verifies + commits |
| [`release.yml`](.github/workflows/release.yml) | `v*` tag push *or* manual dispatch | Builds, packages `pfm3_firmware_<ver>.zip`, publishes a GitHub Release |

Releases are published on this fork's
[Releases page](https://github.com/nvmker/preenfm3/releases). A release run
asserts the tag matches `version.h` before building, so a mismatch fails fast.

---

## Roadmap

This fork is under active development. Current focus areas:

- **Firmware features + fixes** on top of the modernized CMake / Arm GNU 15 build.
- **Test-infrastructure depth** — the host GoogleTest seam now covers the full
  synth render graph, with per-unit goldens and a full-render golden-master tier
  in place. The remaining gap is HIL coverage for the `-Ofast`-only paths the
  host build can't reach.

Upstream may be tracked for backports if the original project comes back to life.

---

## A note on how this is developed (LLM disclosure)

I heavily use LLMs and coding agents to develop this fork. To be clear
about what that means in practice: the agents work under my
supervision. I do not merge generated code blindly — I read the code that
gets produced, and I test the implemented features (including on real
hardware) before it ships. The intent is to move faster with agents assistance
while keeping a human accountable for what goes in.

— nvmker
