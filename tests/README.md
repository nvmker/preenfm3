# preenfm3 host-side unit tests

Host-side C++ unit tests for preenfm3 firmware, built with **GoogleTest** and
run with **CTest**. No hardware, no cross-compiler — these compile on your dev
machine (or CI) with the system `g++`/`clang++`.

> This directory is **scaffolding**: it wires GoogleTest + CTest + a smoke test.
> Coverage of real firmware units lands in a follow-up. See *Roadmap* below.

## Why host-side, and the compiler rule

The firmware builds with `arm-none-eabi-gcc` via `cmake/arm-none-eabi-gcc.cmake`.
That toolchain file applies to an **entire** CMake build tree, so it cannot
co-exist with a host-compiled test target. Therefore `tests/` is a **standalone
CMake project** — configure it **without** `-DCMAKE_TOOLCHAIN_FILE`, using the
default host toolchain.

**Do not** `add_subdirectory(tests)` from the top-level `CMakeLists.txt`. That
would pull the Arm toolchain into the test build and fail to link a host
executable.

The trade-off accepted here: tests can only cover firmware logic that is
*host-compilable* — i.e. the pure logic extracted from HAL/hardware dependencies.
That is the intended scope (and the highest-value, lowest-cost level).

## Run the tests

### Makefile wrapper (easiest)

```sh
make test          # configure + build + run, into build/test/
```

`make clean` removes `build/test/` along with the rest of `build/`.

### Raw CMake

```sh
cmake -B build/test -S tests
cmake --build build/test -j
ctest --test-dir build/test --output-on-failure

# Run a single test by name:
ctest --test-dir build/test -R 'Smoke.BasicAssertionWorks' --output-on-failure
```

### Sanitizer run (ASAN + UBSAN)

```sh
make test-asan
```

Builds the suite under `-fsanitize=address,undefined` and runs ctest. This is a
**reporting** target (not a CI gate) — it matches the `make analyze`
tolerant-triage philosophy. The Hexter coverage session surfaced a real
global-buffer-overflow via this flow under fuzzed input; later coverage phases
repeat that deliberately.

### Coverage run (LLVM source-based)

```sh
make test-cov
```

Builds the suite with clang + `-fprofile-instr-generate -fcoverage-mapping`,
runs ctest, merges the per-test `.profraw`, and prints an `llvm-cov report`
scoped to `firmware/Src` (headers excluded, matching the 12.45% baseline in
`_bmad-output/planning-artifacts/test-coverage-plan.md`). Artifacts land in
`build/test-cov/` (`pfm3_tests.profdata`, `coverage-report.txt`).

The target **forces clang and pins the LLVM tool pair** because LLVM
source-based coverage requires the compiler and `llvm-cov`/`llvm-profdata` to
come from one LLVM distribution: on macOS it pins to Apple's CommandLineTools
pair (`/usr/bin/clang++` +
`/Library/Developer/CommandLineTools/usr/bin/{llvm-cov,llvm-profdata}`); on
Linux it uses the system `clang++` + `llvm-cov`/`llvm-profdata`. On a dev Mac
with Homebrew LLVM alongside Apple's CLT, a naive build silently picks a
mismatched pair and fails at `llvm-profdata merge` ("unsupported
instrumentation level"). The target also wipes `build/test-cov/` before each
configure so a stale cache can't silently produce an uninstrumented build
(CMake won't override a cached `CMAKE_CXX_FLAGS` on reconfigure).

The CI floor gate (`.github/workflows/coverage.yml` +
`scripts/ci/coverage-gate.sh`) consumes the same report and fails on
regression below `scripts/coverage-floor.txt`.

### Perf bench binary (Callgrind Ir gate workload)

```sh
cmake -B build/bench -S tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/bench --target pfm3_bench -j
./build/bench/pfm3_bench --script=a4_default_sustain --json   # list: omit --script
```

`pfm3_bench` (`bench/perf_main.cpp`) is the workload driver for the CI perf
**gates** (two signals): it renders the golden-master scripts — paired with
the exact out-of-band patches the corresponding tests apply — and either
exits 0 (Ir mode: the measurement happens outside the process) or times the
render itself (wall mode). The **Ir signal** is measured by
`.github/workflows/benchmark.yml`, which runs the bench under
valgrind/callgrind (toggle-collect scoped to `Synth::buildNewSampleBlock`)
inside the pinned `gcc:13.3.0-bookworm` container; `scripts/ci/perf-gate.sh`
compares total Ir against `scripts/perf-baseline.json` (2% regression gate).
It has **no gtest** in its link closure.

Wall mode (the second signal, Phase 3a) — usable locally on any OS:

```sh
./build/bench/pfm3_bench --script=a4_default_sustain --mode=wall --json
# {"script": "a4_default_sustain", "blocks": 200, "mode": "wall", "warmup": 3,
#  "repeat": 10, "ns_per_block": ..., "min": ..., "max": ...}
```

Semantics: `--mode=wall` renders `--warmup` (default 3) untimed warmups,
then `--repeat` (default 10) measured renders — a FRESH harness per render,
with only `renderScript` inside the `steady_clock` window — and prints the
median ns/block (with `--json`, exactly one JSON line; the CI gate parses
it). Wall is a **trend/safety-net signal**, gated generously at 25% in CI
(`meta.wall_threshold_pct`): shared-runner clock noise makes it
non-deterministic, but it catches cache/allocation/algorithmic regressions
the deterministic Ir gate cannot see. A local wall smoke run works fine on
any OS with any compiler — only the **Ir** measurement needs the pinned
container/valgrind.

## Build layout: the pfm3_fw_host object library

The firmware-TU closure (everything except the `*_test.cpp` GLOB sources,
including `golden_harness.cpp`) compiles into the **`pfm3_fw_host` OBJECT
library** — shared by `pfm3_tests` and `pfm3_bench` so the measured code is
byte-for-byte the code the golden tests lock. Its include paths, `PFM3_HOST`,
and compile options (`-ffp-contract=off` …) are PUBLIC: consumers compile as if
the closure were still inlined in their own target (the refactor changed
nothing about how `pfm3_tests` compiles — the goldens stayed byte-identical).
New firmware TUs for future coverage sessions belong in
`target_sources(pfm3_fw_host …)`; new tests are just a dropped-in `*_test.cpp`.

## How GoogleTest is fetched

`FetchContent_Declare` in [`CMakeLists.txt`](CMakeLists.txt) pulls GoogleTest
from its git repo at configure time. The tag is **pinned** (`v1.17.0`) so CI is
reproducible and a GoogleTest release can't silently change test behavior. To
upgrade: bump `GIT_TAG` here **and** the cache key in
[`.github/workflows/tests.yml`](../.github/workflows/tests.yml) together.

## Conventions

- **One `*_test.cpp` per unit under test** — `file(GLOB ... CONFIGURE_DEPENDS)`
  picks up new files on the next configure automatically.
- Use `TEST_F` fixtures for shared setup; prefer small, focused `TEST` cases.
- Keep tests deterministic — no real-time clocks, no hardware registers, no
  floating drift across hosts. (Flakiness is critical tech debt.)
- Name tests `<Suite>.<Case>` so `ctest -R` filtering stays ergonomic.

## Roadmap (future coverage sessions)

The scaffolding proves the harness runs. The next sessions add coverage, ranked
by **impact × bug-likelihood**:

| Target | File(s) | Guards against | Status |
| --- | --- | --- | --- |
| Sequencer serialization | `firmware/Src/midi/Sequencer.cpp` | regression of the `-Ofast` unaligned-float hard-fault | ✅ done (Target #1) |
| DX7 sysex import | `firmware/Src/utils/Hexter.cpp` | crash/corruption on malformed sysex | ✅ done (Target #2; surfaced + fixed a global-buffer-overflow) |
| Synth math | `firmware/Src/synth/{Osc,Env,Matrix}.cpp` | silent audio regressions | ✅ done (Target #3) |
| MIDI decode | `firmware/Src/midi/MidiDecoder.cpp` | stuck notes / wrong CC routing | ✅ done (Target #4; decode state machine + NRPN assembly + routing through the real Synth graph) |

All four roadmap targets are now covered. The host-testability seam is
backwards-compatible: new coverage sessions can drop in another `*_test.cpp`
and extend `target_sources` without revisiting the seam decision (see
[SEAM.md](SEAM.md)).

Each of these currently `#include`s HAL/STM32 headers transitively. The work is
**extraction**: isolate the pure logic into a host-compilable translation unit
behind a thin shim (a `PFM3_HOST` define that stubs `HAL_*` and hardware calls),
then test that. GoogleTest is already waiting for them.
