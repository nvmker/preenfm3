# Host-Testability Seam — Architecture Decision

**Scope:** one seam pattern for all four `tests/README.md` roadmap targets —
`Sequencer.cpp` (serialization), `Hexter.cpp` (DX7 sysex), `Osc/Env/Matrix.cpp`
(synth math), `MidiDecoder.cpp` (MIDI decode). This is a **seam**, not a
refactor: we touch the minimum firmware surface needed to compile the pure
logic on a host compiler, and nothing more.

---

## The decisive evidence (include-graph homework)

I traced the full transitive include closure of every target. The result drives
the whole decision, so it's stated up front:

> **The HAL is reached only from `.cpp`/`.c` translation units — never through a
> firmware-owned header.** The entire header closure of all four targets is
> HAL-free.

Verified facts:

- `Common.h` (reachable from every target) has **zero** `#include`s.
- `SynthState.h`'s chain — `Storage.h` → `FileSystemUtils.h`,
  `ConfigurationFile.h`, `ScalaFile.h`, `PatchBank.h`, `MixerBank.h`,
  `SequenceBank.h`, … — is **clean** (no `HAL_*`, no `main.h`, no FatFs).
- `Osc.h`/`Env.h`/`Matrix.h` → `SynthStateAware.h`/`SynthState.h`/`Common.h`
  → clean.
- `Sequencer.h` → `Common.h` → clean. `Synth.h` → `Timbre`/`Voice`/`Lfo*`/`dwt`
  → clean. `FMDisplaySequencer.h` → `FMDisplay.h` → clean.
- A 4-hop exhaustive expansion of `Sequencer.cpp`'s direct includes surfaces
  **no** bridge to `main.h`, `stm32h7xx*.h`, `usb_device.h`, or `usbd_*`.
- The **only** headers anywhere in `firmware/` that touch the HAL are CubeMX
  outputs (`Inc/main.h`, `Inc/stm32h7xx_hal_conf.h`, `Inc/usb_device.h`,
  `Inc/usbd_conf.h`) and the USB middleware (`usbd_midi.h` → `usbd_ioreq.h` →
  `usbd_def.h` → `usbd_conf.h` → `main.h` → `stm32h7xx_hal.h`).

The HAL call surface **inside** each target `.cpp` is tiny and localized:

| Target | HAL / HW calls inside the `.cpp` | Header that forces HAL? |
| --- | --- | --- |
| `Sequencer.cpp` | `HAL_GPIO_WritePin`, `HAL_GetTick` (3 sites, all in `mainSequencerTic` beat/LED block) | **No** |
| `Hexter.cpp` | **none** | **No** |
| `Osc.cpp` / `Env.cpp` / `Matrix.cpp` | **none** | **No** |
| `MidiDecoder.cpp` | `SET_BIT(huart1.Instance->CR1,…)`, `USBD_LL_Transmit(…)`; `extern USBD_HandleTypeDef` / `extern UART_HandleTypeDef` | **Yes** — `extern "C" { #include "usbd_midi.h" }` |

Two more cross-cutting facts:

- `likely()`/`unlikely()` are GCC `__builtin_expect` macros from `Common.h` —
  host-safe under gnu++14 (which `tests/CMakeLists.txt` already sets).
- `__attribute__((section(".ram_d3" / ".instruction_ram" / ".ram_d1")))`
  appears on statics in `Sequencer.cpp`, `Osc.cpp`, `Env.cpp`. On a host linker
  these are accepted and quietly routed to the default section (a
  `-Wattributes` note at most); not fatal, not behavior-changing.

---

## (a) Seam placement — decision

**Adopt Option A: `#ifdef PFM3_HOST` guards inside the firmware translation
units themselves.** No sibling host-only TU; no fake HAL headers on the test
include path.

Justified against the two alternatives **for this codebase specifically**:

- **vs. Option C (fake HAL headers on the test include path).** The README's
  phrasing — "a `PFM3_HOST` define that stubs `HAL_*` and hardware calls" — is
  correct about *calls* but would mislead about *headers*: the header closure
  never reaches the HAL, so a shadow `stm32h7xx_hal.h` / `main.h` would be
  faking a graph the code never traverses. You'd pay ongoing drift against the
  CubeMX header set for zero reach. The **one** exception is `usbd_midi.h` in
  `MidiDecoder.cpp` — and even there, the cleaner move is to guard the
  `#include` out under `PFM3_HOST`, not shadow it (see per-target list).

- **vs. Option B (extract pure logic into a sibling host-compilable TU).** This
  duplicates the exact code whose regression we're guarding. For `Sequencer.cpp`
  the entire point is the byte-exact `__builtin_memcpy` packing that replaced
  the `*(float*)&buf[i]` hard-fault; copy it into a twin and you're testing the
  twin, not the firmware. Drift between twin and origin is the bug class we
  exist to prevent. Reject.

- **Why Option A is cheap here.** Because the header closure is already clean,
  the guards are surgical: **one 7-line block** in `Sequencer.cpp`, and a
  handful of early-return stubs + one `#include` guard in `MidiDecoder.cpp`.
  `Hexter.cpp`, `Osc.cpp`, `Env.cpp`, `Matrix.cpp` need **zero** source changes
  — they are host-compilable as-is.

**Non-negotiable rule:** `PFM3_HOST` is defined **only** by
`tests/CMakeLists.txt` (via `target_compile_definitions`). It must never appear
in `firmware/CMakeLists.txt` or any firmware header. The firmware build is
untouched.

---

## (b) Per-target stub list (exact surface)

### 1. `Sequencer.cpp` — serialization under test

- **Header stubs needed:** none (closure is HAL-free).
- **Source guards needed (1 block):** wrap the LED/tick block inside
  `mainSequencerTic` —

  ```cpp
  // firmware/Src/midi/Sequencer.cpp, inside mainSequencerTic()
  #ifndef PFM3_HOST
      if ((current16bitTimer_ & 0x300) != lastBeat_) {
          ...
          HAL_GPIO_WritePin(LED_CONTROL_GPIO_Port, LED_CONTROL_Pin, GPIO_PIN_SET);
          ledTimer_ = HAL_GetTick();
      } else if (unlikely(HAL_GetTick() - ledTimer_ > 100)){
          HAL_GPIO_WritePin(LED_CONTROL_GPIO_Port, LED_CONTROL_Pin, GPIO_PIN_RESET);
          ledTimer_ = 0Xffffffff;
      }
  #endif
  ```

  (`LED_CONTROL_GPIO_Port`/`LED_CONTROL_Pin` are not even defined in the
  checked-in tree — they only resolve through CubeMX-generated `main.h`, which
  confirms this block is unhospitable on a host and must be the thing we gate.)
- **`__attribute__((section(".ram_d3")))` on `actions[]`/`stepNotes[]`:** leave
  as-is; host linker accepts it. No guard.
- Functions under test (`getFullDefaultState`, `getFullState`, `setFullState`,
  `loadStateVersion1/2`, `getSequenceNameInBuffer`) reference **none** of the
  guarded symbols — they exercise only the `pfm3_seq_{put,get}_{u16,f32}`
  helpers and the field arrays.

### 2. `Hexter.cpp` — DX7 sysex import under test

- **Header stubs needed:** none.
- **Source guards needed:** none. `Hexter.cpp` is host-compilable **verbatim**.
  It already `#include <math.h>` and uses `exp`, `pow`, `M_LN10` — all provided
  by glibc under `gnu++14` (`_DEFAULT_SOURCE`), which the test project enables
  via `CMAKE_CXX_EXTENSIONS ON`.
- Functions under test: `patchUnpack`, `voiceSetData`, `bulkDumpChecksum`,
  `getPreenFMIM`, `getChangeTime`, `getActualLevel`, `limit`, `voiceCopyName`.

### 3. `Osc.cpp` / `Env.cpp` / `Matrix.cpp` — synth math under test

- **Header stubs needed:** none.
- **Source guards needed:** none. All three compile verbatim. (They `#include`
  `SynthState.h`, whose closure is clean per the evidence above.)
- **Runtime-table caveat:** `Env::init` populates `incTab[]` and `Osc::init`
  precomputes `waveTables[].precomputedValue/phaseMul`. Host tests must call
  `init()` once before asserting — same contract as firmware.
- **Honest note on `Matrix.cpp`:** it contains only ctor/dtor/`init()` — the
  *matrix arithmetic* lives in `FMDisplay.cpp` and inlined headers. Testing
  `Matrix.cpp` alone is near-zero value; the roadmap row really means
  *Matrix + the connect logic*. Flag for the test-authoring step, not this seam.

### 4. `MidiDecoder.cpp` — decode under test (the one non-trivial target)

This is the **only** target where a header (`usbd_midi.h`) drags the HAL graph
in, and the only one with raw register writes. Minimal guard set:

- **Guard the C include out entirely:**

  ```cpp
  #ifndef PFM3_HOST
  extern "C" {
  #include "usbd_midi.h"
  }
  #endif
  ```

- **Guard the two HAL-typed externs out:**

  ```cpp
  #ifndef PFM3_HOST
  extern USBD_HandleTypeDef hUsbDeviceFS;
  extern UART_HandleTypeDef huart1;
  #endif
  ```

- **Stub (don't remove) the four HW-touching helpers** so the call graph still
  links. Early-return under `PFM3_HOST`:
  - `sendMidiDin5Out()` — the `SET_BIT(huart1.Instance->CR1,…)` site.
  - `sendMidiUsbOut()` — the `USBD_LL_Transmit(&hUsbDeviceFS,…)` site.
  - `sendMidiUsbOutIfBufferFull()` — fine as-is (only touches `usbMidiOutBuff`).
  - `writeMidiCCOut()` — body can stay; it only writes `usbMidiOutBuff` and
    `usartBufferOut` (a `RingBuffer`, already host-clean).
- **Result:** the decode path — `newByte`, `newMessageType`, `newMessageData`,
  `midiEventReceived`, `midiEventForInstrument1MPE`, `controlChange`,
  `decodeNrpn` — compiles and runs on host with **zero stub headers**.

---

## (c) CMake wiring recipe (no firmware-build pollution)

`tests/CMakeLists.txt` is a standalone project; all wiring is local to it.
Add after the existing `add_executable(pfm3_tests …)`:

```cmake
# --- Firmware seam: pull host-compilable firmware TUs behind PFM3_HOST -------
set(PFM3_FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../firmware")

# Firmware headers are included flat ("Sequencer.h", "Common.h", ...). Mirror
# firmware/CMakeLists.txt's include set, minus Drivers/Middlewares (we proved
# the closure never needs them).
target_include_directories(pfm3_tests BEFORE PRIVATE
    "${PFM3_FW_ROOT}/Inc"
    "${PFM3_FW_ROOT}/Src"
    "${PFM3_FW_ROOT}/Src/filesystem"
    "${PFM3_FW_ROOT}/Src/hardware"
    "${PFM3_FW_ROOT}/Src/midi"
    "${PFM3_FW_ROOT}/Src/MidiController"
    "${PFM3_FW_ROOT}/Src/midipal"
    "${PFM3_FW_ROOT}/Src/SimpleEffect"
    "${PFM3_FW_ROOT}/Src/synth"
    "${PFM3_FW_ROOT}/Src/utils"
    "${PFM3_FW_ROOT}/../lib/Inc")

# Activate the in-TU seam guards. Defined ONLY here — never in firmware/.
target_compile_definitions(pfm3_tests PRIVATE PFM3_HOST)

# Host linker accepts the firmware's section attributes but notes them;
# silence the note so -Werror sanitizer builds stay green.
target_compile_options(pfm3_tests PRIVATE -Wno-attributes)

# Add firmware sources per coverage session. Example for the Sequencer session:
# target_sources(pfm3_tests PRIVATE
#     "${PFM3_FW_ROOT}/Src/midi/Sequencer.cpp")
```

Rules the recipe enforces:

- **No toolchain file** is passed to the test configure (per `tests/README.md`).
- `PFM3_HOST` is private to `pfm3_tests`; `firmware/CMakeLists.txt` is never
  edited, so the Arm build cannot drift into host-land.
- Firmware headers are searched **before** test-local headers (`BEFORE`), so a
  future test-local helper named like a firmware header can't shadow it
  accidentally; the only intentional shadow would be explicit.
- `tests/` is never `add_subdirectory()`'d from the top level.

---

## (d) Signal-fidelity risks — one warning per target

These are the places where a green host test can lie about the firmware. Each
must be designed around when the tests are written (next step).

1. **`Sequencer.cpp` — the test guards *byte fidelity*, not *misalignment
   safety*.** The original bug was a `-Ofast` unaligned-float hard-fault on
   Cortex-M7. On an x86-64/arm64 host, unaligned float access is **legal** —
   the host will never fault, under any `-O`. So the test cannot reproduce the
   fault; it can only assert that `getFullState` → `setFullState` round-trips
   byte-for-byte and that the `pfm3_seq_{put,get}_f32` helpers at the misaligned
   `tempo` offset (index 14) produce the documented layout across all
   `SEQ_VERSION`s. The unaligned-safe `__builtin_memcpy` helpers are what make
   the code host-safe *and* arm-safe, so exercising their byte fidelity is the
   correct proxy regression guard. **Don't** assert anything about faulting.

2. **`Hexter.cpp` — float math is libm-version-sensitive; assert with
   tolerance.** `voiceSetData` calls `exp(M_LN10 * …)` (fixed-frequency osc
   path) and `pow(2, …)` (env timing). The firmware links newlib (Arm); tests
   link glibc. Results agree to ~1 ULP but not bit-exactly. Use
   `EXPECT_NEAR(..., tol)` on computed floats, or — better — golden-vector
   tests: feed a captured DX7 packed patch, compare against firmware-captured
   `OneSynthParams` output. Also: the host test will faithfully expose the
   dead-branch bug at `transposeMultiply` (`else if (transpose < -18)` after
   `if (transpose < -6)` is unreachable). The seam must preserve firmware
   behavior; **assert what the code does, not what it intended** — flag the bug
   separately, don't silently "fix" it in the golden.

3. **`Osc/Env/Matrix.cpp` — beware the latent fall-through and the near-empty
   TU.** (i) `Osc::getNoteRealFrequencyEstimation` has a **missing `break`**
   between `OSC_FT_KEYBOARD` and `OSC_FT_FIXE` (fall-through). A host test
   must assert the *current* (fall-through) output as the golden; fixing it is
   a separate, explicit decision. (ii) `Matrix.cpp` has almost no testable
   body — the real coverage lives in `FMDisplay.cpp`; don't claim "Matrix
   coverage" from testing `Matrix::init` alone. (iii) Runtime tables (`incTab`,
   `waveTables` precompute) must be initialized via `init()` before any assert
   — same as firmware, but easy to forget in a fixture.

4. **`MidiDecoder.cpp` — a no-op stub can hang the test, and routing cannot be
   tested in isolation.** Two traps:
   - **Infinite loop:** `sendCurrentPatchAsNrpns` ends each NRPN with
     `while (usartBufferOut.getCount() > 0) {}`, relying on the USART ISR to
     drain. With `sendMidiDin5Out()` stubbed to no-op, `usartBufferOut` never
     drains → the test hangs. The stub must either keep `writeMidiCCOut`/drain
     semantics functional, or the NRPN-*send* path must be explicitly excluded
     from host test scope (the *decode* path is the stated target). Document
     this boundary in the test file.
   - **Routing needs a Synth:** the bug class ("stuck notes / wrong CC
     routing") lives in `midiEventReceived` + `controlChange`, which dispatch
     into `synth->noteOn/noteOff/setNewValueFromMidi/getTimbre(…)->setMatrixSource`.
     `Synth`'s methods are non-virtual concrete — you cannot drop in a mock
     without a refactor (virtual interface or template), which is out of scope
     for "a seam." Therefore MidiDecoder routing tests must **link the real
     Synth graph** (`Synth.cpp` + `Timbre`/`Voice`/`Matrix`/`FxBus` + their
     static tables — all proven host-compilable) and assert on observable
     timbre/voice state after feeding a byte stream. This makes MidiDecoder the
     heaviest of the four test targets by far; budget accordingly.

---

## Summary table

| Target | Seam change in firmware | Stub headers? | Fidelity warning (one-liner) |
| --- | --- | --- | --- |
| `Sequencer.cpp` | guard 1 LED/tick block | no | host can't fault; assert byte layout only |
| `Hexter.cpp` | none | no | libm differs; golden-vector + tolerance |
| `Osc/Env/Matrix.cpp` | none | no | fall-through bug is golden; `Matrix.cpp` is near-empty |
| `MidiDecoder.cpp` | guard `usbd_midi.h` + 2 externs; no-op 2 HW helpers | no | stub can hang the NRPN loop; routing needs real Synth |

**Next step (out of scope here):** write the `*_test.cpp` files, one per
target, against this seam.

---

## Appendix: contact with the code (Target #1 implementation findings)

*Added when the Sequencer target was implemented. Several decisions above were
refined by reality; recorded here so Targets #2–#4 inherit the corrected picture
instead of the optimistic one.*

### Correction 1 — the header closure is NOT HAL-free

The decision's central claim ("the HAL is reached only from `.cpp`/`.c` files,
never through a firmware-owned header") is **false**. The closure bridges to the
HAL through `lib/Inc`, which the prior include-graph homework searched neither
for HAL references nor as a reachable directory:

```
Synth.h → SynthState.h → Storage.h → MixerBank.h → PreenFMFileType.h
        → fatfs.h (lib/Inc) → sd_diskio.h → adafruit_802_sd.h
        → adafruit_802_conf.h → stm32h7xx_nucleo_conf.h → stm32h7xx_hal.h
```

`PreenFMFileType.h` includes `fatfs.h` because its *method signatures* use the
FatFs `FIL` type. In the closure `FIL` appears only in declarations (`FIL*`,
`FIL&`, one return-by-value) — never embedded by value; all real `static FIL`
objects live in `.cpp` files the host tests do not compile.

**Remedy (deviation from "no fake headers"):** a minimal host shim
`tests/host_shims/fatfs.h`, placed FIRST on the test include path, that
forward-declares `FIL` and omits the SD-disk/HAL branch. Scoped to one tiny
shim; forward-declarations only; no logic. This is the *one* justified fake
header, and only because the real `fatfs.h` is an unguarded chain into the HAL.

### Correction 2 — `__attribute__((section("…")))` is a hard ERROR on Mach-O

The decision claimed section attributes are "a `-Wno-attributes` note at most."
That holds on ELF/Linux; on Mach-O (macOS host — the local-dev and one CI leg)
clang errors: *"mach-o section specifier requires a segment and section
separated by a comma."* `-Wno-attributes` cannot downgrade it (it is a target
diagnostic, not an attribute warning).

**Remedy:** `#ifndef PFM3_HOST` guards around the two `__attribute__((section(.
".ram_d3")))` declarations in `Sequencer.cpp`. The same guard will be needed for
`Osc.cpp` / `Env.cpp` / `FxBus.cpp` / `Lfo.cpp` / `Timbre.cpp` in the synth-math
run — same pattern, copy it.

### Correction 3 — `Common.h` redeclares `strcmp` (linkage conflict on host)

`Common.h:~700` has a bare file-scope `int strcmp(const char*, const char*);`
(C++ linkage). Host libc's `<string.h>` — pulled transitively by libc++ —
declares `strcmp` with C linkage → "different language linkage" error. This is
also a latent firmware smell (libc functions should have C linkage).

**Remedy:** `#ifndef PFM3_HOST` guard around the redeclaration; host libc
provides `strcmp`. The smell is **flagged, not fixed** (`extern "C"`), to keep
the Arm build byte-identical.

### Correction 4 — `Voice.h` `__USAT` is ARM inline asm

`Voice.h` defines a `__USAT` macro using `asm("usat …" : … : "I"(sat), …)`. The
`"I"` constraint is ARM-only; clang rejects it on host during inline-asm
constraint validation when `Voice.h` is parsed (even though no `Voice` method is
instantiated).

**Remedy:** `#ifdef PFM3_HOST` branch providing a portable, *semantics-correct*
C++ unsigned-saturate (`pfm3_host_usat`). A correct fallback (not a no-op) keeps
any future synth-math host tests faithful.

### Correction 5 — linking needs a collaborator stub

The decision assumed `target_sources(Sequencer.cpp)` alone would link. It does
not: `Sequencer.o`'s compiled body references out-of-line `Synth::` /
`FMDisplaySequencer::` methods (in functions the tests never call but whose
bodies are compiled), plus the inline `Synth::midiClockSongPositionStep`, whose
body calls `Timbre::midiClockSongPositionStep`.

**Remedy:** `tests/stubs/sequencer_collaborators_stub.cpp` — empty bodies for
the 13 referenced out-of-line methods (8 `Synth` + 1 `Timbre` + 4
`FMDisplaySequencer`). Standard test-double, not a HAL fake. Signature drift
fails to compile (intended early signal).

### Rule refinement (supersedes the "non-negotiable rule" in §a)

> **`PFM3_HOST` MAY appear in firmware headers, but ONLY for constructs that are
genuinely host-incompatible — libc redeclarations, ARM inline asm, and section/
link attributes. NEVER for logic. The guards are inert under the Arm build
(original code path unchanged inside `#ifndef PFM3_HOST` / `#else`), so firmware
behavior is preserved byte-for-byte. `PFM3_HOST` is still defined ONLY by
`tests/CMakeLists.txt`.**

### Actual Sequencer-seam surface (replaces the summary-table row)

| File | Change | Why |
| --- | --- | --- |
| `firmware/Src/midi/Sequencer.cpp` | `#ifndef PFM3_HOST` around the LED/`HAL_GetTick` block **and** around both `.ram_d3` section attributes | HAL calls; Mach-O section-attribute hard error |
| `firmware/Src/synth/Common.h` | `#ifndef PFM3_HOST` around the file-scope `strcmp` redeclaration | libc linkage conflict |
| `firmware/Src/synth/Voice.h` | `#ifdef PFM3_HOST` portable `__USAT` fallback | ARM inline-asm constraint |
| `tests/CMakeLists.txt` | firmware include dirs (host_shims first), `PFM3_HOST`, `-Wno-attributes -Wno-macro-redefined -Wno-writable-strings`, `target_sources(Sequencer.cpp + stub)` | seam wiring |
| `tests/host_shims/fatfs.h` | forward-declares `FIL`, omits SD-disk/HAL chain | the one justified host shim |
| `tests/stubs/sequencer_collaborators_stub.cpp` | 13 empty collaborator bodies | link satisfaction |
| `tests/sequencer_test.cpp` | 6 tests, 8 ctest entries (incl. smoke) | coverage |

### Layout correction (cosmetic)

The serialization size is **80 bytes** (per-timbre block is 8 bytes:
`u16 stepUnique + u16 timerMask + u8 seqActivated + u8 recording + u8 muted +
u8 instrumentStepSeq`), not the 68 informally estimated during seam design. The
tempo offset (**14**, the regression target) is unaffected.

---

## Appendix: contact with the code (Target #2 implementation findings)

*Added when the Hexter (DX7 sysex) target was implemented. The seam needed ZERO
new firmware guards — `Hexter.cpp` compiled verbatim, exactly as §b predicted.
The notable event was the crash-class bug this target was built to catch — and
it fired on the first structured-garbage input under ASAN.*

### Finding — unbounded table index in `voiceSetData` (global-buffer-overflow)

The whole point of Target #2 (per `tests/README.md` row 2: "crash/corruption on
malformed sysex") was to surface exactly this class of bug. The ASAN build
caught one on the first structured-garbage input.

**Site:** `firmware/Src/utils/Hexter.cpp:932`

```cpp
params->matrixRowState2.mul = dx7_voice_amd_to_ol_adjustment[(patch[140])] / 100.0f;
```

- `dx7_voice_amd_to_ol_adjustment` is `const float[100]` (400 bytes) — a
  file-scope global in `Hexter.cpp`.
- `patch[140]` (unpacked LFO AMD) is copied **raw** from `packed[115]` by
  `patchUnpack`'s "lamd" loop (`*up++ = *pp++`, no mask). The `packed[115] →
  unpacked[140]` mapping was confirmed empirically. Every **other** table access
  in `voiceSetData` is wrapped in `limit(…,0,99)`; this one alone is not.
- Any patch whose `packed[115] >= 100` reads past the 100-entry global.

**Captured ASAN/UBSAN trace** (Apple clang 21, arm64 host, `index 125`):

```text
Hexter.cpp:932:32: runtime error: index 125 out of bounds for type 'const float[100]'
==ERROR: AddressSanitizer: global-buffer-overflow ... READ of size 4
    #0 Hexter::voiceSetData(OneSynthParams*, unsigned char*) Hexter.cpp:932
    #1 Hexter::loadHexterPatch(unsigned char*, OneSynthParams*)  Hexter.cpp:183
... located 100 bytes after global variable 'dx7_voice_amd_to_ol_adjustment' of size 400
```

**Two amplifiers worth noting** (both shape the test design):

1. **The read's result is immediately discarded.** `matrixRowState2.mul` is
   reassigned to `0.0f` two statements after line 932, so the line is
   effectively dead — but the OOB read still executes (UB). On-device under
   `-Ofast`, an adversarial sysex with a huge LFO-AMD byte could read past the
   flash-resident table into an unmapped region and hard-fault: the same crash
   class Target #1 guarded for the sequencer. The bug is NOT observable in the
   imported `OneSynthParams` output, so the test cannot lock it via an output
   golden — only via the ASAN trace.
2. **Large indexes skip ASAN's global redzone.** `packed[115]==255` (an
   all-`0xFF` patch) reads index 255, which lands on an adjacent valid global —
   ASAN-silent, but still wrong-data UB. (This is why the all-ones test passed
   despite also being OOB.) "Green under ASAN" is therefore necessary but not
   sufficient; the malformed-input tests **clamp** `packed[115]` to `<=99`
   rather than relying on ASAN to flag every case.

**Resolution:** FIXED. The follow-up firmware change bounds the index with
`limit(…,0,99)`, matching every other table access in `voiceSetData`:

```cpp
params->matrixRowState2.mul = dx7_voice_amd_to_ol_adjustment[limit(patch[140], 0, 99)] / 100.0f;
```

The dead-store (the read's result is still discarded two lines later) is
intentionally left untouched — the bug was the OOB read, not the discard;
deleting the line is a separate cleanup decision. The test suite flipped in
lockstep with the fix:

- The `packed[115]` clamps in the malformed-input tests are **removed** — they
  now feed genuinely out-of-range bytes (125 via structured garbage, 255 via
  all-ones) through the FULL pipeline to guard the clamp end-to-end under ASAN.
  The exact input that previously aborted the suite (`packed[115]==125`) now
  imports cleanly.
- `HexterUnboundedIndex.OutOfRangeLfoAmdIndexIsClampedByFirmware` drives the
  exact indices ASAN caught (100/125/255) directly into `voiceSetData` — the
  unit-level guard that aborts under ASAN if the `limit()` is ever removed.
- `HexterUnboundedIndex.AmdTableReadResultIsDiscardedByFirmware` is unchanged
  (the discard golden survives the fix).

Both host builds stay green (26/26, incl. ASAN/UBSAN), and the Arm cross-build
links clean.

### Also characterized (preserved as golden, NOT fixed)

- **`transposeMultiply` dead branch** — in `voiceSetData`, `else if (transpose
  < -18)` is unreachable after `if (transpose < -6)`, so `transposeMultiply`
  never reaches `0.25f`. The `HexterTransposeDeadBranch` suite asserts the
  current (`0.5f`-only) behavior so a future fix is a visible, deliberate
  change.

### Actual Hexter-seam surface (replaces the summary-table row)

| File | Change | Why |
| --- | --- | --- |
| `firmware/Src/utils/Hexter.cpp` | **none** | host-compilable verbatim, as §b predicted — no HAL, no ARM asm, no section attrs |
| `tests/CMakeLists.txt` | `target_sources` += `Hexter.cpp` + `Presets.cpp` (the REAL `Presets.cpp`, not a stub) | `voiceSetData` seeds `params` from `defaultPreset` via a float-wise memcpy; pulling real `Presets.cpp` keeps the golden snapshot faithful to actual firmware defaults |
| `tests/hexter_test.cpp` | 18 ctest entries: pure-helper goldens, pipeline golden + determinism, malformed-input crash guard, span-contract finding, **unbounded-index finding**, transpose dead-branch golden | coverage |
| `tests/stubs/*` | **none added** | Hexter's functions are pure over the `OneSynthParams` POD — no collaborator symbols to satisfy (contrast Sequencer's 13-method stub) |

No new host-incompatible constructs surfaced in `Hexter.cpp`. The `fatfs.h`
shim, the `Common.h` `strcmp` guard, the `Voice.h` `__USAT` fallback, and the
section-attribute/Mach-O guards from the Target #1 appendix all carry over
unchanged — Targets #3 and #4 inherit them as-is.
