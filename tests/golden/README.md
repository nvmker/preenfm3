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

Current fixtures: `a4_default_sustain_macos` (local dev, macOS Apple clang,
libsystem) and `a4_default_sustain_linux` (CI ubuntu gcc + ubuntu clang
coverage, glibc).

## Fixture layout (per golden id `X`)

| File | Contents |
| --- | --- |
| `X.bin` | Raw `int32_t` render, `nBlocks × 192` values (3 buffers × 64 stereo per block), host-native little-endian. |
| `X.xxh` | One line: the 64-bit tolerance-normalized hash as 16-digit lowercase hex. |
| `X.diff.txt` | Downsampled human-readable dump: first 4 blocks in full + 1-in-10 thereafter. For diffing on failure. |
| `schema.json` | Manifest: id, preset, note sequence, nBlocks, hash, tolerance, schema version. |

Layout per block in `.bin`: `[b1(64) b2(64) b3(64)]` — buffer1 = out1/2,
buffer2 = out3/4, buffer3 = out5/6 (the 6 hardware DAC outputs).

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

## Warm-start decision

All 200 blocks of each `a4_default_sustain_<variant>` fixture are captured
**including** the `smoothVolume_`/`smoothPan_` one-pole transient (blocks ~0–10).
The transient is deterministic (per-variant) and is part of the locked behavior
— no warm-start skip.

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

### Known benign UBSAN finding in the render path

`Synth::buildNewSampleBlock` formats each DAC sample with `*cb <<= 8`
(`Synth.cpp:~475/497`) on the clamped `int32_t`. When the clamped sample is
negative, this is **C++-standard undefined behavior** (left shift of a negative
signed value). It is well-defined on the two's-complement STM32H7 (Cortex-M7)
hardware the firmware targets, and the committed golden locks the actual
on-hardware result. The finding surfaces when the test binary is run **directly**
under UBSAN; `make test-asan` (via ctest) reports the test as passing because
UBSAN prints without aborting and ctest only shows output for failed tests. It
is a pre-existing firmware trait, not a fixture defect. The standard
well-defined fix (`(int32_t)((uint32_t)v << 8)`) would produce identical bytes,
so it could be applied as a separate firmware change without regenerating the
fixture — but that is a deliberate firmware edit, out of scope for the golden
tier itself.
