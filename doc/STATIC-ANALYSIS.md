# Static analysis (cppcheck + clang-tidy + GCC warnings)

This repo runs three complementary static analyzers over the firmware. The
**cppcheck** job is a CI gate (fails on new findings); **clang-tidy** and the
**GCC `-Wall -Wextra`** warnings are reporting-only triage tools.

## Scope

The analyzers cover the code we own: **`firmware/Src`** + **`lib/Src`**.
Excluded: `firmware/Drivers` (ST HAL), `firmware/Middlewares` (FreeRTOS / FatFs /
USB), `bootloader` (a separate DFU stub), and `tests/`.

The compile database (`build/release/compile_commands.json`, 161 TUs) covers the
whole project; cppcheck and clang-tidy filter to the in-scope dirs at runtime
(`-ifirmware/Drivers -ifirmware/Middlewares -ibootloader` for cppcheck).

## Running the analyzers

### cppcheck (CI gate) — `make analyze`

```sh
make analyze          # configures + builds the DB, runs cppcheck + clang-tidy,
                      # writes build/release/analyze-{cppcheck,clang-tidy}.txt
```

`make analyze` is the **tolerant** triage tool — it never fails on findings. The
**CI gate** (`.github/workflows/static-analysis.yml`) runs the same cppcheck
flags plus `-ibootloader` and fails only on findings **new vs the baseline**.

### The cppcheck baseline

`scripts/cppcheck-baseline.txt` is the checked-in noise floor (194 findings as of
check-in). CI (`scripts/ci/static-analysis.sh`) `comm`-diffs the current run
against it:

- **new** finding (in current, not in baseline) -> CI **fails**
- **resolved** finding (in baseline, not in current) -> CI passes, prints a hint
  to shrink the baseline

Regenerate the baseline after fixing findings or moving code:

```sh
make firmware
scripts/ci/static-analysis.sh build/release/compile_commands.json /tmp scripts/cppcheck-baseline.txt
cp /tmp/cppcheck-current.txt scripts/cppcheck-baseline.txt   # if RESOLVED reported
```

The baseline is pinned to **cppcheck 2.21.0** (source-built in CI); regenerate
when bumping the version.

### clang-tidy (secondary)

`scripts/analyze-tidy.sh` handles clang-tidy's cross-compile include resolution
(injects the Arm toolchain's system dirs, overrides `uint32_t` typedefs to match
GCC-ARM, strips `-mslow-flash-data`). Currently emits ~274 findings across 79
in-scope TUs, 0 errors. Not a CI gate.

## GCC `-Wall -Wextra`

Enabled on the `preenfm3` target (`firmware/CMakeLists.txt`).
`-Wno-unused-parameter` + `-Wno-missing-field-initializers` silence the two
no-signal style flags (~521 of the firmware/Src warnings); the bug-shaped
warnings stay visible. No `-Werror` (the noise floor is the baseline file, not
the compiler).

## GCC `-Wcast-align` (7.6 regression guard)

Enabled on BOTH `preenfm3` and `preenfm3lib` (`firmware/CMakeLists.txt` +
`lib/CMakeLists.txt`, 2026-09-01, PR #35 follow-up). This is the compiler's
check for the finding-7.6 bug class — wide-type pointer casts
(`*(uint32_t*)&byteBuf[runtimeOffset]`) whose target alignment exceeds the
source: latent UB that HardFaulted on device once gcc 15 fused a loop of
misaligned stores into `strd` (which, unlike plain `str`, never tolerates
misalignment on ARMv7-M). The warning flags both store and load forms and is
compile-time-only — no runtime cost, works on the cross-build where the bug
lives. Not `-Werror`; a NEW project-code `-Wcast-align` warning should be
triaged before merge (see `tests/tft_algo_test.cpp` for the dynamic UBSan
guard that complements it on hosted code).

### The vendor baseline (suppressed per file/tree)

The flag is on both targets, which compile vendored code; those trees carry
a **39-warning** vendor baseline, suppressed so the only visible
`-Wcast-align` warnings are PROJECT code:

| Tree / file | Count | Pattern | Assessment |
| --- | --- | --- | --- |
| `firmware/Drivers/.../stm32h7xx_hal_spi.c` | 26 | register/DMA overlay casts | ST-maintained HAL idiom — not ours |
| `firmware/Drivers/.../stm32h7xx_hal_uart.c` | 6 | register/DMA overlay casts | ST-maintained HAL idiom |
| `firmware/Drivers/.../stm32h7xx_ll_usb.c` | 2 | register overlay casts | ST-maintained |
| `lib/Src/sd_diskio.c` (l.148/166/195/214) | 4 | `(uint32_t*)buff` — FatFs `BYTE*` disk-io buffers handed to SDMMC DMA | SDMMC DMA contract requires word-aligned buffers; every firmware caller passes the aligned static `storageBuffer`. A misaligned FatFs buffer would DMA-fault, not CPU-fault; no fork action. |
| `lib/Src/adafruit_802_sd.c` (l.376) | 1 | `(uint32_t*)dummySector` local byte array | Adafruit BSP example code; same DMA-contract class. |

Suppression mechanics: `set_source_files_properties(... -Wno-cast-align)`
on the two lib files (`lib/CMakeLists.txt`) and on the `Drivers/` +
`Middlewares/` subsets of the firmware C glob (`firmware/CMakeLists.txt`).

### The 10-warning PROJECT baseline (visible, documented)

All ten are the bank-serialization **struct-overlay idiom** — casting the
`char storageBuffer[PROPERTY_FILE_SIZE]` global (formal alignment 1) to
`FlashSynthParams*` / `uint32_t*` at whole-patch-size offsets:

- `filesystem/MixerBank.cpp:132/173/230` and
  `filesystem/PatchBank.cpp:80/101/118/138` — `char*` → `FlashSynthParams*`
  overlays. Offsets are multiples of `ALIGNED_PATCH_SIZE` (1024), so runtime
  alignment reduces to `storageBuffer`'s own address; the array is a large
  global that the toolchain/linker aligns in practice. Formally UB,
  practically stable — the upstream persistence idiom. Hardening (memcpy or
  a union/aligned-typed buffer) is a deliberate serialization-area change,
  NOT drive-by material; filed to deferred-work.
- `filesystem/PatchBank.cpp:81/95/139` — the preset version stamp
  `*(uint32_t*)&storageBuffer[ALIGNED_PATCH_SIZE-5]` (constant offset 1019,
  ≡3 mod 4 — genuinely misaligned regardless of base). Tolerated today
  because a lone store compiles to unaligned-permitting `str` on Cortex-M7;
  7.6 faulted only because gcc FUSED a loop of stores into `strd`. Fragile
  under future codegen — filed to deferred-work (harden with 4-byte
  `memcpy` when the file is next touched).

Any NEW `-Wcast-align` warning outside these ten lines (or a count drift
beyond 10) is new code in the 7.6 class — triage before merge.

## GCC `-Wstrict-overflow=2` (Synth output loops — documented benign)

The escalated-warning sweep (phase 8.8, gcc 15.3.1 arm-none-eabi, see the
recipe below) reports 8 `-Wstrict-overflow=2` warnings on the Synth output
loops, all of the form `assuming pointer wraparound does not occur when
comparing P +- C1 with P +- C2` (line refs as of 2026-09-17): the zero-fill
(`Synth.cpp:357`), the six `out` dispatch pointer-pair loops — cases
0/2/3/5/6/8 (`:446/:458/:467/:479/:488/:500`) — and the clip/×256 loop
(`:516`). The `mixAndPan` fast paths (cases 1/4/7) advance by index, not
pointer comparison, and raise no warning. All are benign **by
construction**, not by placement luck:

- The two SAI DMA callbacks (`preenfm3.cpp`) pass **64-element halves of
  the 128-element** `waveform1/2/3` arrays — `TxHalfCplt` the base pointers,
  `TxCplt` the `+64` offsets.
- `buildNewSampleBlock` computes the end pointers **once** as `bufferN + 64`.
- Every loop advances monotonically (2 elements per iteration in the fill
  and dispatch cases; `mixAndPan`: 2 dest increments × `BLOCK_SIZE`(32) = 64
  elements; the clip pass: 1 element, all three buffers in lockstep) and
  stops at or before `endcbN`.
- All pointer arithmetic therefore stays within `[bufferN, bufferN + 64]` —
  a half of the **same array object** — so C++ same-array bounds hold: no
  pointer can overflow or wrap.

GCC cannot see the same-array relation across TUs, hence the warning; the
proof lives beside the code (comment above `Synth::buildNewSampleBlock`,
same style as the ×256 loop's inline signed-multiply proof) and here. A new
strict-overflow warning OUTSIDE these loops is not covered by this record.

**Scan recipe** (reproduce the escalated warning set — also the source of
the 8.8 SW1 fallthrough harvest; kept non-blocking per phase 8.8's CI
pinning decision; verbatim from the `build/scan-ub` configuration):

```sh
cmake -B build/scan-ub -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=return-mismatch -Wcast-align=strict -Warray-bounds=2 -Wnull-dereference -Wshadow=local" \
      -DCMAKE_CXX_FLAGS="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -Wcast-align=strict -Warray-bounds=2 -Wnull-dereference -Wduplicated-cond -Wlogical-op -Wshadow=local -Wstrict-overflow=2"
cmake --build build/scan-ub 2>&1 | grep -E "warning:" | sort | uniq -c | sort -rn
```

(The C flags carry the vendor `-Wno-error` relaxations for ST's HAL C
sources; the C++ escalation is the strict set the 8.8 harvest ran.)

## UBSan trap-mode diagnostic build (8.8 SW5)

Beside the reporting-only scans above, phase 8.8 SW5 ships an ON-DEVICE
trap-mode build: the restored 8.1 fault-capture module (`pfm3_diag`) plus a
selective sanitizer set compiled so every instrumented check emits a `UDF`
trap instead of calling a runtime. `pfm3DiagInit()` enables
Usage/MemManage/Bus faults, so a trap lands in UsageFault (faultId 4, UFSR
bits in CFSR), the naked shim captures the 19×uint32 frame into `.noinit`, the
unit soft-resets, and boot replays the capture over USB MIDI —
`scripts/hardware/h8_crashwatch.py` decodes the burst unchanged.

**Recipe** (diagnostic only — `test-asan` precedent, NOT a CI gate):

```sh
make ubsan-trap
# → build/ubsan-trap/firmware/preenfm3.{bin,elf,map}; flashes by DFU like
#   any firmware image. The target self-verifies at build time and fails
#   loudly on ANY of: a leaked __ubsan runtime reference (firmware OR
#   preenfm3lib — both targets are instrumented), a missing .noinit map
#   symbol, or a fault-shim ABI regression (objdump-disassembles NMI/Hard/
#   MemManage/Bus/Usage and asserts the exact 7-instruction naked sequence:
#   no prologue, faultId in r0, frame in r1, EXC_RETURN in r2, tail-branch
#   to pfm3DiagFaultHook).

# Manual verification (must print NOTHING — zero libubsan refs, lib included):
arm-none-eabi-nm build/ubsan-trap/firmware/preenfm3.elf | grep __ubsan

# .noinit revalidation — ALWAYS re-derive from the fresh map:
grep __noinit_start__ build/ubsan-trap/firmware/preenfm3.map
# 2026-09-23 review-round build: 0x2001e6d4 (76 B, past _ebss). The parked
# 8.1 value 0x2001de84 is OBSOLETE — .bss has grown past it; never assume
# an old address, the capture struct overlaps .bss if you do.
```

The identical-list rule is absolute and applies to BOTH instrumented targets
(`preenfm3` and `preenfm3lib` — FatFs/SD/TFT/encoders/RingBuffer; without
lib instrumentation the soak has a blind spot over the whole lib tree):
`firmware/CMakeLists.txt` and `lib/CMakeLists.txt` apply
`-fsanitize=alignment,shift,signed-integer-overflow,integer-divide-by-zero,
float-cast-overflow` and `-fsanitize-trap=` with the IDENTICAL list. Any
check present in `-fsanitize` but missing from `-fsanitize-trap` references
a `__ubsan_handle_*` symbol and the bare-metal link fails (there is no
libubsan for Cortex-M7). Measured cost (2026-09-23, firmware+lib): text
457,312 → 561,312 (+22.7%), bss +284 (counters + 76 B `.noinit`); well
inside the 768 K flash budget — the size gate covers the RELEASE build only.

**Policy:** this build is a diagnostic instrument for the owner's device
soak (runbook: `_bmad-output/implementation-artifacts/p88-sw5-device-runbook.md`)
and is deliberately NOT a CI gate. The release-flags question
(`-fno-strict-aliasing`, `-fwrapv` on the shipping build) stays OWNER-GATED:
SW5 only ships the measurement protocol — the trap soak's duty-cycle /
real-time-fit data is the input to that decision, not the decision itself.
Release builds are byte-identical to master (`pfm3_diag.h` inline no-ops; the
empty `.noinit` section adds nothing to the load image).

## Resolved findings (triage history)

Real bugs surfaced by the analyzers and fixed (behavior-preserving unless
noted). Host regression tests guard the fixes.

| File / finding | Class | Resolution |
| --- | --- | --- |
| `note_stack.cpp` `uninitvar` (cppcheck + gcc `-Wmaybe-uninitialized`) | `least_recent_note` / `free_slot` fed to `NoteOff` / `pool_[]` uninit / OOB-write risk | Defensive init + found-guard on both scans (algo preserved). `tests/note_stack_test.cpp` (8 cases, incl. LRU-saturation). |
| `FxBus.cpp:60` `float*->long*` cast (cppcheck portability) | Type-pun for `fastroot` read/wrote 8B on a 4B float on LP64 (latent on Arm, corrupting on 64-bit host) | Union type-pun, `int32_t` (matches `sqrt3()` idiom 8 lines above). |
| `Voice.cpp` 33x `-Wsequence-point` (gcc) | `*sp++ = clamp(*sp - ...)` read+advanced unsequenced | Split into `*sp = ...; sp++;` (16 sites). Same post-increment store under `-Ofast`. |
| `Synth.cpp` 6x `-Wsequence-point` (gcc) | `*p++ + *p++` unsequenced sum | Sequenced into temps (sum is commutative). |
| `Voice.cpp` 4x `-Wstrict-aliasing` (gcc) | `(*(uint_fast8_t*) noise)` byte read — `uint_fast8_t` isn't `unsigned char` | `uint8_t` (precedent: `Voice.cpp:6110` uses it for the same pattern). |
| `LfoOsc.cpp` `-Wmaybe-uninitialized` (gcc) | `lfoValue` — switch with no default | Init to `0.0f` (out-of-range shape -> silence, not garbage). |
| `Hexter.cpp`, `UserEnvCurve.cpp`, `UserWaveform.cpp` `bugprone-incorrect-roundings` | `(int)(x + .5f)` rounds negatives wrong | `(int) lround(x)` on validated-positive domains. `Hexter` `GetRoundedSnapsFrequencyMulToHalfSteps` test (11 cases). |
| `bugprone-branch-clone` (clang-tidy, 4 of 21) | Duplicated switch/branch bodies | Collapsed (FMDisplay3, Hexter algo-5 tautology, Voice encoder cases, Sequencer name-offset). |

### Open / flagged for review

- **`FMDisplayEditor.cpp:3447`** `ROW_ENGINE2` case — the `else if (row ==
  ROW_ENGINE ...)` is likely unreachable inside `case ROW_ENGINE2:` (a dead
  branch / copy-paste bug). Flagged for human review; left untouched (display
  logic, needs someone who knows the engine2/engine glide hide-rules).
- `-Wshadow` deferred (noisy on legacy C++); second pass.
- `lib` is not under `-Wall -Wextra` (vendored majority); split vendor-vs-ours
  into a no-warning OBJECT library if lib warnings are wanted later.

## See also

- `scripts/cppcheck-baseline.txt` — the baseline + regen workflow (header).
- `scripts/ci/static-analysis.sh` — the CI gate logic.
- `.github/workflows/static-analysis.yml` — the CI job.
- `Makefile` (`analyze` target), `scripts/analyze-tidy.sh` (clang-tidy wrapper).
