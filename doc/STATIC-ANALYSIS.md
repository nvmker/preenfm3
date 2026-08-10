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
