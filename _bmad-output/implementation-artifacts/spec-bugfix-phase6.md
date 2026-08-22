---
title: 'Firmware bug-fix Phase 6 — residual hardening: 11 ledger leftovers'
type: 'bugfix'
created: '2026-08-22'
status: 'done' # draft | ready-for-dev | in-progress | in-review | done
review_loop_iteration: 0
baseline_commit: '60fb1ce'
context: ['{project-root}/_bmad-output/planning-artifacts/firmware-bug-fix-plan.md', '{project-root}/_bmad-output/implementation-artifacts/deferred-work.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Phase 6 of the firmware bug-fix plan: 11 defects left in the deferred-work ledger after Phases 1–5 — one unvalidated mixer read (6.1), three corrupt-input OOB reads (6.2/6.3/6.4), two ignored SD write failures / fixed-length copies (6.5/6.6), unknown-mixer-version stale core (6.7), unchecked MidiController indexes (6.8), partial-MIDI-message ring risk (6.9), two BSS-reliance members (6.10), and the `Common.h` C++-linkage `strcmp` declaration (6.11). Owner decisions resolved 2026-08-22: 6.7 = reset core to defaults; 6.2/6.3 = clamp at use site; 6.8 = bounds-check at API boundary; 6.11 = C-linkage declaration, drop both workarounds.

**Approach:** One bug per commit, risk ascending, on `fix/bugfix-phase6` off master `60fb1ce`. No synth-render-path changes for valid input → `make golden-regen` null diff expected (6.4 touches only the `.bin` waveform cache, not goldens). Plan-doc status update rides along as its own docs commit.

**Investigation corrections (verified in code, differ from plan sketch):** (1) 6.3 clamps the `invTab` index to `[0, 2047]` (the table bounds), NOT `[0, 49]` — the PAD random preset (`SynthState.cpp:878`) legitimately produces `keybRamp` up to 4.0 → index 200, and the DX7 import (`Hexter.cpp:897`) up to ~6.6 → index 330; a 49-clamp would audibly change valid input. Negative index still clamps to 0, matching the existing safe `-0.01` behavior (`invTab[0]`). (2) 6.1's `f_size` pre-check requires the mixer-STATE extent (`mixerNumber * FULL_MIXER_SIZE + MIXER_SIZE`), NOT the full `FULL_MIXER_SIZE` slot — the full-slot variant would reject banks whose patch tail is truncated, breaking the pinned `ShortBankMarksTimbrePresetNameWithHashes` `##` fallback.

## Boundaries & Constraints

**Always:** One bug per commit. Every flipped/renamed test asserts fixed behavior + a regression case for the fixed path. `make test` green, `make test-asan` clean, `make test-cov` TOTAL ≥ 89, `make firmware` links. 6.1 rejects before touching state (folded-A idiom: `f_size` pre-check + `FR_OK && byteRead == MIXER_SIZE`), state unchanged on abort. 6.4 clamps the `iPos+1` read to `sourceNumberOfSamples - 1`. 6.5 checks both `f_write` results (header + 1024-byte block), bail = `f_close` + return. 6.6 uses the 2.1(b) bounded-copy idiom (strnlen→pad), `mixName_[12] = 0`. 6.7 folds the serialized-core defaults into `setDefaultValues()` (its only caller is `restoreFullState`), values byte-identical to `getFullDefaultState`. 6.8 bounds-checks at the API boundary (page < `MIDI_NUMBER_OF_PAGES`, encoder/button < their array sizes) — no-ops for valid input. 6.9 checks room for all 3 CC bytes before any insert (all-or-nothing: no state change, no partial emission). 6.11 single `extern "C"` declaration in `Common.h`, both site workarounds replaced by `#include <string.h>` (gnu++14 → `_DEFAULT_SOURCE` → newlib declares `strnlen`/`memcpy`; `make firmware` is the gate).

**Ask First:** Any change beyond the 11 items below; any golden diff (6.4 included — HALT if `make golden-regen` differs); keeping either 6.11 workaround if the Arm toolchain rejects `<string.h>`'s `strnlen`.

**Never:** No pre-4.8 mixer-bank migration (read side frozen by design). No MONO-stack channel/range invalidation. No UI-valid-input behavior change anywhere (all clamps/bounds-checks must be invisible for valid input). No coverage-floor moves. No changes to `PPMIMAGE`/flush cadence.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|----------|---------------|-------------------|----------------|
| Truncated mixer bank | `f_size` < offset+MIXER_SIZE at slot N | load aborted before `restoreFullState`; mixer state unchanged | return false, no mutation |
| Mixer state read fails | shim `f_read` fail after passing pre-check | state unchanged, patch reads skipped | bail |
| Corrupt step-seq step | raw `char` step < 0 or > 15 in preset | `expValues` index clamped to [0,15]; emitted value in table | fail-safe clamp |
| Hostile keybRamp | `keybRamp <= -0.02` (or huge float) | `invTab` index clamped to [0,2047]; valid input (up to 4.0) byte-identical | fail-safe clamp |
| UserWaveform last sample | iPos+1 == sourceNumberOfSamples | read clamped to last populated sample; earlier samples unchanged | `.bin` cache may differ (data file) |
| SD write fails in createSequenceFile | header or 1024-B block `f_write` fails/short | creation stops; file stays short; handle closed | bail |
| Short mixer name save | `mixerName` shorter than 12 chars | copy bounded by strnlen, NUL-padded, `mixName_[12]=0` | N/A |
| Unknown mixer version | full-length buffer, version ∉ 1..6 | full default state incl. serialized core ("Mix 01", 440 Hz, outs {1,1,4,4,6,8}, …) | defaults |
| Out-of-range MidiController API call | page ≥ 5 or encoder/button ≥ array size | API no-ops (no OOB write, no emission); valid calls unchanged | no-op return |
| Ring nearly full | `getCount() > 61` when a 3-byte CC fires | whole message dropped, encoder/button state unchanged | drop, no partial message |

</frozen-after-approval>

## Code Map

- `firmware/Src/filesystem/MixerBank.cpp` — 6.1: `loadMixerData` `:135-140` (ignored f_read result/byteRead, no pre-check); 6.6: `saveMixer` `:262` fixed 12-byte copy
- `firmware/Src/synth/MixerState.cpp` — 6.7: `setDefaultValues` `:219-244` (core not reset; sole caller is `restoreFullState:196`); core defaults per `getFullDefaultState:104-190`
- `firmware/Src/synth/LfoStepSeq.cpp` — 6.2: raw step reads `:150/:154` (gated/ungated `target = steps[(int)phase]`) + `noteOn :171`; table `:22` (16 entries)
- `firmware/Src/synth/LfoOsc.h` — 6.3: inline `invTab[(int)(keybRamp * 50.0f)]` `:35`; table is `Lfo::invTab[2048]` (`Lfo.cpp:29`)
- `firmware/Src/filesystem/UserWaveform.cpp` — 6.4: `interpolate` `:243-252` (`buffer[iPos+1]` one-past for last target sample)
- `firmware/Src/filesystem/SequenceBank.cpp` — 6.5: `createSequenceFile` header `:313`, 1024-B block `:329` (both ignore f_write); zero-fill loop already guarded (5.6)
- `firmware/Src/MidiController/MidiControllerState.cpp` — 6.8: `encoderDelta:70` / `buttonDown:89` / `buttonUp:107` + header getters `:82-84`; 6.9: three-insert emit paths in those three methods
- `lib/Inc/RingBuffer.h` — 6.9: add `hasRoomFor(int n)` (usable capacity = `size`; `buf[size+1]`)
- `firmware/Src/synth/FxBus.h` — 6.10: `delay2ReadPos :164`, `delay4ReadPos :180` lack `= 0`
- `firmware/Src/synth/Common.h` — 6.11: `#ifndef PFM3_HOST`-guarded C++-linkage `strcmp` `:700-708`; workarounds: `PreenFMFileType.cpp:20-22` (strnlen), `MidiControllerFile.cpp:23-25` (memcpy)
- Tests: `tests/mixer_bank_test.cpp` (6.1/6.6; `ShortBankMarksTimbrePresetNameWithHashes:186` must stay green), `tests/mixer_state_test.cpp:371` (`UnknownVersionFallsBackToDefaultsOnly` → flip), `tests/lfo_step_seq_test.cpp` (hostile-step regression; `#define private public` pattern), `tests/lfo_osc_test.cpp` (`NegativeKeybRampResyncsPhaseAndSkipsRamp` extend ≤ −0.02; hazard note `:30-34`), `tests/user_waveform_test.cpp:140` (`InterpolateReadsOnePastPopulatedSourceQuirk` → flip), `tests/sequence_bank_test.cpp:290` (write-stall pattern to mirror), `tests/midi_controller_state_test.cpp` (drains `usartBufferOut:28`)
- Shim: `tests/host_shims/fatfs.h` — `fatfsShimFailNext(Nth)`, `fatfsShimInjectBytes`, `f_size` support

## Tasks & Acceptance

**Execution:**

- [x] `firmware-bug-fix-plan.md` — docs commit: Phase 6 owner decisions recorded (6.7 reset-core, 6.2/6.3 clamp-at-use, 6.8 bounds-check, 6.11 C-linkage), status in-progress; both investigation corrections noted
- [x] `firmware/Src/synth/FxBus.h` — 6.10: member initializers `delay2ReadPos = 0`, `delay4ReadPos = 0` (3.7 idiom) — RESULT: `f7fbc36`; value-identical on firmware (BSS zero); 583/583
- [x] `firmware/Src/filesystem/SequenceBank.cpp` + `tests/sequence_bank_test.cpp` — 6.5: check header + 1024-B block `f_write` (`FR_OK && byteWritten == n`, bail with `f_close`); regressions via `fatfsShimFailNextNth("f_write", …, 1)` (header) and `..., 2` (first block) asserting exact short file sizes — RESULT: `d6162d9`; both writes bail with f_close+return (5.6 contract); regressions assert empty file / header-only / closed handles; 585/585
- [x] `firmware/Src/synth/LfoOsc.h` + `tests/lfo_osc_test.cpp` — 6.3: clamp index to [0,2047]; extend `NegativeKeybRampResyncsPhaseAndSkipsRamp` with keybRamp −0.02/−1.0 (+ inf case if cheap); update hazard note — RESULT: `1bd1ce3`; guard-before-cast (folded-B idiom — the (int) cast of NaN/Inf/huge is itself UB), then clamp [0,2047]; −0.02/−0.5/−1/−12345/−Inf assert full 'off' semantics; NaN asserts clamp without resync (NaN < 0 false — comparison semantics untouched); hazard note rewritten FIXED; 586/586
- [x] `firmware/Src/synth/LfoStepSeq.cpp` + `tests/lfo_step_seq_test.cpp` — 6.2: clamp raw step to [0,15] at the three read sites (private helper); hostile-step regression (char 100, −5 → emitted ∈ expValues[0..15]) — RESULT: `e51d95d`; private static stepValue() clamps the step VALUE (first draft wrongly clamped the phase INDEX — caught by 3 failing gate tests, fixed); regression chars 100/−5/127/−128; 587/587
- [x] `firmware/Src/filesystem/UserWaveform.cpp` + `tests/user_waveform_test.cpp` — 6.4: clamp `iPos+1`; flip `InterpolateReadsOnePastPopulatedSourceQuirk` → last sample interpolates `buf[srcN-1]` only — RESULT: `69c15b9`; flipped to InterpolateLastSampleStaysInsidePopulatedWindow with hostile tail (−7.0f) proving no leak; 587/587, ASAN clean
- [x] `firmware/Src/MidiController/MidiControllerState.cpp/.h` + `lib/Inc/RingBuffer.h` + `tests/midi_controller_state_test.cpp` — 6.8+6.9: bounds-check page/encoder/button at all four entry points (no-op out of range); `hasRoomFor(3)` gate before each 3-byte emit (all-or-nothing); out-of-range + ring-full regressions — RESULT: `6251874`; mutators no-op + getters nullptr; hasRoomFor(n) ⟺ count+n < size (ring sacrifices one slot → capacity 63 — spec design-note off-by-one corrected); buttonDown rolls back toggle flip on drop; dropped PUSH buttonUp stays pressed, returns true; 4 regressions; 591/591
- [x] `firmware/Src/filesystem/MixerBank.cpp` + `tests/mixer_bank_test.cpp` — 6.1: `f_size` pre-check (state extent) + `byteRead == MIXER_SIZE` validation in `loadMixerData`, bail before `restoreFullState`; truncated-file + injected-f_read regressions (state unchanged); 6.6: bounded `mixName_` copy in `saveMixer` (2.1(b) idiom) + short-literal regression — RESULT: `9773dfb`; pre-check = offset + MIXER_SIZE only (patch-tail ## fallback preserved via pinned test + slot>0 boundary test); f_lseek checked (folded-A symmetry); loadMixer/loadDefaultMixer now propagate failure (were unconditionally true); bounded copy without strnlen (pre-6.11 constraint); a stray null-buffer call in a new test SEGFAULTed during development — removed; unused <string> include dropped; 595/595, ASAN clean
- [x] `firmware/Src/synth/MixerState.cpp` + `tests/mixer_state_test.cpp` — 6.7: fold serialized-core defaults into `setDefaultValues()` ("Mix 01", channels 0, tuning 440, per-timbre outs/channels/notes/voices/volume per `getFullDefaultState`); flip `UnknownVersionFallsBackToDefaultsOnly` → asserts full default core; verify known-version restores unaffected — RESULT: `504d4fe`; quirk flipped to UnknownVersionResetsEverythingToDefaults (core asserted field-by-field) + new KnownVersionRestoreStillOverwritesCoreDefaults v1..v6 (PutTimbre sentinels corrected after first run); 596/596
- [x] `firmware/Src/synth/Common.h` + `PreenFMFileType.cpp` + `MidiControllerFile.cpp` — 6.11: single `extern "C" int strcmp(const char*, const char*);` (drop `PFM3_HOST` guard), replace both workarounds with `#include <string.h>`, update comments + `tests/SEAM.md` seam table — RESULT: `35f4461`; needed #ifdef __cplusplus — Common.h is included by C TUs (stm32h7xx_it.c) where extern "C" is invalid syntax (caught by make firmware); arm-none-eabi accepts <string.h>; SEAM.md correction 3 resolved; 596/596, ASAN clean, cov 89.47%, firmware links, golden-regen null diff (23/23)

**Acceptance Criteria:**

### Review Findings

- [x] [Review][Patch] LfoOsc 6.3 guard missed finite-but-huge keybRamp (1e30f): `keybRamp*50` overflows the int cast — guard now bounds the whole cast domain (`keybRamp <= 2047.0f/50.0f`) before casting; huge-positive regression cases (1e30/1e9/1e6) assert invTab[0] clamp without resync [`firmware/Src/synth/LfoOsc.h`] — `b1c559c`
- [x] [Review][Patch] buttonDown no-room ROLLBACK flipped an already-pressed PUSH button to 0 on a double-press (state changed on drop, violating all-or-nothing); restructured to check hasRoomFor(3) BEFORE any mutation, rollback block deleted; double-press regression added to NearlyFullRingDropsWholeCcMessage [`firmware/Src/MidiController/MidiControllerState.cpp`] — `b1c559c`
- [x] [Review][Patch] FxBus delay1..4ReadLen uninit siblings of 6.10 (read at FxBus.cpp:807-810 before the sizeParam block writes them) — in-class `= 0` initializers, BSS-value-identical [`firmware/Src/synth/FxBus.h:157/:165/:173/:181`] — `b1c559c`
- [x] [Review][Patch] UserWaveform interpolate `sourceNumberOfSamples <= 0` reads `buffer[-1]` via the clamped index — early return (callers pass 33..1023) [`firmware/Src/filesystem/UserWaveform.cpp`] — `b1c559c`
- [x] [Review][Patch] Magic literal 6 in the new bounds checks — named constants MIDI_NUMBER_OF_ENCODERS/MIDI_NUMBER_OF_BUTTONS [`firmware/Src/MidiController/MidiControllerState.h`] — `b1c559c`
- [x] [Review][Patch] RingBuffer::hasRoomFor comment promised a property the class does not enforce — now states the cooperative-scheduler assumption explicitly (main-loop inserts, ISR only drains) [`lib/Inc/RingBuffer.h`] — `b1c559c`
- [x] [Review][Defer] MixerState default-truth duplication: setDefaultValues + getFullDefaultState hand-maintain the same defaults in two representations — deferred (frozen 6.7 design; refactor out of scope)
- [x] [Review][Defer] MixerBank::saveMixer mutates mixName_ before f_open; failed open leaves the in-memory name changed — deferred (pre-existing ordering, 6.6 kept it deliberately)
- Rejected after verification: 6.9 check-then-act "not atomic" vs other insert sites (all inserts run in the cooperative main loop; the USART ISR only drains — no preemption window on the actual firmware model); null-check on saveMixer's mixerName (codebase has no null-check convention on string params); "include <string.h> in Common.h instead" (owner decision 3a chose the C-linkage declaration); positive-##-path test gap (covered by pinned ShortBank test + StateExtent test); fallback-direction documentation (documented in the code comment); partial createSequenceFile file left on disk (pre-ruled fail-safe by construction in Phase 5 review).

**Acceptance Criteria:**

- Given a truncated mixer bank, when loaded, then mixer state is unchanged (previous values intact) and no OOB/unvalidated read feeds `restoreFullState`
- Given an unknown mixer version byte in a full-length buffer, when restored, then the serialized core equals `getFullDefaultState` defaults (name "Mix 01", tuning 440 Hz, outs {1,1,4,4,6,8}, channels 1..6, volumes 1.0)
- Given corrupt step-seq steps or hostile `keybRamp`, when the LFO runs, then every table read is in-bounds and valid-input output is byte-identical
- Given `usartBufferOut` with ≤ 3 free slots, when a CC emission fires, then no bytes are inserted and no state changed
- Given the controller-file save/load round-trip and any TU including `<string.h>` after `Common.h`, then both compile and on-disk bytes are identical

## Spec Change Log

## Design Notes

- **6.7 mechanics:** `setDefaultValues()` has exactly one caller (`restoreFullState:196`) — folding the core reset there means known versions overwrite it immediately via their version reader (byte-identical behavior) while unknown versions keep the defaults. Default values mirror `getFullDefaultState` exactly (name "Mix 01" = its `mixNumber=1` shape) so a future default-mixer round-trip test stays coherent. `MixerBankTest::SetUp` loads a default state then sets `mixName_ = "MYMIX"` — the 6.1 state-unchanged test can lean on that fixture.
- **6.1 × 6.7 interplay:** truncation → rejected (state unchanged); full-length corrupt version → defaults. Both must be tested separately.
- **6.9 semantics:** dropping the whole CC message (state + emission) keeps value and stream consistent; `getCount() + 3 <= 64` mirrors `isFull()`'s `size`-slot usable capacity.
- **6.11 blast radius:** header-only linkage change, every TU rebuilds — zero runtime delta; CI Clang + GCC host legs plus `make firmware` cover the three compilers. `tests/SEAM.md:366-376,415` documents the workaround seam — update after landing.
- **Commit order** = task order: docs → 6.10 (trivial) → 6.5 → 6.3 → 6.2 → 6.4 → 6.8/6.9 → 6.1/6.6 → 6.7 → 6.11 last (biggest rebuild, no runtime risk).

## Verification

**Commands:**

- `make test` — expected: 100% pass; flipped tests assert fixed behavior
- `make test-asan` — expected: clean incl. new hostile-input cases
- `make test-cov` — expected: TOTAL ≥ 89
- `make firmware` — expected: links (6.11 gate: `<string.h>` after `Common.h` compiles on arm-none-eabi)
- `make golden-regen` — expected: null diff; any diff = HALT + Ask First

**Manual checks:** none (all paths shim/gtest-verified).

## Suggested Review Order

**Mixer validation & unknown-version semantics (6.1 + 6.7)**

- Entry point: f_size pre-check requires state extent only — the "##" patch-tail fallback survives
  [`MixerBank.cpp:135`](../../firmware/Src/filesystem/MixerBank.cpp#L135)

- Exact-length read validation before any state mutation; loadMixer/loadDefaultMixer now propagate failure
  [`MixerBank.cpp:145`](../../firmware/Src/filesystem/MixerBank.cpp#L145)

- Unknown version now resets the serialized core — defaults mirror getFullDefaultState ("Mix 01")
  [`MixerState.cpp:219`](../../firmware/Src/synth/MixerState.cpp#L219)

- Known versions overwrite everything: v1..v6 sentinel regression
  [`mixer_state_test.cpp:421`](../../tests/mixer_state_test.cpp#L421)

**Corrupt-input clamps (6.2 + 6.3 + 6.4)**

- Step values clamped at all three use sites; first draft wrongly clamped the index — caught by gates
  [`LfoStepSeq.cpp:160`](../../firmware/Src/synth/LfoStepSeq.cpp#L160)

- Guard bounds the whole cast domain (review patch: 1e30f keybRamp overflows the int cast)
  [`LfoOsc.h:44`](../../firmware/Src/synth/LfoOsc.h#L44)

- Interpolate upper read clamped to the populated window; hostile tail cannot leak
  [`UserWaveform.cpp:257`](../../firmware/Src/filesystem/UserWaveform.cpp#L257)

**Durability (6.5 + 6.6)**

- Header and state-block writes checked; short-file regressions pin exact sizes
  [`SequenceBank.cpp:313`](../../firmware/Src/filesystem/SequenceBank.cpp#L313)

- Bounded name copy: NUL-aware, zero-padded, mixName_[12] always terminated
  [`MixerBank.cpp:279`](../../firmware/Src/filesystem/MixerBank.cpp#L279)

**MIDI controller hardening (6.8 + 6.9)**

- Bounds checks + named constants at every entry point; getters return nullptr out of range
  [`MidiControllerState.cpp:70`](../../firmware/Src/MidiController/MidiControllerState.cpp#L70)

- All-or-nothing CC emission: check-before-mutate (review patch deleted the broken rollback)
  [`MidiControllerState.cpp:105`](../../firmware/Src/MidiController/MidiControllerState.cpp#L105)

- Ring capacity query states its cooperative-scheduler assumption
  [`RingBuffer.h:60`](../../lib/Inc/RingBuffer.h#L60)

**Header linkage & BSS hygiene (6.10 + 6.11)**

- strcmp declared extern "C" under __cplusplus — <string.h> includable again, both workarounds dropped
  [`Common.h:700`](../../firmware/Src/synth/Common.h#L700)

- delay*ReadPos AND delay*ReadLen in-class zeroed (review patch completed the sibling set)
  [`FxBus.h:165`](../../firmware/Src/synth/FxBus.h#L165)

**Peripherals**

- Review-patch regressions: huge-positive ramps, double-press drop, ring boundary
  [`lfo_osc_test.cpp:347`](../../tests/lfo_osc_test.cpp#L347) · [`midi_controller_state_test.cpp:216`](../../tests/midi_controller_state_test.cpp#L216)

- Failure-injection suite: truncated banks, unreadable state, short names
  [`mixer_bank_test.cpp:186`](../../tests/mixer_bank_test.cpp#L186)
