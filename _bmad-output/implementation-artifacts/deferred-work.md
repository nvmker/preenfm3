# Deferred Work

> **Ledger audit 2026-08-22** (post PR #30): ~40 resolved entries were purged —
> firmware defects consumed by bug-fix Phases 1–5 (plan: `planning-artifacts/firmware-bug-fix-plan.md`,
> PRs #26–#30), the G1/G3/G4 `_linux` golden fixtures (all committed; 0 `TBD_PENDING_LINUX_WORKFLOW`
> left in `tests/golden/schema.json`), and the DAC `<<8` UB (PR #16 `21c3345`).
> Two PR-#28-review entries (scala built-table validation, `encoderType` load coercion) were
> resolved as folded Phase-4 findings (`69ff175`, `7266c3b`) — verified live in code.
> Survivors are regrouped below; the 11 open firmware defects feed the proposed **Phase 6**.

## Open firmware defects — feed Phase 6 (proposed; see plan for triage)

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase1.md`
  plan: phase6 item 6.1
  summary: Truncated mixer-bank buffer feeds restoreFullStateVersion6 an unbounded read (no length param) — OOB under any size below the version layout.
  evidence: `MixerState::restoreFullState(char*)` (MixerState.cpp:192) dispatches per-version readers that index a fixed layout with no bounds check; only a full-length 216-byte buffer is safe. Reviewer: Blind Hunter (Phase 1 review). Belongs with Phase 4 filesystem-corruption guards.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase1.md`
  plan: phase6 item 6.2
  summary: Corrupt step-seq steps (char < 0 or > 15) index expValues[] out of bounds and feed the matrix an OOB global read.
  evidence: `LfoStepSeq` indexes `expValues[steps[currentStep]]` (LfoStepSeq.cpp:157 live use; :21 table, 16 entries) with the raw `char` step; UI clamps but a corrupt preset does not. Reviewer: Edge Case Hunter (Phase 1 review). Firmware-owner decision needed (clamp vs. validation).

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase1.md`
  plan: phase6 item 6.3
  summary: keybRamp <= -0.02 makes valueChanged(ENCODER_LFO_KSYNC) read invTab[] at a negative index (OOB global read).
  evidence: LfoOsc.h inline `invTab[(int)(keybRamp * 50.0f)]` (LfoOsc.h:35) — negative ramp yields index <= -1. Tests characterize only the safe -0.01 range (lfo_osc_test.cpp NegativeKeybRampResyncsPhaseAndSkipsRamp + header hazard note). Reviewer: Edge Case Hunter / Blind Hunter. Firmware-owner decision needed.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase1.md`
  plan: phase6 item 6.10
  summary: FxBus::delay2ReadPos/delay4ReadPos are uninitialized members read by the first processBlock before being written — safe only via BSS zero-init of the firmware globals.
  evidence: FxBus.h declares both without initializers (:164/:180); processBlock reads them on first run (:711/:755). Characterized in fx_bus_test.cpp fixture notes (BSS-equivalent backing). Reviewer: implementer + Blind Hunter. Firmware-owner decision (member initializers) needed.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase4.md`
  plan: phase6 item 6.4
  summary: `UserWaveform::interpolate` reads `buffer[iPos+1]` one past the populated source window for the last target sample — the value silently comes from stale/zero tail data. (The UserEnvCurve twin was removed with 5.4; this one is live code with real callers.)
  evidence: `firmware/Src/filesystem/UserWaveform.cpp` (interpolate). Characterized by `UserWaveformTest.InterpolateReadsOnePastPopulatedSourceQuirk`.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase4.md`
  plan: phase6 item 6.6
  summary: `MixerBank::saveMixer` copies 12 bytes of `mixerName` via `FileSystemUtils::copy` regardless of the source's actual length (same fixed-length-copy class as the fixed 2.1(b); the `saveDefaultMixer` truncation half was fixed in Phase 1 item 1.5).
  evidence: `firmware/Src/filesystem/MixerBank.cpp:262`.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase1.md` (Phase 1 review)
  plan: phase6 item 6.7 — OWNER DECISION D
  summary: Unknown mixer versions retain stale core state (name/channels/tuning/routing) while resetting only newer fields. Reason: avoid expanding this test-coverage PR into mixer fallback semantics; handle unknown-version behavior in a dedicated firmware fix (`tests/mixer_state_test.cpp:360,372`, `firmware/Src/synth/MixerState.cpp:192-215`; quirk pinned by `UnknownVersionFallsBackToDefaultsOnly`).

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase5.md`
  plan: phase6 item 6.8
  summary: MIDI controller encoder/button APIs do not bounds-check page and control indexes.
  evidence: `encoderDelta`, `buttonDown`, `buttonUp`, and the public getters index `midiPage_[pageNumber]` and its six-element arrays directly. Invalid caller indexes can access outside the state object; UI-valid inputs are the current implicit contract.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase5.md`
  plan: phase6 item 6.9
  summary: The controller does not reserve ring-buffer capacity for a complete three-byte CC message.
  evidence: Each event performs three independent `usartBufferOut.insert` calls without checking `isFull()` or available capacity. Near-full buffers can overwrite or expose a partial MIDI message depending on ISR timing; ring-overflow behavior is outside this coverage phase.

- source_spec: `_bmad-output/implementation-artifacts/spec-bugfix-phase5.md` (PR review)
  plan: phase6 item 6.11 — owner-level
  summary: `synth/Common.h` declares `strcmp` with C++ linkage, so any file in its include chain cannot include `<string.h>`/`<cstring>` — every new libc string user must hand-declare the function `extern "C"` instead (strnlen in `PreenFMFileType.cpp`, memcpy in `MidiControllerFile.cpp` this phase).
  evidence: the conflicting-declaration compile error surfaced in the phase-5 firmware build (MidiControllerFile.cpp including `<string.h>` after Common.h); both workarounds carry comments pointing at each other. A proper fix (make Common.h's declaration C-linkage or remove it) touches a firmware-wide header used by every translation unit — owner-level decision.

- source_spec: `_bmad-output/implementation-artifacts/spec-bugfix-phase5.md` (PR #30 review)
  plan: phase6 item 6.5
  summary: `SequenceBank::createSequenceFile` ignores failures and short writes for the bank header and each 1024-byte state block.
  evidence: `firmware/Src/filesystem/SequenceBank.cpp:313/:329` discards both `f_write` results. On an SD I/O error, later writes continue from the wrong offset and can create a malformed bank. This behavior predates Phase 5 item 5.6, which is scoped to preventing the zero-fill loop from spinning forever.

## Deliberate owner defers (firmware — decided NOT to fix for now)

- source_spec: `_bmad-output/implementation-artifacts/spec-bugfix-phase4.md` (PR review)
  summary: Mixer bank slots written by pre-4.8 firmware keep the permuted default master-FX values when loaded after the fix — the read side is frozen by design, so only re-saving the bank corrects them.
  evidence: `getFullDefaultState` now emits the canonical v6 order, but bank files already persisted with the old permuted byte order restore through the unchanged `restoreFullState` parse (order-sensitive, read side deliberately frozen per the approved spec). A migration/normalization pass (e.g. detect-and-rewrite on load, or a bank-format version bump) is a separate owner decision.

- source_spec: `_bmad-output/implementation-artifacts/spec-bugfix-phase4.md` (PR review)
  summary: The MONO held-note stack is not invalidated when the MIDI channel/range configuration changes while keys are held — note-offs swallowed by the routing change leave stale entries that a later recall could retrigger.
  evidence: `monoNotePush`/`monoNoteRemove` track preenNoteOn/preenNoteOff pairs; a channel or keyboard-range change mid-hold (review: Blind Hunter) drops the note-offs, so the stack retains notes no longer held. The play-mode self-heal added in the phase-4 review commit covers mode switches only. Needs an invalidation hook wherever the routing config changes.

## Features (not bugs)

- source_spec: `_bmad-output/implementation-artifacts/spec-dx7-bankdir-config.md`
  summary: Full recursive DX7 folder browser (multi-level drill in/out + breadcrumb, arbitrary depth under the configured root).
  evidence: Explicitly deferred by the user during planning — E-picker β (depth-1 folder picker under the root) is implemented first. The picker is designed as a non-throwaway stepping stone: its `initSubDirs(path,…)` enumeration and its "current folder = a path" model are reused directly by the browser, which adds only multi-level navigation and a breadcrumb on top.

## Test infrastructure & golden-master hardening (separate pass; see `static-analysis-followup-plan.md`)

### Build & CI

- source_spec: `_bmad-output/implementation-artifacts/spec-cmake-gcc15-build-system.md`
  summary: Mark the bootloader's `bootJumpToApplication` / `bootJump*` helpers `__attribute__((noreturn))` and drop the GCC-7-compat `-Wno-error=return-mismatch` workaround.
  evidence: Surfaced by step-04 review — these helpers never return but aren't declared noreturn, which is the root cause of the `main.c` `return;`-without-value diagnostics promoted to errors by GCC 14+. The CMake build currently downgrades them to warnings via the toolchain compat flags (faithful to GCC-7 behavior); a proper fix is the noreturn attribute + removing that specific `-Wno-error`. Pre-existing code smell, not caused by the CMake migration.

- source_spec: `_bmad-output/implementation-artifacts/spec-cmake-gcc15-build-system.md`
  summary: Compile only HAL modules enabled in `stm32h7xx_hal_conf.h` (instead of globbing all `Drivers/STM32H7xx_HAL_Driver/Src/*.c`), matching CubeIDE's per-module selection.
  evidence: Code review (edge-case hunter) — the original CubeIDE build compiled a subset (e.g. bootloader: 21 modules, excluding dma2d/i2c/i2c_ex); the CMake glob compiles all. `--gc-sections` already drops unused functions (firmware is within 5% despite this), so the impact is build efficiency + faithfulness, not correctness.

- source_spec: `_bmad-output/implementation-artifacts/spec-ci-build-and-release.md`
  summary: Guard or document that `workflow_dispatch` of release.yml creates the tag on the dispatched branch tip (`GITHUB_SHA`), not necessarily main.
  evidence: Surfaced by review (blind hunter) — a maintainer who dispatches the release from a feature branch would land the `v*` git tag on that branch's tip rather than main. Low-severity operator footgun (dispatch is human-driven); deferred rather than over-constraining the workflow. A future hardening could fail-fast when `github.ref != refs/heads/main` on dispatch.

- source_spec: `_bmad-output/implementation-artifacts/spec-ci-build-and-release.md`
  summary: Decouple release.yml's `artifacts/firmware/preenfm3.bin` download path from the upload-artifact least-common-ancestor layout.
  evidence: Surfaced by review (blind hunter) — `download-artifact@v4` extracts under the LCA of the uploaded paths; the hardcoded `artifacts/firmware/...` / `artifacts/bootloader/...` paths in the stage step would silently break if the upload list in build.yml gains/loses a top-level entry that shifts the LCA. Current layout is stable and the `find artifacts` debug line + stage-release.sh's file-existence check catch a mismatch loudly, so deferred.

- source_spec: `_bmad-output/implementation-artifacts/spec-ci-build-and-release.md`
  summary: Define behavior for force-repush of an existing `v*` tag (release update vs. tag-move semantics) in release.yml.
  evidence: Surfaced by review (blind hunter) — `softprops/action-gh-release@v2` on a force-pushed tag updates the Release, but the tag-move vs. release-recreate semantics are untested. Rare operator scenario; deferred pending a real need.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-infra.md`
  summary: coverage-gate.sh could parse `llvm-cov export -summary` JSON (`data.totals.lines.percent`) instead of TOTAL-line column `$10`, for forward version-stability.
  evidence: Review (edge-case hunter) — the `$10` column index assumes the Regions/Functions/Lines/Branches summary layout; verified correct on LLVM 18 (CI ubuntu) and 21 (macOS Apple CLT), and structurally sound on the older no-Branches layout (still `$10`), but a future llvm-cov summary redesign that adds/reorders a metric column would silently shift `$10` and the gate would enforce the wrong metric. JSON is version-stable; deferred because all current environments parse correctly and JSON parsing in POSIX sh adds a jq/python dependency.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-infra.md`
  summary: Pin `actions/checkout` and `actions/upload-artifact` to full 40-char commit SHAs across ALL workflows (repo-wide policy), not just coverage.yml.
  evidence: Review (blind hunter) — `actions/checkout@v5` / `actions/upload-artifact@v6` are mutable major-version tags (supply-chain risk, as in the trivy-action/kics-github-action compromises). coverage.yml matches the existing convention in tests.yml/static-analysis.yml/build.yml/release.yml, so SHA-pinning coverage.yml alone would make it inconsistent; this is a repo-wide policy decision, deferred out of this story's scope.

### Golden-master test side

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g0.md`
  summary: Make the tolerance-normalized hash a diagnostic-only signal (don't fail the test when goldenCompare passes within tolerance but the hash differs on a normalization bucket-boundary split).
  evidence: Review (blind hunter) — the README documents that bucket-boundary splits can make benign drift flip the hash, yet compareAgainstFixture fails the test on a hash mismatch even when goldenCompare passed. For G0's single-host CI the determinism self-check makes this unreachable (byte-exact render -> hash always matches), but cross-host (Phase G2) could false-positive. Phase G2 cross-host validation is the natural place to resolve it (tighten to exact-bit hash, or demote hash to diagnostic).

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g0.md`
  summary: renderA4DefaultSustain calls noteOn once and never resets Synth state — it is not idempotent across repeated calls on one harness.
  evidence: Review (blind hunter) — currently safe (each TEST constructs its own GoldenHarness and renders once), but the API invites a future caller to render twice on one harness and get a silently-wrong result (double noteOn, accumulated smoother/envelope state). Add either a documented one-call contract or a `rendered_` re-entry assertion.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g0.md`
  summary: Golden locks the int32 samples but discards buildNewSampleBlock's `uint8_t` return (the outputSaturated clip mask) — a DSP change producing identical samples but a different saturation flag would ship uncaught.
  evidence: Review (blind hunter) — the saturation mask is a real firmware observable (drives the clip/CPU UI) not currently captured. Scope-appropriate for a G1+ enhancement-goldens row, not G0's single canonical golden.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g0.md`
  summary: Guard RESET_DWT_CYCCNT()/READ_DWT_CYCCNT() and the DWT_*/SCB_DEMCT register macros under `!defined(PFM3_HOST)` in dwt.h, not just the CYCLE_MEASURE_* macros — a future direct host caller would fault with no compile-time guard.
  evidence: Review (edge-case hunter) — the G0 guard covers CYCLE_MEASURE_START/END (the only current consumers, Synth.cpp:275/501), so today's build is safe, but the underlying hazard primitives (RESET_DWT_CYCCNT, READ_DWT_CYCCNT, the DWT_CONTROL/DWT_CYCCNT/SCB_DEMCT volatile-pointer macros) remain unconditionally defined; a future host TU that touches them directly would segfault. Completing the guard is a seam-robustness cleanup.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g1.md`
  summary: `make golden-regen` regenerates ALL goldens (incl G0) — a maintainer intending to regen one fixture silently overwrites the others.
  evidence: Review (blind hunter) — the target runs `ctest -R 'Golden'` in regen mode, so every Golden test rewrites its fixture. With G1's 5× fixture count this footgun is more likely to bite. A `-R` filter or per-golden targets would scope regen. Low severity (regen is a deliberate act; a regen of G0 with an unchanged render writes identical bytes, but a stray local code change would silently propagate to all committed fixtures).

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g1.md`
  summary: Regen's success criterion is "fixture write ok", not "render is non-trivial" — a future change that silences a render would regen an all-zero fixture "successfully" and lock silence.
  evidence: Review (blind hunter) — `writeFixture` returns the hash and the test asserts it is non-zero, but an all-zero buffer still hashes non-zero, so a uniformly-silent regression (e.g. a voice-allocation bug making a golden produce silence) would be locked at regen time without any signal. A non-silence assertion in the regen path (e.g. require max|sample| above a floor) would catch it.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g1.md`
  summary: `multi_timbre_mix` only exercises buffer1 mixing (both timbres have the default `out=1`); no golden guards cross-buffer routing (timbres split across buffer2/buffer3) or `fxBus->mixAdd` into non-buffer1 outputs.
  evidence: Review (blind hunter) — the default mixer `out` values are `{1,1,4,4,6,8}`, so timbres 0+1 both sum into buffer1 (out1/2). A future golden with a `TimbreSetup` per-timbre `out` override (route timbre 1 to buffer2) would exercise the cross-buffer mix + fxBus paths the current golden cannot reach; deferred as a coverage enhancement, not a defect (the current golden accurately guards the buffer1 multi-timbre summing + per-timbre smoothers).

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g2-g3.md`
  summary: Automate the non-silence + distinct-from-G0 self-check for goldens (the spec's "confirm first-block max|sample| differs from G0" is currently a manual check, not a TEST).
  evidence: Review (blind hunter + edge case hunter) — the G3 `ALL_ENV_DECAY`→`MIX_OSC1` discovery and the ALG17 trap before it are both zero-signal-trap instances; `DeterminismSelfCheck` is blind to them (compares two silent renders as equal) and `compareAgainstFixture` passes if the fixture itself is silence. A `runGolden` assertion that the golden's render is non-silent AND not byte-identical to the G0 fixture (first N blocks) would catch the trap class generically at test time. Related to (but distinct from) the Phase G1 deferred item "regen success criterion is 'fixture write ok', not 'render is non-trivial'" — that one is about the regen path; this is about a compare-time distinctness assertion. Deferred because the exact policy (compare-to-G0 baseline vs an energy floor; per-golden opt-in vs global) is a design choice.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g2-g3.md`
  summary: The `allParameterRows` stub's permissive bounds (±1e6 for matrix + LFO rows) accept out-of-range values the real firmware would clamp (matrix mul [-10,24], LFO freq [0,100.8]).
  evidence: Review (blind hunter) — functionally faithful for the goldens' in-range values (mul 0.6, LFO freq 9.0), but a future G3+ golden sending an out-of-range value would lock the unclamped path and diverge from production. A future hardening could populate the stub's matrix/LFO ParameterDisplay entries with the real FMDisplayEditor.cpp min/max (the display-layer names/order arrays stay unneeded); low priority since the current goldens stay in-range and the stub is host-only.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g2-g3.md`
  summary: `GoldenHarness::setMatrixRow` has an undocumented single-use contract — calling it after `noteOn` or after a render leaves the voice's already-allocated routing stale (the `afterNewParamsLoad` reset of runtime caches does not re-route an active voice).
  evidence: Review (edge case hunter) — the header says "Call BEFORE noteOn" but there is no runtime guard; a future caller invoking setMatrixRow mid-render would get silently-wrong (partial) modulation. Low severity (current goldens call it before noteOn); a `renderStarted_` flag + abort in setMatrixRow would make the contract enforceable.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g2-g3.md`
  summary: `setMatrixRow`'s 12-arm switch over `rowIdx` couples the harness to `MATRIX_SIZE` without a compile-time guard against a future matrix-row-count change.
  evidence: Review (blind hunter) — the `rowIdx >= MATRIX_SIZE` abort (line ~243) defends the write, but the switch arm count must track `MATRIX_SIZE` manually. A computed-offset form (`((struct MatrixRowParams*)&getParamRaw()->matrixRowState1)[rowIdx]`, valid because matrixRowState1..12 are contiguous in OneSynthParams) would remove the coupling; cosmetic maintainability, not a defect.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g2-g3.md`
  summary: The stub's permissive-bounds override covers `ROW_MATRIX1..12` and `ROW_LFOOSC1..3` but NOT `ROW_LFOENV1..2` / `ROW_LFOSEQ1..2`, even though `Synth::newParamValue` has live cases for all five LFO row-groups.
  evidence: Review (edge case hunter) — the asymmetry is latent (no current golden drives LFOENV/LFOSEQ param changes); a future golden that does would hit the zeroed dummy (maxValue=0) and silently no-op. Wire those rows to `permissiveParamRow` too when a golden first needs them; consistent with "wire only what the current goldens need" but worth noting.

- source_spec: `_bmad-output/implementation-artifacts/spec-golden-master-phase-g4.md`
  summary: Phase G4 Option 3 — sequencer **internal-clock-mode** golden (needs a `HAL_GetTick` host seam for `Sequencer::ticMillis`).
  evidence: Phase G4 shipped the arp golden (internal block-counter clock, shim-free) + the seq **external**-MIDI-clock golden (driven by `MIDI_BYTE` clock bytes through `MidiDecoder`, shim-free). The seq **internal**-clock path (`Sequencer::ticMillis`, `Sequencer.cpp:296`) reads `HAL_GetTick` (line 338) for its cadence — the `#ifndef PFM3_HOST` gate removes the adjacent LED block but the cadence logic still needs a deterministic tick source to advance. A real host time seam (`tests/host_shims/`, a deterministic tick counter injected between render calls) is broader blast radius (`tests/SEAM.md` territory) than the shim-free G4 work. Internal-clock seq is rarely the regression source (external sync is the common live usage), so the cost/benefit doesn't clear until a regression demands it. Unblocks by: add a `HAL_GetTick` host shim (a test-owned monotonic counter the harness advances per `buildNewSampleBlock`), then a `seq_internal_playback` golden driving `sequencer->ticMillis()` between renders.

### Sequencer test pinning

- source_spec: `_bmad-output/implementation-artifacts/spec-bugfix-phase2.md`
  summary: The 2.3 note-off phantom path (values[8] read in the two note-off loops) is not independently pinned by a test.
  evidence: Review (Blind Hunter + Edge Case Hunter, bugfix-phase2). `StepPlaybackPlaysFiveNotesWithoutPhantomSixth` fails on the unfixed firmware via the note-ON loop, but the sequencer harness drives the real Synth graph, so a phantom noteOff for a note that never sounded is invisible to `getLowerNote` — a regression confined to the note-off loops would pass. All three loops share the identical rewritten condition, which is the current protection. Pinning needs an outgoing-note trace in the Synth test double (Target #4 pulls real Synth/Timbre/Voice).

### FatFs shim fidelity

- source_spec: `_bmad-output/implementation-artifacts/spec-bugfix-phase1.md`
  summary: Shim `f_rename` overwrite path does not refuse renames onto/around files with open handles (real FatFs returns `FR_LOCKED` under `FF_FS_LOCK`).
  evidence: Review (Blind Hunter + Edge Case Hunter, bugfix-phase1). The phase-1 `saveConfig` flow closes before renaming so no firmware path is affected today; fidelity gap noted for any future test that renames an open file. `f_unlink`'s open-handle check (FR_DENIED) exists; rename lacks its equivalent.

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase4.md`
  summary: Shim `f_stat` on a directory returns FR_NO_FILE (real FatFs returns FR_OK with `AM_DIR` set); the shim's directory model is otherwise observable only via opendir/readdir.
  evidence: `tests/host_shims/fatfs_impl.cpp` (f_stat); no firmware TU stats a directory today (PPMImage stats files only). Fidelity gap noted for any future TU that directory-stats. Review (Blind Hunter, Phase 4 review).

- source_spec: `_bmad-output/implementation-artifacts/spec-test-coverage-phase4.md`
  summary: Shim `f_readdir` enumerates live shim state (children recomputed per call, index-based iteration) — matches real FatFs's live-directory semantics, but a test that mutates the map mid-enumeration would see index-shifted results.
  evidence: `tests/host_shims/fatfs_impl.cpp` (f_readdir); intentional (real-FatFs fidelity), documented here so a future order-sensitive golden knows the contract. Review (Edge Case Hunter, Phase 4 review).

- source_spec: `spec-bugfix-phase6.md`
  summary: MixerState default-truth duplication — setDefaultValues() and getFullDefaultState() hand-maintain the same default-mixer values in two representations; they will diverge on the next default change.
  evidence: Phase-6 review (Blind Hunter). setDefaultValues (MixerState.cpp:219) now seeds the full serialized core mirroring getFullDefaultState (MixerState.cpp:104) per the frozen 6.7 spec design; unifying them (e.g. getFullDefaultState deriving from a default-seeded state + getFullState) is a refactor beyond the phase scope. Known-v6 round-trip tests pin both sides today.

- source_spec: `spec-bugfix-phase6.md`
  summary: MixerBank::saveMixer mutates mixName_ before f_open — a failed bank open leaves the in-memory mixer renamed while nothing was saved.
  evidence: Phase-6 review (Blind Hunter). Pre-existing ordering (the old fixed-length copy had it too); 6.6 kept it deliberately (name-copy scope only). Fix = move the copy after a successful open.
