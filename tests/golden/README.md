# Golden-master fixtures — preenfm3 full-render regression

This directory holds the committed golden fixtures for the full-render regression
tier (`tests/golden_master_test.cpp`). See
`_bmad-output/planning-artifacts/golden-master-test-plan.md` for the full design.

## What is locked

Each fixture is a byte-exact snapshot of the complete 6-output render of
`Synth::buildNewSampleBlock(b1, b2, b3)` across a scripted note/parameter
sequence. The render goes through the **real** firmware Synth graph
(Synth → Timbre → Voice → Matrix → FxBus → int32 mix) — no mocks. The golden
locks the **aggregate** chain, complementing the per-unit goldens in
`synth_math_test.cpp` / `hexter_test.cpp`.

## Cross-platform fixtures

The full render is **NOT byte-stable across libms**. `Osc::init` / `Env::init`
precompute their tables at init time via `sinf`/`expf`/`logf`, and those table
values differ ~1-ULP between platform libms (macOS libsystem vs linux glibc).
The preenfm oscillators use **FM feedback**, which is chaotic — a ~1-ULP
wavetable/env-table difference amplifies exponentially, so block 0's output
stays within ±1 LSB but by block 1 the two libms diverge by ~36% of full scale.
No compile flag fixes this (`-ffp-contract=off` was probed; the divergence is
libm-driven, not FMA-contraction).

Crucially, the discriminator is the **libm, not the compiler**: ubuntu gcc and
ubuntu clang BOTH link glibc and produce **byte-identical** renders, while macOS
clang links libsystem and differs. So each platform/libm gets its **own
committed fixture**, and `tests/golden_master_test.cpp` selects by `__APPLE__`
(macOS → `_macos`, else → `_linux`). Each fixture is a faithful per-platform
lock; the regression guard catches any same-platform render change (the
realistic regression class). Adding a new platform/libm requires generating +
committing its fixture (see *Regenerating fixtures* below, run on that platform).

Current fixtures: see the **Fixture catalog** below. G0 (`a4_default_sustain`)
has both `_macos` and `_linux` committed, as do the four Phase G1 goldens
(`envelope_adsr_full`, `fm_algo2`, `fm_algo27_6carrier`, `multi_timbre_mix`)
— all 10 of those fixture triples (5 goldens × {macos, linux}) are committed.
The three Phase G3 goldens (`live_lfo_pitch_modulation`,
`live_matrix_mul_change`, `live_lfo_freq_change`) have their `_macos` triples
committed with the code; their `_linux` triples are generated via the
`regenerate-linux-goldens` workflow dispatch (⏳ in the catalog until committed).
The `regenerate-linux-goldens` workflow (`workflow_dispatch`, usable once merged
to `master`) regenerates the `_linux` triples on demand when a render change
legitimately alters them — see *Regenerating the `_linux` fixtures* below.

## Fixture layout (per golden id `X`)

| File | Contents |
| --- | --- |
| `X.bin` | Raw `int32_t` render, `nBlocks × 192` values (3 buffers × 64 stereo per block), host-native little-endian. |
| `X.xxh` | One line: the 64-bit tolerance-normalized hash as 16-digit lowercase hex. |
| `X.diff.txt` | Downsampled human-readable dump: first 4 blocks in full + 1-in-10 thereafter. For diffing on failure. |
| `schema.json` | Manifest: id, preset, note sequence, nBlocks, hash, tolerance, schema version. |

Layout per block in `.bin`: `[b1(64) b2(64) b3(64)]` — buffer1 = out1/2,
buffer2 = out3/4, buffer3 = out5/6 (the 6 hardware DAC outputs).

## Fixture catalog

Each row is one `TEST(GoldenMaster, …)`; the platform suffix (`_macos`/`_linux`)
is appended by `tests/golden_master_test.cpp`. ✅ = fixture triple committed in
this directory.

| id | script | nBlocks | guards | macos | linux |
| --- | --- | --- | --- | --- | --- |
| `a4_default_sustain` | noteOn(0,69,100)@0, sustain | 200 | entire Synth→output chain (default ALGO1) | ✅ | ✅ |
| `envelope_adsr_full` | noteOn(0,69,100)@0, noteOff(0,69)@300 | 600 | Env stage transitions + release tail | ✅ | ✅ |
| `fm_algo2` | setTimbreAlgo(0, ALGO2), noteOn(0,69,100)@0 | 200 | ALGO2 `{1,1,2,0,0,0}` — 2-carrier summing | ✅ | ✅ |
| `fm_algo27_6carrier` | setTimbreAlgo(0, ALG27), noteOn(0,69,100)@0 | 200 | ALG27 `{1,1,1,1,1,1}` — 6-carrier additive summing | ✅ | ✅ |
| `multi_timbre_mix` | noteOn(0,69,100) + noteOn(1,72,100)@0 | 200 | voicesToTimbre mix + per-timbre smoothVolume_ + fxBus->mixAdd | ✅ | ✅ |
| `live_lfo_pitch_modulation` | setMatrixRow(LFO1→OSC1_FREQ@0.5), noteOn(0,69,100)@0 | 400 | steady LFO1→matrix→osc-freq→Voice routing (several LFO cycles) | ✅ | ⏳ |
| `live_matrix_mul_change` | setMatrixRow(LFO1→OSC1_FREQ@0.0), noteOn@0, setNewValueFromMidi(ROW_MATRIX8,ENCODER_MATRIX_MUL,0.6)@80 | 200 | live CC→matrix-mul→Voice: pitch wobble kicks in mid-note | ✅ | ⏳ |
| `live_lfo_freq_change` | setMatrixRow(LFO1→MIX_OSC1@0.5), noteOn@0, setNewValueFromMidi(ROW_LFOOSC1,ENCODER_LFO_FREQ,9.0)@100 | 300 | live LFO-freq CC + non-pitch (amplitude/tremolo) destination | ✅ | ⏳ |

**Phase G3 — live mid-render param changes (the "stuck/wrong CC routing" bug
class):** G0/G1 lock the render under *static* params (noteOn/noteOff +
algo/voice setup only). The G3 goldens drive `Synth::setNewValueFromMidi`
mid-render on a SOUNDING voice — the path MIDI CCs take in production. The
matrix routing is set out-of-band via `GoldenHarness::setMatrixRow` (a
pre-render field patch + `afterNewParamsLoad`, same proven pattern as
`setTimbreAlgo`); the mid-render `PARAM_CHANGE` events fire `setNewValueFromMidi`.

**Critical constraint — why matrix routing, not direct osc-freq writes:**
`Synth::newParamValueFromExternal` (the listener `setNewValueFromMidi` propagates
to) has switch cases for matrix rows (`ENCODER_MATRIX_MUL` writes `rows[r].mul`,
read live every block by `Matrix::computeAllDestinations`), LFO rows
(`ENCODER_LFO_FREQ` writes `lfo->freq`), etc. — but **NO case for
`ROW_OSC1..6` or `ROW_ENGINE`**. A direct osc-frequency / env-time param write
via `setNewValueFromMidi` does NOT take effect on a sounding voice (the voice
reads those at note allocation). So live pitch/env changes MUST route through
the matrix (destination `OSC1_FREQ` / `ALL_ENV_DECAY` / `MIX_OSC1` / etc.), which
is re-read every block. See
`_bmad-output/implementation-artifacts/spec-golden-master-phase-g2-g3.md`
Design Notes.

**G3 golden 3 destination change (from `ALL_ENV_DECAY` to `MIX_OSC1`):** the
spec originally specified `LFO1→ALL_ENV_DECAY` for golden 3. Implementation
found `ALL_ENV_DECAY` modulation is **inaudible on a sustained note** — the env
leaves its decay stage ~block 50, so decay-time modulation has no effect (the
golden rendered byte-identical to G0, a zero-signal trap). Switched to
`MIX_OSC1` (osc1 level → tremolo), which is audible on a sustained note and
gives dest-diversity (amplitude, vs goldens 1/2's pitch). The live LFO-freq
change at block 100 then doubles the tremolo rate — clearly audible. See the
spec's Spec Change Log.

**`allParameterRows` stub enhancement:** the host build links a *stub*
`allParameterRows` (`tests/stubs/midi_decoder_collaborators_stub.cpp`), not the
real one from FMDisplayEditor.cpp (4365 lines of non-host-compilable TFT code).
The stub's zeroed `ParameterDisplay` (maxValue=0) made `Timbre::setNewValue`
clamp every positive value to 0, silently defeating `setNewValueFromMidi` for
G3's mid-render changes. The stub now provides a permissive-bounds
`ParameterRowDisplay` (min/max = ±1e6) wired to **only** the matrix rows
(`ROW_MATRIX1..12`) and LFO-osc rows (`ROW_LFOOSC1..3`); all other rows stay on
the zeroed dummy so `midi_decoder_test`'s NRPN-assembly behavior is unchanged.
The real bounds (matrix mul `[-10,24]`; LFO freq `[0,100.8]`) would clamp
identically for the goldens' in-range values (mul 0.6, LFO freq 9.0).

**G1 algorithm note:** the default `preenMainPreset` has all-zero modulation
indices, so only the **carrier count** distinguishes algorithms under it (an
algorithm that keeps 1 carrier renders byte-identical to G0). The FM goldens
therefore lock **carrier-topology diversity** (ALGO2 = 2 car, ALG27 = 6 car),
not modulation stacks. True modulation-routing coverage needs a non-zero-IM
preset and is deferred.

## Comparison model

- **Authoritative gate:** `goldenCompare` — every sample must agree within
  **±256 stored `int32` units**. The renderer's final DAC formatting is
  `int32 = (24-bit clamped sample) << 8` (`Synth.cpp` clamps to ±0x7FFFFF then
  left-shifts by 8), so **256 stored units = 1 audio-LSB** — the meaningful
  signal tolerance. (A bare ±1 on the stored value would only cover the
  always-zero low padding bits and effectively require an exact match.) This
  absorbs benign drift **within a platform/libm** (minor-version / build-path
  differences); it does NOT absorb cross-libm drift — see *Cross-platform
  fixtures*. Any real DSP change moves many samples by many audio-LSBs.
- **Hash (diagnostic-only):** a self-contained FNV-1a 64-bit + splitmix64
  finalizer over a tolerance-normalized buffer (granularity 1; no system/Homebrew
  `xxhash`). The committed `.xxh` is compared on each run, but a mismatch is
  **informational only** — printed to stderr, never fails the test.
  `goldenCompare` is the sole pass/fail gate. (The hash's normalization can flip
  on a bucket boundary even when `goldenCompare` passes within tolerance, so
  making it authoritative would re-introduce false positives that defeat the
  tolerance.)
- **Determinism self-check:** `GoldenMaster.DeterminismSelfCheck` renders the
  golden twice in one process and asserts byte-exact equality. This is the
  prerequisite that makes the golden lock trustworthy.
- **Tolerance-headroom monitor (Phase G2):** `GoldenMaster.ToleranceHeadroom`
  renders `a4_default_sustain`, computes the per-sample max |delta| against the
  committed same-platform fixture across ALL samples (`goldenCompare` stops at
  the first mismatch; this scans the whole buffer), and records it as test
  properties (`max_delta_stored_units`, `max_delta_audio_lsb`, `max_delta_block`)
  visible in the ctest/CI JUnit XML. On the exact commit that generated the
  fixture the value is **0** (within-process byte-exactness, per the
  DeterminismSelfCheck); a future macOS point release / glibc update shifting
  libm tables by a few ULP would show a small non-zero value — the point is to
  surface silent drift toward the ±256 ceiling *before* the main gate fails. It
  does NOT tighten the gate (the committed fixture is from a prior libm build;
  exact-match would false-positive on legitimate drift). DeterminismSelfCheck
  gives a binary pass/fail; ToleranceHeadroom quantifies the consumption.

## Regenerating fixtures

Regeneration is a **deliberate** act, gated behind an env var. Fixtures are
regenerated **only** when the render output legitimately changes, and the commit
message must explain why.

```sh
make golden-regen      # builds + runs the Golden tests in regen mode
```

or manually:

```sh
cmake -B build/test -S tests && cmake --build build/test -j
PFM3_REGENERATE_GOLDENS=1 ctest --test-dir build/test -R 'Golden' --output-on-failure
```

Then commit the regenerated `X.bin` / `X.xxh` / `X.diff.txt`. The subsequent
plain `make test` compares against them.

**Never** regenerate to silence a mismatch you do not understand. A mismatch is
a signal that something in the render path changed; investigate first.

### Regenerating the `_linux` fixtures via GitHub Actions

The `_linux` (glibc) fixtures are committed; use this workflow to regenerate
them on demand when a render change legitimately alters them and you don't
have a Linux host. The manual **regenerate-linux-goldens** workflow
(`.github/workflows/`) must exist on `master` to be dispatchable — GitHub
registers `workflow_dispatch` workflows from the default branch — so it's
usable once this PR merges.

```sh
gh workflow run regenerate-linux-goldens.yml --ref <target-branch>
```

(or via the Actions UI: select the workflow → **Run workflow** → choose the
branch.) The workflow (ubuntu-latest) builds the tests, regenerates every
`_linux` triple, **self-verifies** with a full `ctest` (this green run IS the
linux-gate evidence), then commits + pushes the changed `_linux` fixture files
to the target branch. The commit is pushed by the default `GITHUB_TOKEN`,
whose pushes do **not** re-trigger `pull_request` workflows (GitHub loop
prevention) — so the Self-verify step is the linux-green proof; the separate
`tests.yml` check can be re-run manually if desired.

After it runs, update any `_linux` entry in `schema.json` whose `hash` changed
(the test reads `.bin`/`.xxh`, not `schema.json`, so CI is green on the fixture
files alone; `schema.json` is catalog completeness).

## Warm-start decision

Every fixture captures **all** its blocks including the `smoothVolume_`/
`smoothPan_` one-pole transient (blocks ~0–10) and, for `multi_timbre_mix`, the
per-timbre smoother startup; for `envelope_adsr_full`, the Env release tail
(blocks 300–599). The transient is deterministic (per-variant) and is part of
the locked behavior — no warm-start skip.

## Schema versioning

`schemaVersion` in `schema.json` is bumped if the fixture format, the render
script, or the comparison/hash semantics change in a way that invalidates
existing fixtures. Bumping it requires regenerating every fixture and noting the
reason in the manifest.

## Characterization stance

The fixtures lock the **current** render output, including any latent quirks.
A golden mismatch is a signal to investigate what changed in the render path,
not an instruction to regenerate. This is the same stance the per-unit tests
take for preserved firmware quirks (see `tests/SEAM.md`).

### Previously reported UBSAN finding in the render path — resolved

`Synth::buildNewSampleBlock` previously formatted each DAC sample with
`*cb <<= 8` (`Synth.cpp:~475/497`) on the clamped `int32_t`. When the clamped
sample was negative this was **C++-standard undefined behavior** (left shift of
a negative signed value), well-defined only on the two's-complement STM32H7
(Cortex-M7) target. It has since been rewritten as the fully standard-defined,
bit-identical signed-multiply form `*cb = *cb * 256` (all three output buffers).
The preceding clamp bounds the value to [-0x7FFFFF, 0x7FFFFF], so the scaled
result stays within [-0x7FFFFF00, 0x7FFFFF00] — strictly inside int32_t range,
which means signed arithmetic cannot overflow (no UB) and no cast is needed
(no implementation-defined unsigned-to-signed step). The committed golden
fixtures are **unchanged** — the rewrite produces the same bytes as the
on-hardware result the fixtures locked, verified by the
`GoldenMaster.A4DefaultSustain200Blocks` test passing against the existing
fixtures with no regeneration. Running the test binary directly under UBSAN
(`UBSAN_OPTIONS=halt_on_error=1 ... --gtest_filter='GoldenMaster.*'`) now exits
clean; previously it aborted at `Synth.cpp:475` with `left shift of negative
value`.
