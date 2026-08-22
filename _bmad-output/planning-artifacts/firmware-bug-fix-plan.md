# Firmware Bug-Fix Plan — burning down the coverage-surfaced defects

**Date:** 2026-08-19
**Scope:** firmware behavior fixes in `firmware/Src` for defects surfaced (and characterized, never fixed) by the test-coverage program, Phases 1–5
**Status:** Phase 1 ✅ COMPLETE — merged as PR #26 (`fix/bugfix-phase1`). Phase 2 ✅ COMPLETE — merged as PR #27 (`fix/bugfix-phase2`). Phase 3 ✅ COMPLETE — merged as PR #28. Phase 4 ✅ COMPLETE — merged as PR #29 (`fix/bugfix-phase4`, 13 commits, suite 570/570, ASAN clean, coverage 89.39%, golden regen null diff). Phase 5 **in progress** on `fix/bugfix-phase5`. This document IS the approval vehicle: it proposes the triage, order, and golden-flip contract for every fix.
**Source inventory:** `_bmad-output/implementation-artifacts/deferred-work.md` (~30 firmware-defect entries from Phases 1–5 + their code reviews)
**Baseline (re-verified post-Phase-3, on `fix/bugfix-phase3` @ `ffe5eb3`, pre-merge):** 563/563 tests, 89.46% line coverage (floor 89 — holds, no ratchet needed), `make test-asan` clean, `make firmware` links

---

## Why these exist / the fix contract

The coverage program ran under an explicit **characterization-only** boundary: firmware
behavior changes need owner approval. Every defect below is therefore *pinned* by a test
that asserts the **buggy** behavior (names ending in `…Quirk` or describing the wrong
outcome). That changes the economics of fixing:

> **Fix contract (applies to every item):** one firmware edit + one test flip
> (characterization expectation → correct expectation, rename to drop the Quirk suffix)
> + regression cases for the fixed path. The pinned golden makes each fix verifiable;
> CI already runs the suite, the ASAN legs, and the coverage floor.

Rules inherited from the coverage program, still binding:

- **No behavior change rides along.** One bug per commit. If a fix changes rendered
  audio, goldens regenerate as their own reviewed commit (see Phase 4).
- Coverage floor does not move down; ratchet only if a phase meaningfully lifts it.
- Every phase is independently mergeable (branch `fix/bugfix-phaseN`, one PR, like the
  coverage phases). Phases are ordered by severity, not dependency — **no cross-phase
  dependencies exist**.

### Triage axes

Severity was scored on three axes, in priority order:

1. **User impact** — data loss / corruption of user files > audible wrong sound > latent crash > cosmetic
2. **Reachability** — triggerable by normal use (import a file, send a CC) > needs corrupt/hostile input > dormant code
3. **Fix risk** — does the fix change file formats, rendered audio, or a documented quirk someone may rely on?

---

## Phase overview

| Phase | Theme | Items | Est. | Audio-path golden impact |
|---|---|---|---|---|
| **1** | Data corruption & user-file loss | 5 | ✅ done (PR #26) | none |
| **2** | Memory safety — OOB / uninit reads reachable from user input | 7 | ✅ done (PR #27) | none |
| **3** | Undefined & invalid-behavior hygiene | 9 | ~1–1.5 days | none |
| **4** | Sequencer / synth logic (incl. 3 owner decisions) | 8 | ~2 days | **yes — golden regen needed** |
| **5** | Cosmetic, dormant, file-format oddities | 7 | ~1 day | PPM item only (not in goldens) |

---

## Phase 1 — Data corruption & user-file loss (most critical) 🔴 — ✅ COMPLETE (PR #26)

> **Landed** on `fix/bugfix-phase1`, merged 2026-08-19: 1.1 `c8bf4f8`, 1.2 `ca2bd31`,
> 1.3 `6eaca6a` + hardening `3af63d5`, 1.4 `2e86772` + rotate-through-backup `3023193`,
> 1.5 `426b7b8`; plus review fixes `233f1a1`/`2a14b8b`. Suite grew 524 → 539 tests.
> Review of the phase itself filed 3 new deferred-work entries (see Phase 3 items
> 3.8/3.9 and the exclusions note) — **3.8/3.9 landed in Phase 3 (PR #28).**

Every item here either destroys user data or pushes non-finite garbage into the audio
path, and each is reachable through *normal* device use (import a file, save a config,
send a CC).

| # | Bug | Location | Fix | Golden to flip | Risk |
|---|---|---|---|---|---|
| 1.1 | **`CC_UNISON_SPREAD` missing `break`** — any CC14 with value>0 that sets unison spread ALSO starts the step sequencer (fall-through into `CC_SEQ_START_ALL`) | `midi/MidiDecoder.cpp` (controlChange CC table) | add the `break` | `UnisonSpreadFallsThroughToSeqStartAll` → rename/flip + add pure-spread CC test | lowest — one line |
| 1.2 | **`ScalaFile` ÷0 on truncated `.scl`** — declares N degrees, fewer lines ⇒ `interval[N-1]` stays 0.0f ⇒ octaveRatio 0 ⇒ notes below middle C become **+inf**, in-octave notes collapse to 0 | `filesystem/ScalaFile.cpp` (`applyScalaScale`) | validate parsed-degree count vs declared; on mismatch reject the file (existing error path), never divide by 0 | `TruncatedFileDividesByZeroIntervalQuirk` → flip to rejects-file | low |
| 1.3 | **`UserEnvCurve::normalize` inverted condition** — `m = (max-min)==0 ? 1/(max-min) : 1` (backwards): all-zero curve → every sample **NaN** (clamps miss NaN); non-flat curve never scaled into 0..1; min seeded to 0 not `buffer[0]` so constant 0.5 curve untouched | `filesystem/UserEnvCurve.cpp` (`normalize`) | swap the ternary arms to `m = (max-min) != 0 ? 1/(max-min) : 1`; seed `min = buffer[0]` | `AllZeroCurveNormalizesToNaNQuirk`, `FlatNonZeroCurveIsLeftUntouchedByMinSeeding`, `NonFlatNormalizeNeverScalesQuirk` → flip all three | low-medium: changes normalization of every user curve (correct math, but different values than what users' current devices computed) |
| 1.4 | **`MidiControllerFile::saveConfig` deletes before knowing the save succeeds** — `remove()` then `save()` with no failure check; an I/O failure destroys the last valid config | `MidiController/MidiControllerFile.cpp` | save to temp name, verify, then remove old + rename; or save-first-then-remove | `midi_controller_file_test.cpp` overwrite/round-trip tests extend with failure-injection (shim `f_write` fail) | medium: ordering semantics; keep file name stable |
| 1.5 | **`MixerBank::saveDefaultMixer` rewrites without truncation** — `FA_OPEN_ALWAYS\|FA_WRITE` + lseek(0): a previously longer `mix.dfl` keeps its stale tail | `filesystem/MixerBank.cpp` | use `FA_CREATE_ALWAYS` (or explicit truncate) | `SaveDefaultMixerRewritesInPlaceWithoutTruncation` → flip | low |

**Acceptance:** each flipped test asserts the *fixed* behavior; new failure-injection
test for 1.4; `make test` green; `make test-asan` clean; `make firmware` links.

**How to verify:**

    make test          # all flipped tests pass with corrected expectations
    make test-asan     # clean
    make test-cov      # TOTAL ≥ 89% (floor untouched)
    make firmware      # links

---

## Phase 2 — Memory safety: OOB / uninitialized reads reachable from user input 🔴 — ✅ COMPLETE (PR #27)

> **Landed** on `fix/bugfix-phase2`, merged 2026-08-20: 2.6 `d5e1260`, 2.1 `a4bd2e4`,
> 2.3 `690f328`, 2.4 `c9f5cb7`, 2.5 `1b43506`, 2.2 `4d8c475` + clamp-before-float
> `cfcc5ba` + cppcheck gate `dbaad3e`, 2.7 `77a86cb`; plus review fixes P1–P4 `085ce75`.
> Suite grew 539 → 543 tests. Review of the phase itself filed 2 new deferred-work
> entries (SequenceBank header/version validation gaps beyond 2.6's contract;
> `Synth.cpp:388` negative `panTable` index on hostile `send > 1`) — **both folded
> into Phase 3 as items 3.10/3.11 (PR #28) and resolved there.**

All confirmed by ASAN or by inspection during Phases 2–4. Reachable from corrupt banks,
truncated files, or external MIDI — i.e., input the device does not control.

| # | Bug | Location | Fix | Golden to flip | Risk |
|---|---|---|---|---|---|
| 2.1 | **`PreenFMFileType::addEmptyFile` two OOB reads** — (a) reads `myFiles_[k].fileType` BEFORE the `k < numberOfFilesMax_` bound (full listing reads past the array; the `.ram_d2b` allocs have no padding); (b) copies 12 name bytes from caller strings that may be shorter | `filesystem/PreenFMFileType.cpp` (~670-680) | reorder bound-before-read; bounded name copy (strnlen→pad) | `AddEmptyFileWithFullListingReturnsNull` (kept, now no OOB); un-pad the test's 12-char names to prove the short-literal case | low |
| 2.2 | **`FxBus::mixAdd` indexes past `panTable[255]`** when `send > 1` — reachable from a corrupt bank or external MIDI (no clamp in restore/setters) | `synth/FxBus.cpp:549` | clamp `send` to the valid range at the boundary (and/or in the setters) | `fx_bus_test.cpp:109` OOB-path test → flip to clamped behavior | low-medium: audio-adjacent, but only the out-of-range path changes |
| 2.3 | **5-note step playback reads past `StepSeqValue::values`** — `values[3+n]` evaluated before the loop bound; populated note slots read adjacent metadata as **phantom notes** | `midi/Sequencer.cpp:414,421,426` | hoist the bound check before each `values[3+n]` read | new regression test (was inspection-only) | low |
| 2.4 | **`createNewNoteIfEmpty` copies uninitialized bytes 4–7** of a local `StepSeqValue` → phantom chord notes | `midi/Sequencer.cpp:988-994` | zero-init the local (`= {}`) or init all 8 bytes | Phase-2 characterization note → flip to defined zeros | low |
| 2.5 | **`FileSystemUtils::stof`/`getLine` don't stop at NUL** — unbounded forward scan past non-numeric bytes on a non-terminated buffer | `filesystem/FileSystemUtils.cpp` | stop at NUL in both scans (semantics for valid input unchanged) | `StofSkipsLeadingGarbageAcrossNulQuirk` → flip to stops-at-NUL | low |
| 2.6 | **`SequenceBank::isReadOnly` returns uninitialized stack garbage** when `f_open` fails | `filesystem/SequenceBank.cpp` | init `bankVersion = 0` (or return a defined false on open failure) | extend `IsReadOnlyFollowsStoredVersion` with the open-fail path (now assertable) | lowest |
| 2.7 | **`lineBuffer` extern size mismatch** — real def `char[1024]` (Storage.cpp); externs declare `[512]` (ScalaFile/Config/UserWaveform) and `[64]` (UserEnvCurve). Harmless today; a trap for any future bounds-check/memset via the wrong extern | `filesystem/*.cpp` externs | unify all externs to the real size via one shared constant/header | none (compile-time hygiene); add a `static_assert(sizeof)` guard test | lowest |

**Acceptance:** `make test-asan` clean with the *hostile* variants unmasked (un-pad test
names for 2.1, add short-buffer cases for 2.5); flipped tests assert defined behavior.

---

## Phase 3 — Undefined & invalid-behavior hygiene 🟠 — ✅ COMPLETE (PR #28)

> **Landed** on `fix/bugfix-phase3` (opened as PR #28, 2026-08-20): all 9 items in 11
> commits — 3.7 `9de973b`, 3.1 `3a87550`, 3.4 `6cf58be`, 3.2 `c4755c9`, 3.3 `58d765b`,
> 3.5 `7b33898`, 3.6 `d91bacf`, 3.8 `27e69e8`, 3.9 `c9add07` — **plus the two PR-#27-review
> deferred items folded in as planned**: 3.10 Synth.cpp:388 dry-path `panTable` clamp
> `e3b2e06`, 3.11 SequenceBank 4-byte-header requirement `5d6b57c` (also fixed a still-
> uninitialized `bankVersion` in `loadDefaultSequence`). Review follow-up `ffe5eb3`:
> the 3.10 post-cast clamp was itself UB for `(int)NaN`/`(int)∞` — re-derived with the
> PR-#27 clamp-before-cast idiom, non-finite send fails audible (index 255 — `panTable[0]`
> is zero and would mute). Suite grew 543 → 563. Shim gained `f_read`/`f_lseek`
> failure-injection hooks. Spec: `spec-bugfix-phase3.md`. Review filed 2 new deferred
> entries (scala extreme-but-finite ratio overflow in the table build; `encoderType`
> load-side asymmetry — inert today).

Defined memory, undefined or invalid behavior. Mostly input validation at persistence
and arithmetic boundaries.

| # | Bug | Location | Fix | Golden to flip | Risk |
|---|---|---|---|---|---|
| 3.1 | **`encoderDelta` signed-int overflow** — `value + delta` overflows before the min/max clamp (UB) | `MidiController/MidiControllerState.cpp` | compute in `int32_t`/saturate, then clamp | new overflow regression test | low |
| 3.2 | **Persisted MIDI channel > 15 → invalid status byte** — `0xb0 + ch` leaves CC range | `MidiController/MidiControllerState.cpp` (load + channel resolve) | validate/clamp channel at load and at use | Phase-5 characterization → flip to clamped/validated | low |
| 3.3 | **Invalid persisted `buttonType` emits CC with stale state** — only PUSH/TOGGLE change `value`, but the CC is sent unconditionally | `MidiController/MidiControllerState.cpp` | validate enum at load; skip emission for unknown types | Phase-5 characterization → flip | low |
| 3.4 | **Truncated controller file accepted** — only checks `size >= PROPERTY_FILE_SIZE`, ignores the `load` return; deserializes 60 records from < 1202 read bytes | `MidiController/MidiControllerFile.cpp` | require the actual read length before walking the buffer | Phase-5 malformed-file no-op tests extend to truncated-known-version | low |
| 3.5 | **`newParamValue` unchecked float→int conversion** — `(newValue-min)*100` outside int range before NRPN emit | `midi/MidiDecoder.cpp` (valueToSend) | clamp the scaled value to the representable range before narrowing | `NewParamValueNrpnFloatParamScalesByHundred` kept; add out-of-range case | low |
| 3.6 | **Non-finite comp input propagates NaN/Inf** through the output block | `SimpleEffect/SimpleComp.cpp` | sanitize at block entry (replace non-finite with 0 or clamp) — or document as out-of-contract | Phase-5 finite-input tests + new non-finite case | low |
| 3.7 | **BSS-reliance member initialization** — `SimpleComp` (`previousGain_`, `keydBMax_`, `gr_`, `keydBMaxCpt_`) and `MidiControllerState::resetState` (3 fields) never initialized; safe only because globals sit in BSS | `SimpleEffect/SimpleComp.cpp`, `MidiController/MidiControllerState.cpp` | explicit member init in ctor/`resetState` (value-identical on firmware: BSS zero) | none — zero-value identical; existing tests guard | lowest |
| 3.8 | **`PreenFMFileType::save` ignores a failed `f_lseek`** — seek fails ⇒ writes land at offset 0: a future `seek != 0` caller silently writes the wrong slot while the size check can still pass | `filesystem/PreenFMFileType.cpp` (`save`) | check the `f_lseek` result; zero the return on failure | new failure-injection test (shim `f_lseek` fail) | low — latent: no `seek != 0` caller today (phase-1 tmp-save uses `seek=0`) |
| 3.9 | **`ScalaFile` validates only the final octave interval** — malformed non-final intervals can still generate NaN/Inf/zero/negative note frequencies (1.2 fixed only the truncated-file ÷0 path) | `filesystem/ScalaFile.cpp` (`applyScalaScale`) | validate every parsed interval before building the frequency table; reject like the 1.2 error path | malformed non-final-interval cases (NaN/Inf/zero/negative) | low — read-side validation; file format unchanged |

**Acceptance:** `make test-asan` clean including new hostile-input cases; firmware
behavior byte-identical on-device for valid input (3.7 must not change goldens).

---

## Phase 4 — Sequencer & synth logic (incl. 3 owner decisions) 🟠 — ✅ COMPLETE (PR #29)

> **Landed** as merge `317313b` (PR #29, branch `fix/bugfix-phase4`, 13 commits): all 8
> items plus folded review findings. Suite grew 563 → 570 (570/570, 100%), ASAN clean,
> coverage 89.39% (floor 89 holds), firmware links. **Golden regen: null diff** — every
> fix was byte-identical on the valid render path (4.4's synced-LFO wrap corrected
> out-of-range modulation without changing pinned fixtures; 4.8's canonical-order and
> `lroundf` round-trip fixes kept on-disk/state bytes stable for the tested paths).
> Owner decisions resolved: A = implement MONO note-stack recall (`cd49336`), B = canonical v6 restore order (`be943ae`), C = remove the
> dead PRESET_VERSION2 patch path (`8f94a1c`, landed in Phase 5 numbering as item 5.3).
> Spec: `spec-bugfix-phase4.md`.

⚠️ **This is the phase that can change rendered audio.** Fixes here may flip
`tests/golden/` fixtures: regenerate `_macos` locally, run the
`regenerate-linux-goldens` workflow post-merge, and treat the audio diff itself as the
review artifact (a wrong fix shows up as an unexplained golden diff — that is the
safety net working).

| # | Bug | Location | Fix | Golden to flip | Risk / decision |
|---|---|---|---|---|---|
| 4.1 | **`Sequencer::clear` never reclaims memory** — the NONE-marking walk reads `nextIndex` AFTER resetting it, so the whole defrag/compaction block is dead code; bank slots leak over a session | `midi/Sequencer.cpp` (`clear`) | capture `nextIndex` before reset (restore the intended defrag) | Phase-2 `clear` characterization → flip; add capacity-reuse test | medium: resuscitates long-dead code — review the defrag carefully before trusting it |
| 4.2 | **`stepClearPart`/`stepClearAll` omit step 255** — loops stop at `< 255`; final step keeps stale data | `midi/Sequencer.cpp:628-650` | `<= 255` / `< 256` | new regression test | lowest |
| 4.3 | **FxBus deferred preset changes freeze at reverb level exactly 0** — `totalSent == 0` returns before the wait counter advances | `synth/FxBus.cpp:210` | advance the counter before/independent of the early return | `fx_bus_test.cpp:303` → flip | low; audio-path — check goldens |
| 4.4 | **LFO TIME_3/4/8 sync phase overshoot** — phase advances 2–4 while waveform code subtracts only one ⇒ out-of-range modulation | `synth/LfoOsc.cpp:103-129` | align subtract with actual advance (or wrap phase once) | `lfo_osc_test.cpp:419` → flip; **expect golden regen** (LFO in render path) | medium: audible change, but only for synced TIME_3/4/8 |
| 4.5 | **Unsupported sync BPMs (e.g. 246) match no arm** — retain zero/stale `phaseStep` | `synth/LfoStepSeq.cpp:36-88` | default arm mapping to nearest supported division | `lfo_step_seq_test.cpp:120` → flip | low-medium: choose the fallback deliberately |
| 4.6 | **Zombie `playing` flag** — voice stolen while `newNotePending` can decay to silence but keep `playing = true`; `Synth::isPlaying()` lies | `synth/Voice.cpp:592-596` | clear `playing` when the quick-release tail ends on the pending path | `StolenOldestNoteIsGoneNoteOffsOfSoundingNotesDrainAll` extended | low-medium: UI/CPU-display observable, not audio |
| 4.7 | **MONO mode has no note-stack recall** — releasing the top note leaves a still-held lower note silent. May be *intended* vintage behavior; currently undocumented either way | `synth/Timbre.cpp` (`preenNoteOff`) | **OWNER DECISION A:** document-as-intended (keep golden, add README note) OR implement stack recall (flip `MonoModeNoteOffReleasesWithoutRecall`) | `synth_core_test.cpp:244` | medium: recall changes play feel; documenting costs nothing |
| 4.8 | **Mixer save/load drift + default-bank FX permutation** — (a) 0.01f/×100 round-trip decrements 62/256 byte values per cycle (default predelay 54→53→52…); (b) `getFullDefaultState` order ≠ v6 restore order ⇒ 8/13 master-FX params get neighboring defaults on fresh boot | `synth/MixerState.cpp` (+ restore path); `MixerBank` write path | **OWNER DECISION B:** fixing (a) changes saved-file bytes — old files must still load (read-side compat kept, write-side corrected); (b) pick the canonical order (restore order) and align `getFullDefaultState` | `mixer_state_test.cpp:190` (permutation) + Phase-1 drift goldens | medium-high: file-format-adjacent; both changes alter what users hear after boot/save |

**Acceptance:** every flipped golden reviewed via `golden-regen` diff; `_macos` fixtures
regenerated locally and byte-exact expectations re-pinned; `regenerate-linux-goldens`
workflow run post-merge; `make test` 100% green; `make firmware` links; on-device spot
check for 4.4 (synced LFO) and 4.8 (mixer save/reboot round-trip).

---

## Phase 5 — Cosmetic, dormant, file-format oddities 🟡 — **ACTIVE** (`fix/bugfix-phase5`)

> 5.3 (PRESET_VERSION2) already landed in Phase 4 as `8f94a1c`. Two PR-#29 deferred
> review findings are folded in here (SequenceBank unchecked payload reads; non-finite
> step-seq BPM cast). See `spec-bugfix-phase5.md` for the frozen contract.

Real defects, but today they cost bytes, tidiness, or trap future maintainers — not
sound or data.

| # | Bug | Location | Fix | Notes |
|---|---|---|---|---|
| 5.1 | **PPMImage ctor leaves `isInitialized` uninitialized** (stack instances read garbage; only BSS globals happened to be zero) and the 5→8-bit expansion shifts without low-bit replication (0x1F→0xF8, not 0xFF) | `filesystem/PPMImage.cpp` | init the flag in the ctor; replicate low bits | **investigation correction:** the original "flushes 12×" claim was wrong — 12 ring flushes × 6400 px = 76 800 px = exactly the full 240×320 frame, and `FirstSaveWritesHeaderAndRgbBody` proves every pixel lands correctly; the streaming ring is a valid design (19.2 KB chunks vs a 230 KB buffer). "Flush once at end" would truncate the file to 1/12 — so 5.1 is scoped to ctor init + low-bit replication only; flush cadence untouched (Ask-First) |
| 5.2 | **`initFiles` tilde-padding is not NUL-terminated** — `~` fills all 13 name bytes for dotless names; later str_cmp walks read past the array | `filesystem/PreenFMFileType.cpp` | keep at least one NUL (pad to 12) | `DotlessTstNamesGetTildePadding` → flip |
| 5.3 | **PRESET_VERSION2 internal inconsistency** — V2 load memcpy's 936 bytes but `loadPatchName` reads the name at offset 992; dormant (current version is V1) | `filesystem/PatchBank.cpp` | either fix the V2 offsets consistently or delete the dead V2 path | **OWNER DECISION C:** keep V2 (fix) vs. remove (simplify) |
| 5.4 | **Dead interpolate branch** — `loadUserEnvCurves`'s `3 < n < 64` branch unreachable (txt parser rejects ≠64 first) | `filesystem/UserEnvCurve.cpp` | remove dead branch or relax the parser — pick one | coverage visibly lifts if removed |
| 5.5 | **`loadPatchName` pointer-cast to `unsigned int`** — fine on Arm, hard error on 64-bit host (host build carries an `#ifdef PFM3_HOST` workaround) | `filesystem/PatchBank.cpp` | replace with `offsetof(OneSynthParams, presetName)` (bit-identical) | removes a host shim — seam gets smaller |
| 5.6 | **`SequenceBank` save-loop can spin forever** on a host failure-injection (loop advances only via `f_write`'s byteWritten; no bound) | `filesystem/SequenceBank.cpp:261-267` | bounded retry / bail on zero-write | unblocks future failure-injection tests |
| 5.7 | **`storageBuffer` strict-aliasing walk** — `uint16_t*` over `char storageBuffer[]` in MidiControllerFile (firmware-wide persistence idiom) | `MidiController/MidiControllerFile.cpp` (+ same pattern elsewhere) | memcpy / `char*`-only walk | biggest refactor of the list; only do it with the Phase-3 validation work fresh |

**Acceptance:** `make test` green; PPM fixture (byte-exact dump golden) re-pinned to
the corrected single-flush format; coverage floor ratcheted up if 5.4 removal lands.

---

## What is deliberately NOT in this plan

- **Test-infrastructure items** (coverage-gate JSON parsing, SHA-pinning Actions, FatFs
  shim fidelity notes — now including the shim `f_rename` open-handle/`FR_LOCKED`
  fidelity gap from the PR #26 review — DWT guard completion) — hardening of the harness, not firmware
  defects. They live in `deferred-work.md` and belong to a separate hardening pass
  (see also `static-analysis-followup-plan.md`).
- **The DX7 recursive browser** — a feature (`spec-dx7-bankdir-config.md` follow-up),
  not a bug.
- **Chasing the remaining ~10% coverage** — the plan's ceiling analysis already drew
  that line (`test-coverage-plan.md`, "What is deliberately not covered").

## Suggested opening move

Phases 1–3 ✅ landed (PR #26, PR #27, PR #28) — the fix contract is proven on data,
memory, and UB classes. Next: **Phase 4** — the first audio-path phase. Read the
phase header first: golden regeneration rules, the on-device spot checks (4.4 synced
LFO, 4.8 mixer round-trip), and the **three owner decisions that gate work**: 4.7
(MONO note-stack recall vs. document-as-intended), 4.8 (mixer save/load drift —
canonical-order choice changes what users hear after boot/save), and 5.3 (Phase 5,
PRESET_VERSION2 keep-vs-remove, but decided alongside 4.x file-format work). Do NOT
start 4.x implementation before those decisions are made — the plan's contract is
owner approval for behavior changes.

Cheap warm-up if a full Phase-4 kickoff isn't wanted yet: **4.2** (`stepClearPart`/
`stepClearAll` omit step 255 — `< 255` → `<= 255`, lowest-risk item in the phase) or
**4.1** (revive the dead defrag in `Sequencer::clear` — medium risk, review the
defrag logic carefully before trusting it).

## Verification (every phase)

    make test            # all flipped/new tests green
    make test-cov        # TOTAL ≥ 89% lines (floor never drops)
    make test-asan       # clean, incl. hostile-input variants
    make firmware        # preenfm3.elf links; no test source linked
    # Phase 4 only:
    make golden-regen    # _macos fixtures; review diff BEFORE committing
    # post-merge (Phase 4): run the regenerate-linux-goldens workflow
