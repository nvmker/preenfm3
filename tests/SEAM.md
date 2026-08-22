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
  `SynthState.h`, whose closure is clean per the evidence above.) **[CORRECTED
  by Target #3 — see appendix: THREE section-attribute guards ARE needed
  (Osc.cpp:38, Env.cpp:21, Env.cpp:49), exactly as Target #1 appendix
  Correction 2 predicted.]**
- **Runtime-table caveat:** `Env::init` populates `incTab[]` and `Osc::init`
  precomputes `waveTables[].precomputedValue/phaseMul`. Host tests must call
  `init()` once before asserting — same contract as firmware.
- **Honest note on `Matrix.cpp`:** it contains only ctor/dtor/`init()` — the
  *matrix arithmetic* lives in `FMDisplay.cpp` and inlined headers. Testing
  `Matrix.cpp` alone is near-zero value; the roadmap row really means
  *Matrix + the connect logic*. Flag for the test-authoring step, not this seam.

  > **STALE — corrected by Target #3 implementation:** there is **no
  > `FMDisplay.cpp`** in the tree. The real matrix arithmetic is **inline in
  > `Matrix.h`** (`computeAllDestinations()`, `setSource()`, `getDestination()`,
  > `getSource()`); the `FMDisplay*` family is split across
  > `firmware/Src/hardware/FMDisplay{3,Menu,Mixer,Editor,Sequencer}.cpp`, none
  > of which is the matrix arithmetic. Matrix coverage = test the **inline
  > header methods** (genuine `source × mul → destination` routing across the
  > MTX1-4 rows, pure arithmetic → exact) with `Matrix.cpp`'s `init()` supplying
  > the rows pointer. This is real value, not near-zero — do not under-scope
  > it. See the Target #3 appendix.

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

### Correction 3 — `Common.h` redeclares `strcmp` (linkage conflict on host) — RESOLVED (6.11)

`Common.h:~700` had a bare file-scope `int strcmp(const char*, const char*);`
(C++ linkage). Host libc's `<string.h>` — pulled transitively by libc++ —
describes `strcmp` with C linkage → "different language linkage" error.

**History:** first `#ifndef PFM3_HOST`-guarded (flagged, not fixed). **Fixed
in bug-fix Phase 6 item 6.11** (owner decision 2026-08-22): the declaration
is now `extern "C"`, guarded by `#ifdef __cplusplus` for the C TUs that
also include the header (`stm32h7xx_it.c`). `<string.h>`/`<cstring>` are
includable after `Common.h` again — the hand-declared `strnlen` /
`memcpy` workarounds in `PreenFMFileType.cpp` / `MidiControllerFile.cpp`
were dropped.

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
| `firmware/Src/synth/Common.h` | ~~`#ifndef PFM3_HOST` around the file-scope `strcmp` redeclaration~~ **removed (6.11):** `#ifdef __cplusplus` + `extern "C" strcmp` — C-linkage declaration coexists with `<string.h>` on host and target | libc linkage conflict (resolved)
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

---

## Appendix: contact with the code (Target #3 implementation findings)

*Added when the synth-math (Osc/Env/Matrix) target was implemented. Target #1
appendix Correction 2 predicted the section-attribute guards; this appendix
records what else reality required.*

### Correction 6 — three section attributes ARE needed (as Correction 2 predicted)

§b.3 claimed Osc/Env/Matrix "compile verbatim" with no source guards. **False on
a Mach-O host**, exactly as Target #1 appendix Correction 2 forecast for the
synth-math run. Three `__attribute__((section(".instruction_ram")))`
declarations are hard errors under Apple clang (mach-o section specifier
requires a segment and section separated by a comma); `-Wno-attributes` cannot
downgrade them (target diagnostic, not an attribute warning). Gated with the
established `#ifndef PFM3_HOST` leading-attribute pattern copied from
`Sequencer.cpp:38-43`:

| File | Site | Declaration |
| --- | --- | --- |
| `firmware/Src/synth/Osc.cpp:38` | `userWaveform[6][1024]` | section attr |
| `firmware/Src/synth/Env.cpp:23` | `Env::incTab[1601]` | section attr |
| `firmware/Src/synth/Env.cpp:57` | `userEnvCurves[4][64]` | section attr |

Inert under the Arm build (the attribute is preserved verbatim inside
`#ifndef PFM3_HOST`); the cross-build links clean and the firmware binary is
byte-identical.

### Correction 7 — link deps are REAL firmware TUs, not stubs (waves.c, Common.cpp)

§b said Osc/Env/Matrix need no stubs. True for collaborator METHODS (none), but
the compiled bodies reference two **data** symbols whose definitions live in
sibling firmware TUs:

- **`waves.c`** — `Osc.cpp`'s `waveTables[]` initializer references `sinTable`,
  `sawTable`, `squareTable`, `sinSquareTable`, `sinOrZeroTable`, `sinPosTable`,
  all defined in `firmware/Src/synth/waves.c` (a plain-**C** data TU). Pulled for
  real (not stubbed) so the wavetable pointers — and thus every DSP golden —
  reflect actual firmware data. Required enabling the **C** language in the test
  `project(... LANGUAGES C CXX)`; `waves.c` compiles byte-identically to the
  firmware Arm build.
- **`Common.cpp`** — `Env::init()` calls `checkIsLoop()` (inline in `Env.h`),
  whose body indexes `algoOpInformation[(int)*algoNumber][envNumber]`. That
  table is defined in `firmware/Src/synth/Common.cpp` (pure data). Pulled for
  real so the carrier/modulator routing is faithful (a zero-stub would make
  every env a non-modulator and silently change loop/release behavior).

`Matrix.cpp` has no extern deps beyond its own members. No collaborator-method
stub file was needed (contrast Sequencer's 13-method stub).

### Notable approach — minimal `SynthState` (memset + `tuning_` patch)

`Osc::newNote` / `getNoteRealFrequencyEstimation` dereference exactly one
`SynthState` field — the public `mixerState.tuning_` (multiplied by `INV440`).
`SynthState`'s constructor is out-of-line (`SynthState.cpp`, not compiled here),
so a real construction would not link. The fixture instead backs the pointer
with a zeroed, `alignof(SynthState)`-aligned byte buffer and patches
`mixerState.tuning_ = 440.0f` (neutralising `INV440`). Osc never dispatches a
virtual or touches another member, so:

- the member write lands at the byte offset the compiler computes from the
  class definition (no layout guesswork);
- UBSAN's vptr check does not fire on a plain data-member access (it fires only
  on virtual dispatch / member-fn call through a bad vptr), so this is clean
  under `-fsanitize=undefined`;
- ASAN is a non-issue (the buffer is real, properly sized/aligned).

Object lifetime is technically not begun, which is why this is documented here
rather than used indiscriminately — it is a scoped, justified deviation for a
struct the firmware treats as a bag of bytes with one float field. `Env` and
`Matrix` need no `SynthState`.

### Latent bug PRESERVED as golden — `getNoteRealFrequencyEstimation` fall-through

`Osc::getNoteRealFrequencyEstimation` (Osc.cpp ~L220) has **no `break`**
between the `OSC_FT_KEYBOARD`, `OSC_FT_FIXE`, and `OSC_FT_KEYHZ` cases. All
three fall through to the KEYHZ formula; the KEYBOARD and FIXE results are
computed then immediately overwritten. Asserted as the CURRENT golden:

- `OscFreqEstimationFallThrough.AllFrequencyTypesYieldKeyHzFormula` — all
  three frequencyTypes return the SAME (KEYHZ-formula) value for identical
  inputs. A future fix that adds the breaks flips this test.
- `OscFreqEstimationFallThrough.NewNoteDifferentiatesByFrequencyType` —
  contrast proof: `Osc::newNote`'s switch DOES have breaks, so it yields three
  DISTINCT `mainFrequency` values. This pins the bug as estimation-specific
  (not a property of the enum or inputs) and documents the intended behavior a
  fix should restore.

NOT fixed here — flagged for a separate firmware change, exactly as Target #2
did for the `transposeMultiply` dead branch.

### Signal-fidelity note — host goldens guard the shared source, not -Ofast

The oscillator/envelope hot paths are pure float arithmetic; the host test
compiles them under strict IEEE (no `-Ofast`). A firmware-only `-Ofast`
reordering that does not also manifest in the host build would not be caught
here — but the realistic silent-regression classes (wavetable **table-layout**
change, `Osc::init` **precompute-math** change, **index-wrap** change, Env
**curve-table / incTab** change, Matrix **routing** change) are shared between
the host and firmware builds (same source), so the host goldens DO guard them.
Captured goldens: a 32-sample `getNextBlock` block at A4 (SIN), and a 25-point
`getNextAmpExp` trace across a full noteOn→release ADSR (envExponential curve).
A tiny `EXPECT_NEAR` (1e-5) absorbs 1-ULP host FPU / FMA-contraction differences
across gcc/clang, x86/arm64; pure-int matrix arithmetic and single-derive
checks stay exact.

### Actual synth-math seam surface (replaces the summary-table row)

| File | Change | Why |
| --- | --- | --- |
| `firmware/Src/synth/Osc.cpp:38` | `#ifndef PFM3_HOST` around the `userWaveform` section attr | Mach-O hard error |
| `firmware/Src/synth/Env.cpp:23,57` | `#ifndef PFM3_HOST` around the `incTab` + `userEnvCurves` section attrs | Mach-O hard error |
| `tests/CMakeLists.txt` | enable `C` language; `target_sources` += `Osc.cpp` + `waves.c` + `Env.cpp` + `Matrix.cpp` + `Common.cpp` | waveTables data + `algoOpInformation` link deps |
| `tests/synth_math_test.cpp` | 21 ctest entries: Osc DSP goldens (OFF/frozen/advancing/block-vs-sample) + estimation fall-through + newNote contrast; Env incTab-formula + ADSR lifecycle + curve-routing + 25-pt trace; Matrix single-row + MTX1-4 coupling + MTX2/3/4 + skip-rule + row0-assign | coverage |
| `tests/stubs/*` | **none added** | Osc/Env/Matrix have no out-of-line collaborator-method symbols; all link deps are real data TUs |

No NEW host-incompatible construct beyond the three predicted section
attributes. The `fatfs.h` shim, `Common.h` `strcmp` guard, `Voice.h` `__USAT`
fallback, and the Sequencer section-attribute guards all carry over unchanged.
Target #4 (MidiDecoder) remains the only target needing a header guard
(`usbd_midi.h`) and HW-helper stubs.

---

## Appendix: contact with the code (Target #4 implementation findings)

*Added when the MIDI decode target was implemented. Target #4 was forecast in
§b.4 + §d.4 as the heaviest of the four targets by far (the only one where a
header drags the HAL graph in, the only one with raw register writes, and the
only one whose routing tests need the REAL Synth graph). Reality matched the
forecast — and the link gate (the marquee risk) cleared, so all three tiers
shipped.*

### GO/NO-GO gate result — PASSED: the real Synth graph links on host

The marquee risk (§d.4.2) was whether the real Synth graph — `Synth.cpp` +
`Timbre.cpp` + `Voice.cpp` + `FxBus.cpp` + `Lfo*.cpp` + their closures — would
link into the host build in reasonable effort. **It does.** The closure is 11
real firmware TUs (no Synth refactor, no mock — Synth's methods stay non-virtual
concrete, exactly as §d.4.2 mandated):

| Real firmware TU pulled | Why |
| --- | --- |
| `midi/MidiDecoder.cpp` | the unit under test |
| `synth/Synth.cpp` | noteOn/noteOff/setNewValueFromMidi/controlChange dispatch target |
| `synth/Timbre.cpp` | Synth::noteOn → Timbre::noteOn → voice allocation (lowerNote_ observable) |
| `synth/Voice.cpp` | Timbre::noteOn → Voice::noteOn (voice state) |
| `synth/FxBus.cpp` | Synth::buildNewSampleBlock + MixerState member |
| `synth/Lfo.cpp` / `LfoOsc.cpp` / `LfoEnv.cpp` / `LfoEnv2.cpp` / `LfoStepSeq.cpp` | Timbre/Voice LFO members |
| `synth/MixerState.cpp` / `SynthStateAware.cpp` / `SynthParamListener.cpp` | SynthState base + mixer |
| `midipal/note_stack.cpp` / `event_scheduler.cpp` | Timbre/Voice arpeggiator + note-stack members |
| `SimpleEffect/SimpleComp.cpp` / `SimpleEnvelope.cpp` | Synth's `instrumentCompressor_[NUMBER_OF_TIMBRES]` member |

`SynthState.cpp` is **deliberately NOT pulled**: its closure drags the FMDisplay
family (`FMDisplayMixer/Menu/Editor/Sequencer.h`) + HAL (`HAL_RNG_*`). The
SynthState ctor is never called — the test fixture backs SynthState with a
`memset`+aligned buffer and `reinterpret_cast<SynthState*>` (Target #3
minimal-SynthState precedent, scaled up). The out-of-line **non-virtual**
SynthState methods the compiled Synth/Timbre/Voice bodies reference
(`setCurrentInstrument`, `scalaSettingsChanged`, `setParamsAndTimbre`,
`loadPresetFromMidi`) are stubbed in
`tests/stubs/midi_decoder_collaborators_stub.cpp` — standard link-time test
doubles for code paths the routing tests never drive (preset-load / Scala /
instrument-switch flows).

### Synth.cpp — four new PFM3_HOST guards (the predicted HAL surface)

Synth.cpp's HAL surface is tiny and localized (the §b prediction of "none" for
Osc/Env/Matrix held; for Synth it did not). Four guards, all inert under Arm:

| File / site | Guard | Why |
| --- | --- | --- |
| `Synth.cpp` top | `#ifndef PFM3_HOST` around `#include "stm32h7xx_hal.h"` | the CubeMX HAL header has no host build |
| `Synth.cpp` `extern RNG_HandleTypeDef hrng;` | `#ifndef PFM3_HOST` | HAL-typed extern (same pattern as the 2 MidiDecoder externs) |
| `Synth.cpp` `Synth::buildNewSampleBlock` RNG site | `#ifdef PFM3_HOST … random32bit = 0; #else <HAL_RNG acquisition + noise[31] fallback> #endif`; the `noise[]` fill loop is **shared** (runs unconditionally, seeded by whichever branch set `random32bit`) | gates only the host-incompatible CALL (the HAL RNG acquisition); the deterministic fill is kept shared so the host path never leaves `noise[]` uninitialized if `buildNewSampleBlock` is ever exercised on host. Extends the §b.4 carve-out from "early-return in HW helpers" to "mid-function in HW-call-bearing paths". **Flagged** as a justified extension — see "Refined rule" below. |
| `Synth.cpp` `Synth::beforeNewParamsLoad` `HAL_Delay(5)` | `#ifndef PFM3_HOST` around the call | host-incompatible call; tests never call `beforeNewParamsLoad` |

`Synth.cpp` also declares `extern uint32_t SystemCoreClock;` under `#ifdef
PFM3_HOST` (the CMSIS variable is normally declared via the HAL chain gated
out; the host stub TU defines it). `noise[32]` needs no stub — it's defined in
`Osc.cpp:25`, already pulled via Target #3.

### Section-attribute guards — FxBus, Lfo, Timbre (as Target #1 appendix Correction 2 predicted)

Target #1 appendix Correction 2 explicitly forecast that `FxBus.cpp` /
`Lfo.cpp` / `Timbre.cpp` would need the same Mach-O section-attr guard as
`Osc.cpp`/`Env.cpp`/`Sequencer.cpp`. They did. Each `__attribute__((section(
".ram_d1"/".ram_d2"/".ram_d2b")))` declaration is rewritten to the leading-
attribute pattern (verbatim from `Osc.cpp:38`): the attribute is gated under
`#ifndef PFM3_HOST`, the **definition** is preserved so the symbol still links.
Inert under Arm (the compiled output is byte-identical — a leading vs trailing
`__attribute__` on a static-data-member definition applies identically).

| File | Sites gated | Section names |
| --- | --- | --- |
| `synth/Lfo.cpp:22` | `Lfo::invTab[2048]` | `.ram_d1` |
| `synth/FxBus.cpp:72-87` | 14 static-data-member definitions (`delay1-4Buffer`, `predelayBuffer`, `inputBuffer1-4`, `diffuserBuffer1-4`) | `.ram_d1`, `.ram_d2`, `.ram_d2b` |
| `synth/Timbre.cpp:28,30` | `midiNoteScale[2][6][128]`, `Timbre::delayBuffer[6][N]` | `.ram_d1`, `.ram_d2b` |

`Timbre.cpp` also picks up a `#ifdef PFM3_HOST #include <cstddef> #endif` —
`Timbre.cpp:2604` uses `NULL` without a defining header (the firmware build
gets it via the transitive HAL chain; host needs `<cstddef>`). Harmless under
Arm (`NULL` already defined).

### MidiDecoder.cpp — the §b.4 guards applied verbatim

Exactly as §b.4 specified, no surprises:

- `usbd_midi.h` include guarded out (`#ifndef PFM3_HOST extern "C" { #include "usbd_midi.h" } #endif`).
- Two HAL-typed externs guarded (`USBD_HandleTypeDef hUsbDeviceFS`,
  `UART_HandleTypeDef huart1`).
- Two HW-touching helpers given `#ifdef PFM3_HOST … return; #else <real body> #endif`
  early-return stubs: `sendMidiDin5Out` (the `SET_BIT(huart1.Instance->CR1,…)`
  site) and `sendMidiUsbOut` (the `USBD_LL_Transmit(&hUsbDeviceFS,…)` site).
- Left host-clean as-is: `sendMidiUsbOutIfBufferFull` (touches only
  `usbMidiOutBuff`) and `writeMidiCCOut` (writes `usbMidiOutBuff` +
  `usartBufferOut`).

### Trap #1 (§d.4.1) — the NRPN-send hang — DODGED

`sendCurrentPatchAsNrpns` ends each NRPN with `while (usartBufferOut.getCount()
> 0) {}`, relying on the USART ISR to drain. With`sendMidiDin5Out` stubbed to
no-op (no ISR on host), `usartBufferOut` NEVER drains → infinite loop. The
SEND path is excluded from host scope exactly as §d.4.1 mandated: tests never
call `sendCurrentPatchAsNrpns`,`newParamValue` (the param-change-out path), or
`processAsyncActions` with a `SEND_PATCH_AS_NRPN` action. The DECODE-side
enqueue of `SEND_PATCH_AS_NRPN` (NRPN paramMSB=127, paramLSB=127) IS tested —
`MidiNrpn.ParamMsb127WithLsb127EnqueuesSendPatchAsNrpn` asserts
`asyncActions.getCount()` increments, then does NOT drain the queue. The
boundary is documented in the test file header.

### FAVOR-REAL-DATA EXCEPTION — `allParameterRows` stubbed (not pulled)

`allParameterRows` is a `struct AllParameterRowsDisplay` (an array of
`ParameterRowDisplay*` row pointers) defined in
`firmware/Src/hardware/FMDisplayEditor.cpp`. That TU is 4365 lines of TFT/UI
code (`FirmwareTftDisplay.h`, `COLOR_*`, `LINE_PARAM_*`, `tft_->...`) and is
NOT host-compilable in reasonable effort — so this is the ONE justified
exception to the established decision rule ("pull the REAL firmware TU when it
carries data or pure logic that influences a golden"). The stub
(`midi_decoder_collaborators_stub.cpp`) provides ONE zero-initialized
`ParameterRowDisplay` and points every `row[i]` at it. The zeroed
`ParameterDisplay` entries have `displayType=DISPLAY_TYPE_NONE` (=0), so
`MidiDecoder::decodeNrpn`'s FLOAT-family check is false and the raw NRPN value
passes through unchanged to `synth->setNewValueFromMidi`. This lets the
paramMSB<2 branch execute and dispatch without crashing; the value-
transformation logic that depends on real `ParameterDisplay` data
(`displayType`, `minValue`) is NOT under test here. The NRPN ASSEMBLY (the
Tier 2 target) is independent of this data and is faithfully exercised.

### Refined rule (supersedes §b.4 carve-out letter, preserves its spirit)

> **`PFM3_HOST` guards in firmware may replace a host-incompatible CALL
> mid-function (not just early-return-in-HW-helper), provided: (a) the call is
> genuinely host-incompatible (HAL-typed symbol or HAL header); (b) the host
> branch produces a deterministic, benign substitute; (c) the Arm path keeps
> the real body verbatim inside `#ifndef PFM3_HOST`.** This extends the §b.4
> carve-out ("early-return stubs in HW-touching helpers") to call sites inside
> larger functions (e.g. `Synth::buildNewSampleBlock`'s RNG call) — same
> spirit (replace the CALL, not business logic; Arm path unchanged), broader
> shape. Flagged here so future targets inherit the corrected picture.

### Latent firmware quirks characterized as golden (NOT fixed)

- **`controlChange` CC_OMNI_ON / CC_OMNI_OFF channel match lacks the routing
  path's `-1`.** `midiEventReceived`'s note-routing channel match is
  `(instrumentState_[timbre].midiChannel - 1) == midiEvent.channel`; but
  `controlChange`'s CC_OMNI_ON/OFF match is `instrumentState_[timbre].
  midiChannel == midiEvent.channel` (NO `-1`). For a timbre listening on MIDI
  channel 1 (midiChannel=1, event channel 0), CC_OMNI_ON on event channel 0
  evaluates `1 == 0` → false and is silently DROPPED — even though NoteOns on
  the same channel route correctly. Locked by
  `MidiDecoderRouting.CcOmniOnChannelMatchUsesMidiChannelDirectlyNotMinusOne`
  so a future fix (or a regression that spreads the inconsistency) is visible.
  Flagged for a separate firmware change.
- **Status byte mid-message is treated as DATA, not a re-anchor.** While
  `MIDI_EVENT_IN_PROGRESS`, `newByte` calls `newMessageData(byte)` for ALL
  bytes (including status bytes >= 0x80); the decoder does NOT re-anchor on a
  status byte mid-message. (Spec-compliant MIDI re-anchors.) Locked by
  `MidiDecoderRouting.MalformedByteStreamsDoNotCrashOrCorrupt` step 4.

### Minimal-SynthState scaled up (Target #3 precedent, wider field set)

Target #3's `Osc`-only minimal-SynthState (memset + one `tuning_` patch)
scales to MidiDecoder, which dereferences a much wider field set:
`fullState.synthMode`, `fullState.midiConfigValue[MIDICONFIG_*]`,
`mixerState.globalChannel_/currentChannel_/MPE_inst1_/userCC_[]`,
`mixerState.instrumentState_[t].{midiChannel,firstNote,lastNote,shiftNote,
numberOfVoices,scaleFrequencies}` (scaleFrequencies is a `float*` — the
fixture owns the backing tables and points each `instrumentState_[t]` at one),
and `params` (pointed at `synth.getTimbre(0)->getParamRaw()` after
`Synth::init` populates it). No virtual is dispatched through the resulting
SynthState pointer (MidiDecoder and Synth::noteOn read plain data members), so
UBSAN's vptr check does not fire. Object lifetime is technically not begun —
documented as a scoped, justified deviation, same stance as Target #3.

### Test-only private-state access — `#define private public`, scoped

MidiDecoder's interesting decode state (`currentEventState.eventState`,
`currentEvent.eventType/channel/value[]`, `currentNrpn[]`, `runningStatus`,
`omniOn[]`, `bankNumber[]`, `songPosition`, `midiClockCpt`) is `private`. The
refined SEAM rule forbids `PFM3_HOST` in firmware headers for anything but
genuinely host-incompatible constructs, so a `friend` test hook in
`MidiDecoder.h` is NOT allowed. Instead the test TU uses the standard,
contained C++ pattern `#define private public` scoped AROUND the
`MidiDecoder.h` include only: every firmware header `MidiDecoder.h` reaches is
pre-included first (`Synth.h` covers the SynthState/Timbre/Voice/... closure;
`RingBuffer.h` is pre-included for its private template data), so the macro
affects ONLY the MidiDecoder class body. Zero firmware surface; no runtime
cost; no ODR impact (access specifiers do not change class layout, and the
pre-inclusion ensures every shared header is parsed identically across TUs).
This is the same stance Target #2 took with `using Hexter::<protected-member>`
— the only difference is `private` vs `protected` access, which `using`
cannot cross.

### Actual MidiDecoder-seam surface (replaces the summary-table row)

| File | Change | Why |
| --- | --- | --- |
| `firmware/Src/midi/MidiDecoder.cpp` | `#ifndef PFM3_HOST` around the `extern "C" #include "usbd_midi.h"`; same around the 2 HAL-typed externs (`hUsbDeviceFS`, `huart1`); `#ifdef PFM3_HOST … return; #else <real> #endif` early-return stubs in `sendMidiDin5Out` + `sendMidiUsbOut` | `usbd_midi.h` header pulls HAL; the 2 externs are HAL-typed; the 2 helpers make HW calls (§b.4, applied verbatim) |
| `firmware/Src/synth/Synth.cpp` | `#ifndef PFM3_HOST` around `#include "stm32h7xx_hal.h"` + `extern RNG_HandleTypeDef hrng;`; mid-function gate around only the `HAL_RNG_GenerateRandomNumber` acquisition (host: `random32bit = 0` deterministic seed), with the downstream `noise[]` fill loop shared Arm/host; `#ifndef PFM3_HOST` around `HAL_Delay(5)`; `#ifdef PFM3_HOST extern uint32_t SystemCoreClock; #endif` | HAL header + HAL-typed extern + 2 HW calls + CMSIS var decl |
| `firmware/Src/synth/FxBus.cpp` | leading-attribute pattern (`#ifndef PFM3_HOST __attribute__((section("…"))) #endif`) on all 14 `.ram_d1/.ram_d2/.ram_d2b` static-data-member definitions | Mach-O section-attr hard error (Correction 2 forecast) |
| `firmware/Src/synth/Lfo.cpp` | same pattern on `Lfo::invTab[2048]` (`.ram_d1`) | Mach-O section-attr hard error |
| `firmware/Src/synth/Timbre.cpp` | same pattern on `midiNoteScale` (`.ram_d1`) + `Timbre::delayBuffer` (`.ram_d2b`); `#ifdef PFM3_HOST #include <cstddef> #endif` for `NULL` | Mach-O section-attr hard error + NULL undef |
| `tests/CMakeLists.txt` | `target_sources` += `MidiDecoder.cpp` + Synth graph (11 TUs) + NoteStack/EventScheduler/SimpleComp/SimpleEnvelope + new stub | decode + routing link graph |
| `tests/stubs/sequencer_collaborators_stub.cpp` | **pruned** — removed 8 `Synth::*` + 1 `Timbre::midiClockSongPositionStep` stubs (now provided by the real Synth.cpp/Timbre.cpp); kept 4 `FMDisplaySequencer::*` stubs (FMDisplay family still not pulled) | duplicate-symbol avoidance |
| `tests/stubs/midi_decoder_collaborators_stub.cpp` | NEW: `SystemCoreClock` definition; 4 `SynthState::*` non-virtual method stubs; `allParameterRows` zero-init stub (FAVOR-REAL-DATA exception) | link satisfaction for Synth graph + decodeNrpn safety |
| `tests/midi_decoder_test.cpp` | NEW: 25 ctest entries (1 smoke + 7 Tier-1 decode + 8 Tier-2 NRPN + 9 Tier-3 routing), scoped `#define private public` for MidiDecoder privates, minimal SynthState fixture (memset + field patches), `asyncActions` extern (C++ linkage — NOT `extern "C"`) for SEND_PATCH_AS_NRPN observation | coverage |

### Headline result

- **Host build: 72/72 ctest entries pass** (47 carried over + 25 new). Includes
  the 1 smoke + 7 Tier-1 decode-state-machine + 8 Tier-2 NRPN + 9 Tier-3
  routing-through-real-Synth tests.
- **ASAN+UBSAN build: 72/72 pass.** Malformed-input robustness (Target #2
  stance) verified: truncated NoteOns, all-data-byte streams, oversized sysex,
  and the CC_OMNI_ON/NRPN increment paths all complete cleanly under the
  sanitizer.
- **`arm-none-eabi` cross-build: links clean** (`preenfm3.elf` produced). All
  firmware guards are inert under the Arm build (the original code paths are
  preserved verbatim inside `#ifndef PFM3_HOST`; the section-attr leading-form
  rewrite is semantically identical to the trailing form).

All four `tests/README.md` roadmap rows are now covered.
