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
  ±`lsbTolerance` (default ±1 LSB). A 1-ULP float drift in the mix becomes 0-or-1
  LSB after the `×0x7fffff` scale + truncation, so ±1 LSB absorbs benign
  compiler drift (gcc/clang, x86/arm64) while any real DSP change moves many
  samples by many LSBs.
- **Hash gate:** a self-contained FNV-1a 64-bit + splitmix64 finalizer over the
  tolerance-normalized buffer. No system/Homebrew `xxhash` (CI is ubuntu/gcc).
  The normalization quantizes each sample to its ±tolerance bucket so benign
  drift is hash-stable in the common case; bucket boundaries can still split
  under a uniform shift (a known, documented limitation — Phase G2 cross-host
  validation will decide whether to tighten to an exact-bit hash).
- **Determinism self-check:** `GoldenMaster.DeterminismSelfCheck` renders the
  golden twice in one process and asserts byte-exact equality. This is the
  prerequisite that makes the lock trustworthy.

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

All 200 blocks of `a4_default_sustain` are captured **including** the
`smoothVolume_`/`smoothPan_` one-pole transient (blocks ~0–10). The transient is
deterministic and is part of the locked behavior — no warm-start skip.

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
