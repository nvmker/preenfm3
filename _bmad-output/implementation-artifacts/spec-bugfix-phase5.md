---
title: 'Firmware bug-fix Phase 5 — cosmetic, dormant, file-format oddities (+2 folded review findings)'
type: 'bugfix'
created: '2026-08-22'
status: 'done' # draft | ready-for-dev | in-progress | in-review | done
review_loop_iteration: 0
baseline_commit: '317313b'
context: ['{project-root}/_bmad-output/planning-artifacts/firmware-bug-fix-plan.md', '{project-root}/_bmad-output/implementation-artifacts/deferred-work.md']
---

<frozen-after-approval reason="human-owned intent — do not modify unless human renegotiates">

## Intent

**Problem:** Phase 5 of the firmware bug-fix plan: cosmetic/dormant/file-format defects that cost bytes, tidiness, or trap maintainers — plus two folded deferred-review findings (SequenceBank unchecked payload reads; non-finite step-seq BPM cast). Item 5.3 (PRESET_VERSION2) already landed in PR #29. **Investigation correction:** plan item 5.1's file-size claim is wrong — 12 ring flushes × 6400 px = 76 800 px = exactly 240×320; the pinned test (`FirstSaveWritesHeaderAndRgbBody`) verifies every pixel lands correctly. The streaming ring is correct; "flush once at end" would truncate the file to 1/12. 5.1 is therefore scoped to ctor init + low-bit replication only.

**Approach:** One bug per commit, risk ascending, on `fix/bugfix-phase5` off master `317313b`. No synth-render-path changes for finite valid input → no golden regen expected (run to confirm null diff). Plan-doc status update rides along as its own docs commit.

## Boundaries & Constraints

**Always:** One bug per commit. Every flipped/renamed test asserts fixed behavior + a regression case for the fixed path. `make test` green, `make test-asan` clean, `make test-cov` TOTAL ≥ 89, `make firmware` links. 5.2 pads `~` only into `[len, 12)` with `name[12] = 0`. 5.7 rewrite is byte-identical on disk (pinned by `SavePinsAllSixtyVersionOneRecordsAndPadding`). Folded-A rejects short/failed payload reads via `byteRead == n && FR_OK` and an `f_size` pre-check so a truncated file never mutates sequencer state; name loads return `"##"` on failure (existing convention). Folded-B treats non-finite BPM exactly like the 4.5 unmatched fallback (TIME_4 arm) — finite BPMs byte-identical.

**Ask First:** Any change beyond the nine items below; any golden diff; touching `PPMIMAGE_ENABLE`/flush cadence; new persistence semantics beyond stated contracts.

**Never:** No MONO-stack channel/range invalidation (deferred). No pre-4.8 mixer bank migration (owner decision, deferred). No parser relaxation in 5.4 (would revive the OOB `iPos+1` read). No flush-cadence change in PPM. No coverage-floor moves.

## I/O & Edge-Case Matrix

| Scenario | Input / State | Expected Behavior | Error Handling |
|----------|--------------|-------------------|----------------|
| Dotless 8.3 name | `plainname` (no dot) | `name` = "plainname~~~" + NUL at [12] | N/A |
| Read-only dotless | `_readonly` | "_readonly~~~" + NUL, `FILE_READ_ONLY` | N/A |
| PPM color expand | RGB565 0x1F/0x3F channels | 0xFF/0xFF after replication (31→`<<3\|>>2`, 63→`<<2\|>>4`) | N/A |
| PPM stack instance | ctor-run (no BSS) | `isInitialized == false` after ctor; lazy init runs | N/A |
| Env-curve txt ≠64 samples | `numberOfSample != 64` | parser error path (unchanged); dead branch gone | existing `numberOfSampleError` |
| Corrupt patch name slot | V1 load on host | `offsetof`-based name offset, bit-identical | N/A |
| SD write fails mid-`createSequenceFile` | `f_write` fails/0 bytes | loop breaks — no infinite spin (`HAL_Delay` hang) | bail, file stays short |
| Truncated .seq payload | `f_size` < 4+1024+16384+12336 (v1) / 4+1024+16384+24576 (v2) | rejected before `setFullState`; state unchanged | load aborts |
| Short payload read | `byteRead != n` mid-load | abort load, state unchanged / name = `"##"` | N/A |
| NaN/±Inf step-seq BPM | corrupt patch | TIME_4 arm (same as 246–255) — no UB cast | fail-safe fallback |
| uint16 persistence walk | save/load controller file | memcpy-based access, on-disk bytes identical | N/A |

</frozen-after-approval>

## Code Map

- `firmware/Src/filesystem/PreenFMFileType.cpp` — 5.2: 13-byte copy loop `:212-219`; `char name[13]` (`PreenFMFileType.h:79`)
- `firmware/Src/filesystem/PPMImage.cpp` — 5.1: ctor `:38` sets `cptImage` only; expansion shifts `:111-113`; test pins flush cadence as correct
- `firmware/Src/filesystem/UserEnvCurve.cpp` — 5.4: dead branch `:77-79`; member `interpolate` `:222` has no other callers (header decl `UserEnvCurve.h:40`); parser reject at `:155`
- `firmware/Src/filesystem/PatchBank.cpp` — 5.5: `#ifndef PFM3_HOST` cast `:119-124`; `presetName[13]` fixed array (`synth/Common.h:607`)
- `firmware/Src/filesystem/SequenceBank.cpp` — 5.6: zero-write loop `:277-283`; folded-A: ignored `f_read`s `:160-164` (v1), `:176-180` (v2), `:203/:215` (names); `"##"` `:231`
- `firmware/Src/MidiController/MidiControllerFile.cpp` — 5.7: `uint16_t*` walks `:105` (load), `:178` (save) — only two left in `firmware/Src`
- `firmware/Src/synth/LfoStepSeq.cpp` — folded-B: `switch ((int)seqParams->bpm)` `:34`; 4.5 default arm `~:80`
- Tests: `tests/preenfm_file_type_test.cpp:116` (`DotlessTstNamesGetTildePadding` — flip), `tests/ppm_image_test.cpp` (`BitExpansionShiftsWithoutLowBitReplication` flip; `FirstSaveWritesHeaderAndRgbBody` expectations re-derived; drop BSS workaround), `tests/user_env_curve_test.cpp`, `tests/patch_bank_test.cpp`, `tests/sequence_bank_test.cpp:221/:251` (mirror for payload), `tests/lfo_step_seq_test.cpp:266-273` (mirror for NaN/Inf), `tests/midi_controller_file_test.cpp:99/:145` (byte pins must stay green)
- Shim: `tests/host_shims/fatfs_impl.cpp` `fatfsShimFailNext` (f_read/f_write one-shot) + `f_size` support check

## Tasks & Acceptance

**Execution:**
- [x] `firmware-bug-fix-plan.md` — docs commit: Phase 4 ✅ COMPLETE (PR #29), Phase 5 in-progress note; 5.1 correction recorded — RESULT: `3fa809c`; note: `_bmad-output` is gitignored, so the plan doc was force-added (`git add -f`) to honor the docs-commit contract
- [x] `PreenFMFileType.cpp` + `preenfm_file_type_test.cpp` — pad `~` to k<12, `name[12]=0`; flip `DotlessTstNamesGetTildePadding` — 5.2 — RESULT: `0f1e8e1`; loop bounded to k<12 + `name[12]=0`, '_'/'.'/beforePoint logic untouched; test flipped to NUL-terminated expectations + 12-char full-length regression; 573/573
- [x] `LfoStepSeq.cpp` + `lfo_step_seq_test.cpp` — reject non-finite `bpm` before the cast → TIME_4 arm; NaN/±Inf regression test — folded-B — RESULT: `5b949bc`; `__builtin_isfinite` guard maps NaN/±Inf to TIME_4 before the cast (matching the Synth.cpp 3.10 idiom), default arm's `(int)bpm` reuse replaced with the guarded int; finite BPMs byte-identical; NaN/±Inf test mirrors 246-255; 574/574
- [x] `UserEnvCurve.cpp/.h` + `user_env_curve_test.cpp` — remove dead `3 < n < 64` branch + now-unused `interpolate` member — 5.4 — RESULT: `01343dd`; dead branch replaced with an explanatory comment, `interpolate()` member + header decl removed, quirk test `InterpolateReadsOnePastPopulatedSourceQuirk` deleted (its only subject was the removed member) with comment noting why; !=64 rejection tests green; 573/573
- [x] `PatchBank.cpp` + `patch_bank_test.cpp` — single `(char*)`-subtraction (or `offsetof`) form, drop `#ifndef PFM3_HOST` shim; V1 tests stay green — 5.5 — RESULT: host-arm `(char*)` subtraction kept as the single portable form; unrelated `.ram_d2b` section `#ifndef PFM3_HOST` at :20 left as-is (not dead); 573/573
- [x] `SequenceBank.cpp` + `sequence_bank_test.cpp` — bound the zero-fill loop (break on `FR != FR_OK || byteWritten != toWrite`) — 5.6 — RESULT: loop breaks on write failure/short write, `HAL_Delay` placement unchanged under `#ifndef PFM3_HOST`; `CreateSequenceFileReturnsAfterWriteStall` regression via one-shot `fatfsShimFailNext("f_write", FR_INT_ERR)`; 574/574
- [x] `SequenceBank.cpp` + `sequence_bank_test.cpp` — folded-A: `f_size` pre-check + exact `byteRead` validation for v1/v2 payload + name reads; state-unchanged and `"##"` regressions via `fatfsShimFailNext("f_read", …)` + truncated-file fixtures — RESULT: per-slot totals verified from code (v1 4+1024+16384+12336, v2 4+1024+16384+24576, name 4+20); f_size check requires the full slot for the requested index (4 + slotSize*(patchNumber+1)); every payload f_read requires FR_OK && byteRead == n with abort before setFullState; name arms fall through to the shared close + `"##"` return; 4 regressions added; 578/578
- [x] `MidiController/MidiControllerFile.cpp` + `midi_controller_file_test.cpp` — memcpy-based uint16 access in both walks; byte pins + round-trip stay green — 5.7 — RESULT: both load/save walks rewritten char*-only via `rdU16(const char*&)`/`wrU16(char*&, uint16_t)` memcpy helpers (anonymous namespace); stepping semantics preserved exactly (6 name bytes + 2 padding + 6 u16 fields per 20-byte record — original `p += 4` u16-step was 8 bytes from record start since the name loop never advanced `p`); save size now plain char* subtraction; grep confirmed these were the only `uint16_t*`/`unsigned short*` casts over char buffers in `firmware/Src` (Sequencer.cpp hit is a comment); byte pins + round-trip green unchanged; 578/578
- [x] `PPMImage.cpp` + `ppm_image_test.cpp` — ctor sets `isInitialized = false`; low-bit replication in R/G/B expansion; flip `BitExpansionShiftsWithoutLowBitReplication`, fix stale quirk comments, drop the placement-new BSS workaround — 5.1 — RESULT: ctor adds `isInitialized = false` (every member now ctor-initialized: cptImage, imageTitle, isInitialized; sharpIndexInName set by init() before use) so the placement-new BSS workaround was dropped for plain stack instances (`<new>` include removed); expansion replicates low bits (R/B `v<<3|v>>2`, G `v<<2|v>>4`) with the 12-flush ring cadence untouched; helpers re-derived, test renamed `BitExpansionReplicatesLowBits` asserting 31→0xFF / 63→0xFF; quirk comments reworded — ring flush documented as intentional streaming, 15+12×6400×3 byte-count assertion kept; 578/578

**Acceptance Criteria:**
- Given a dotless 8.3 name, when listed, then `name` is NUL-terminated at [12] with `~` padding only in `[len,12)`
- Given a truncated/corrupt `.seq` file, when loaded, then sequencer state is unchanged (or name = `"##"`) and no infinite loop or partial mutation occurs
- Given NaN/±Inf `bpm`, when `midiClock` runs, then behavior matches the TIME_4 fallback with no UB cast
- Given the controller save/load round-trip, then on-disk bytes are identical pre/post 5.7 rewrite

### Review Findings

- [x] [Review][Patch] Folded-B guard misses finite-but-out-of-int-range bpm (1e30f): `(int)bpm` is still UB — extend the guard to the [0,255] enum domain (all in-range finite behavior unchanged) + ±1e30 regression cases [`firmware/Src/synth/LfoStepSeq.cpp:43`]
- [x] [Review][Patch] `f_lseek` result ignored in `loadSequenceDataVersion1/2` — seek failure reads wrong-slot bytes and mutates state; check result like the name path does + injected-seek-failure regression [`firmware/Src/filesystem/SequenceBank.cpp:160/:176`]
- [x] [Review][Patch] `setFullState` runs after read 1 of 3 — mid-load failure on actions/stepNotes aborts AFTER core-state mutation; reorder: actions → stepNotes → staged 1024 read → `setFullState` last, so any single-read failure precedes mutation; per-read failure injection tests [`firmware/Src/filesystem/SequenceBank.cpp`]
- [x] [Review][Patch] Name-load `f_size` pre-check over-strict (demands full slot payload; name needs only `4 + N*slotSize + 20`) — a name-readable slot in a mid-slot-truncated bank regressed to `"##"`; relax to the name's own extent + truncated-tail test [`firmware/Src/filesystem/SequenceBank.cpp:203/:215`]
- [x] [Review][Patch] ASAN gate caught a pre-existing 1-byte-over-read: the fixed 12-byte copy of `getSequenceNameInBuffer`'s result ran past the 3-byte `"##"` fallback literal — latent at baseline, first exercised by this phase's truncated-payload tests; NUL-aware copy in both arms (real 12-char space-padded names identical) [`firmware/Src/filesystem/SequenceBank.cpp:246/:268`] — `738f6cc`
- [x] [Review][Defer] `synth/Common.h:707` declares `strcmp` with C++ linkage, so no file in its include chain can include `<string.h>` — each new libc string user must hand-declare `extern "C"` (strnlen in PreenFMFileType, memcpy in MidiControllerFile this phase) — deferred
- Rejected after verification: PPM expectation-helper mirrors implementation (pre-existing idiom; literal constants asserted in the flip test); 5.2 k<12 truncation (strictly safer for 8.3 AND LFN — NUL-termination was the task); `__builtin_isfinite` non-standard (spec-sanctioned, matches the 3.10 `__builtin_lroundf` idiom); 5.6 partial bank file left on disk (folded-A's f_size check rejects it on load — fail-safe by construction in this phase); SCOPED_TRACE-with-NaN + TIME_4-equivalence-via-neighbor-test cosmetics (mirrors the 246–255 test shape by design).

## Spec Change Log

## Design Notes

- **5.1 mischaracterization (verified):** 12 × 6400 = 76 800 px = the full 240×320 frame; `FirstSaveWritesHeaderAndRgbBody` already proves block-correct layout. Fix ONLY init + replication. Flush cadence is a valid streaming design (19.2 KB chunk vs 230 KB buffer). `#define PPMIMAGE_ENABLE 0` + `#ifdef` = always-true guard; leave as-is (Ask First to change).
- **5.4 removal choice:** parser `:155` rejects `!= 64`, so `numberOfSample ∈ {0, 64}` at the branch — `3 < n < 64` unreachable. Removing (not relaxing) keeps the OOB `iPos+1` path dead and the bin-cache format frozen.
- **Folded-A atomicity:** the 1024-B state goes through `storageBuffer` staging; an `f_size` pre-check (4 + payload, per version) rejects truncation *before* `setFullState`; mid-read I/O errors abort via `byteRead != n`. `actions`/`stepNotes` direct reads follow the same exact-length contract.
- **5.7 endianness:** memcpy of `uint16_t` on both ARM target and little-endian hosts = current byte order; `SavePinsAllSixty…` is the guard.
- **Commit order** = task order (docs → lowest risk → 5.7 → 5.1 last: only user-visible byte change).

## Verification

**Commands:**
- `make test` — expected: 100% pass; flipped tests assert fixed behavior
- `make test-asan` — expected: clean incl. new hostile/truncated inputs
- `make test-cov` — expected: TOTAL ≥ 89 (5.4 removal should lift)
- `make firmware` — expected: links
- `make golden-regen` — expected: null diff (no render-path change for valid input); any diff = HALT + Ask First

**Manual checks:** none (PPM output verified via shim-extracted bytes in gtest).

## Suggested Review Order

**SequenceBank validation & durability (5.6, folded-A + 3 review patches)**

- Entry point: f_size pre-check rejects truncation before any mutation; checked seek; setFullState LAST
  [`SequenceBank.cpp:152`](../../firmware/Src/filesystem/SequenceBank.cpp#L152)

- Bounded zero-fill loop: write stalls break instead of spinning
  [`SequenceBank.cpp:333`](../../firmware/Src/filesystem/SequenceBank.cpp#L333)

- Name extent = its own 20 bytes, not the whole slot (review patch 4)
  [`SequenceBank.cpp:239`](../../firmware/Src/filesystem/SequenceBank.cpp#L239)

- NUL-aware copies bound the `"##"` fallback literal (ASAN-surfaced, `738f6cc`)
  [`SequenceBank.cpp:281`](../../firmware/Src/filesystem/SequenceBank.cpp#L281)

**Corrupt-input guards (folded-B + review patch 1)**

- bpm domain guard [0,255]: finite out-of-range and NaN/Inf both fail safe to TIME_4
  [`LfoStepSeq.cpp:45`](../../firmware/Src/synth/LfoStepSeq.cpp#L45)

**Persistence byte-exactness (5.5, 5.7)**

- offsetof-style offset, host/target single form, shim removed
  [`PatchBank.cpp:121`](../../firmware/Src/filesystem/PatchBank.cpp#L121)

- memcpy-based uint16 walk replaces the type-punning casts (on-disk identical)
  [`MidiControllerFile.cpp:37`](../../firmware/Src/MidiController/MidiControllerFile.cpp#L37)

- extern-C memcpy declaration forced by Common.h's C++-linkage strcmp (deferred)
  [`MidiControllerFile.cpp:25`](../../firmware/Src/MidiController/MidiControllerFile.cpp#L25)

**Filesystem hygiene (5.1, 5.2, 5.4)**

- Ring cadence untouched; ctor inits the flag; low bits replicated 31→0xFF
  [`PPMImage.cpp:117`](../../firmware/Src/filesystem/PPMImage.cpp#L117)

- Tilde padding bounded to k<12 with explicit NUL at name[12]
  [`PreenFMFileType.cpp:211`](../../firmware/Src/filesystem/PreenFMFileType.cpp#L211)

- Dead interpolate branch removed; parser contract unchanged
  [`UserEnvCurve.cpp:77`](../../firmware/Src/filesystem/UserEnvCurve.cpp#L77)

**Peripherals**

- Tests: sequence-bank failure-injection suite, bpm fallback cases, PPM replication flip
  [`sequence_bank_test.cpp:221`](../../tests/sequence_bank_test.cpp#L221)

- Plan doc: phase 4 marked complete, 5.1 mischaracterization corrected
  [`firmware-bug-fix-plan.md:1`](../planning-artifacts/firmware-bug-fix-plan.md#L1)
