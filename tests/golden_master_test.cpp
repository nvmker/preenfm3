// Golden-master full-render regression for preenfm3.
//
// Regression target (per _bmad-output/planning-artifacts/golden-master-test-plan.md,
// Phase G0): the per-unit goldens (Osc::getNextBlock, Env::getNextAmpExp) guard
// individual DSP units but NOTHING guards the aggregate Synth -> Timbre -> Voice
// -> Matrix -> FxBus -> int32 mix chain. A silent regression in end-to-end
// matrix routing, voice allocation, the DAC-scale mix, or the
// smoothVolume_/smoothPan_ smoothers would ship uncaught. This tier snapshots
// the full 6-output render of Synth::buildNewSampleBlock across a scripted note
// sequence and locks it with a self-contained hash + ±1-LSB compare.
//
// CHARACTERIZATION STANCE: the committed fixture locks the CURRENT render
// output, including any latent quirks. A golden mismatch is a signal to
// investigate what changed in the render path, NOT an instruction to regenerate
// the fixture. Regeneration is a deliberate act gated behind the
// PFM3_REGENERATE_GOLDENS env var (see tests/golden/README.md); the commit
// message for a regeneration must explain WHY the render output legitimately
// changed.
//
// Determinism model: the render is fully reproducible PER (libm-class) on host.
// The HAL RNG is gated to seed 0 under PFM3_HOST (Synth.cpp:285) and the
// downstream noise[] fill loop is shared host/firmware (same LCG, same table).
// The smoothVolume_/smoothPan_ one-pole transient (blocks ~0-10) is deterministic
// and is part of the locked behavior — all 200 blocks are captured (no warm-
// start skip). NOTE the render is NOT byte-stable ACROSS libms: macOS libsystem
// vs linux glibc differ in the Osc::init/Env::init precomputed tables
// (sinf/expf/logf), and FM feedback amplifies the ~1-ULP table difference, so
// each platform/libm gets its OWN committed fixture (see tests/golden/README.md
// -> Cross-platform fixtures + the fixture-id selection below). Ubuntu gcc and
// ubuntu clang both link glibc and produce byte-identical renders, so the
// discriminator is the PLATFORM (__APPLE__), not the compiler.

#include "golden_harness.h"
#include "golden/golden_snapshot.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// tests/CMakeLists.txt defines PFM3_GOLDEN_DIR to the absolute source path of
// tests/golden/ so fixture I/O is cwd-independent under ctest (which runs each
// TEST() from an arbitrary cwd).
#ifndef PFM3_GOLDEN_DIR
#error "PFM3_GOLDEN_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace {

constexpr const char* kFixtureDir = PFM3_GOLDEN_DIR;
constexpr std::size_t kNBlocks = 200;

bool regenMode() {
    // Require the documented opt-in value exactly ("=1"): a bare presence
    // check would also enable regen for PFM3_REGENERATE_GOLDENS=0 or an empty
    // value, silently overwriting fixtures during an ordinary test run.
    const char* v = std::getenv("PFM3_REGENERATE_GOLDENS");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// The full-render golden is (libm-class)-specific: libm differences in the
// Osc::init/Env::init precomputed tables (sinf/expf/logf) are amplified by FM
// feedback, so a libsystem render (macOS) and a glibc render (linux) diverge by
// >1 LSB within ~1 block. Empirically ubuntu gcc and ubuntu clang BOTH link
// glibc and produce byte-identical renders, so the discriminator is the
// PLATFORM/libm, NOT the compiler. The test selects by __APPLE__; each platform
// locks its own committed fixture. A new platform/libm must generate + commit
// its own fixture (see tests/golden/README.md -> Cross-platform fixtures).
#if defined(__APPLE__)
#define PFM3_GOLDEN_VARIANT "macos"      // libsystem libm (macOS dev)
#else
#define PFM3_GOLDEN_VARIANT "linux"      // glibc libm (CI ubuntu gcc + ubuntu clang)
#endif

// Fixture id for the canonical G0 golden, platform-suffixed.
const std::string& a4DefaultSustainId() {
    static const std::string id =
        std::string("a4_default_sustain_") + PFM3_GOLDEN_VARIANT;
    return id;
}

// Common render + (compare-or-regen) for the Phase G1 goldens. Each G1 TEST is
// a one-line call to this; G0's A4DefaultSustain200Blocks above is intentionally
// left inline — it is the byte-identical-refactor guard for the delegator
// renderA4DefaultSustain, so it must exercise that exact path, not renderScript.
//
// idBase:      fixture id WITHOUT the platform suffix (e.g. "envelope_adsr_full");
//              the _macos/_linux variant is appended here.
// algoTimbre:  timbre whose FM algorithm to override before render, or -1 to
//              leave the default ALGO1 (from preenMainPreset). `algo` is ignored
//              when algoTimbre < 0.
// preRender:   optional setup hook run AFTER construction + setTimbreAlgo and
//              BEFORE renderScript. Phase G3 goldens use it to call
//              setMatrixRow(...) (their matrix routing is a per-timbre state
//              change, like setTimbreAlgo). Default {} = no extra setup (G0/G1).
void runGolden(const char* idBase, std::size_t nBlocks,
               const golden::RenderScript& script,
               const golden::TimbreSetup& setup,
               int algoTimbre, Algorithm algo,
               std::function<void(golden::GoldenHarness&)> preRender = {}) {
    const std::string id = std::string(idBase) + "_" + PFM3_GOLDEN_VARIANT;
    golden::GoldenHarness harness(kFixtureDir, setup);
    if (algoTimbre >= 0) {
        harness.setTimbreAlgo(algoTimbre, algo);
    }
    if (preRender) {
        preRender(harness);
    }
    std::vector<int32_t> render(nBlocks * golden::GoldenHarness::kSamplesPerBlock);
    harness.renderScript(script, nBlocks, render.data());

    if (regenMode()) {
        const uint64_t h = harness.writeFixture(id, render.data(), nBlocks);
        ASSERT_NE(h, 0ull) << "fixture regeneration failed for " << id
                           << " (see stderr)";
        return;
    }
    golden::GoldenDiff diff{};
    const bool ok =
        harness.compareAgainstFixture(id, render.data(), nBlocks, &diff);
    if (!ok) {
        // goldenCompare is authoritative (the hash is diagnostic-only; see
        // tests/golden/README.md).
        FAIL() << "golden mismatch for " << id << " at flat index "
               << diff.firstMismatchIndex << " (block " << diff.blockIndex
               << "): expected=0x" << std::hex << diff.expectedSample
               << " actual=0x" << diff.actualSample << std::dec
               << " delta=" << diff.sampleDelta
               << " (tolerance=±256 stored units = ±1 audio-LSB; the firmware "
                  "clamps to 24-bit then left-shifts 8)\n"
               << "If this is a deliberate render change, regenerate with"
               << " PFM3_REGENERATE_GOLDENS=1 (see tests/golden/README.md).";
    }
}

// ===========================================================================
// Phase 3 sweep helpers (spec-test-coverage-phase3). The sweeps render every
// FM algorithm / FX filter WITHOUT committing a fixture — they assert the
// invariants every golden implicitly relies on: (a) FINITE output — the DAC
// formatting clamps to ±0x7FFFFF then <<8, so any |sample| above 0x7FFFFF00
// (the exact clamp ceiling in stored units) means the shift step overflowed
// (a real signal, not merely a style question — int32 has no NaN); (b)
// DETERMINISM — a second, identically-configured harness must render
// byte-equal (DeterminismSelfCheck's proof, generalized per config); (c)
// NON-SILENCE — max |sample| must exceed kSweepSilenceThreshold so a
// regression that zeroes an algo/FX arm cannot hide behind a passing sweep.
// ===========================================================================

// Theoretical clamp ceiling in stored int32 units (24-bit clamp <<8). A sample
// above this (only INT32_MIN..-0x80000000 could be) means the multiply-by-256
// overflowed signed range — the finite-output signal for int32 storage.
constexpr int64_t kStoredClampCeiling = 0x7FFFFF00;

// Non-silence oracle: max |sample| in stored int32 units across the render.
// 1e5 stored units = ~390 audio-LSB (256 units/LSB) — comfortably above the
// ±1-LSB compare tolerance, so anything at/under it is acoustically silence.
// Empirical anchor: the a4_default_sustain render peaks at >1e8 stored units;
// every non-exempt algo/FX config measured in the sweeps exceeds 1e5 by orders
// of magnitude. Documented (not tuned per-case) so a future algo quirk that
// barely whispers still gets flagged rather than excused.
constexpr int64_t kSweepSilenceThreshold = 100000;

int64_t maxAbsSample(const int32_t* buf, std::size_t count) {
    int64_t m = 0;
    for (std::size_t i = 0; i < count; i++) {
        const int64_t v = std::abs(static_cast<int64_t>(buf[i]));
        if (v > m) m = v;
    }
    return m;
}

// Shared finite/deterministic/non-silent assertions for one sweep config.
// `label` names the config in failure messages (e.g. "algo 13/ALG13").
// `expectSilent` exempts a config from the NON-SILENCE assert only — the
// finite and determinism asserts always hold (a "silent exemption" never
// excuses NaN/overflow or non-determinism; documented exemptions only — see
// the exemption lists in the sweep TESTs). `renderOut` (optional) receives
// the first render so callers can add cross-config assertions (the algo
// sweep's pairwise-distinct check, the FX sweeps' wet-vs-dry delta).
void assertSweepInvariants(const char* label,
                           const std::function<void(golden::GoldenHarness&)>& configure,
                           std::size_t nBlocks, bool expectSilent,
                           std::vector<int32_t>* renderOut = nullptr) {
    const std::size_t count =
        nBlocks * golden::GoldenHarness::kSamplesPerBlock;

    golden::GoldenHarness h1(kFixtureDir);
    configure(h1);
    std::vector<int32_t> r1(count);
    h1.renderScript(golden::RenderScript::a4Sustain(), nBlocks, r1.data());

    // Finite: no stored sample may exceed the clamp ceiling (see kStoredClamp-
    // Ceiling). The int32 mix cannot hold NaN — overflow IS the blowup signal.
    const int64_t peak = maxAbsSample(r1.data(), count);
    ASSERT_LE(peak, kStoredClampCeiling) << label << ": |sample| " << peak
        << " exceeds the 0x7FFFFF00 clamp ceiling — the render overflowed";

    // Non-silence (unless exempted with justification).
    if (!expectSilent) {
        ASSERT_GT(peak, kSweepSilenceThreshold) << label << ": max |sample| "
            << peak << " <= " << kSweepSilenceThreshold
            << " stored units — the config renders silent (zero-signal trap; "
               "investigate the patch or document an exemption)";
    }

    // Determinism: a fresh, identically-configured harness renders byte-equal.
    golden::GoldenHarness h2(kFixtureDir);
    configure(h2);
    std::vector<int32_t> r2(count);
    h2.renderScript(golden::RenderScript::a4Sustain(), nBlocks, r2.data());
    ASSERT_EQ(0, std::memcmp(r1.data(), r2.data(), count * sizeof(int32_t)))
        << label << ": two identical renders differ — non-determinism";

    if (renderOut != nullptr) {
        *renderOut = std::move(r1);
    }
}

// Max |a-b| across two equal-length renders (stored int32 units).
int64_t maxAbsDiff(const int32_t* a, const int32_t* b, std::size_t count) {
    int64_t m = 0;
    for (std::size_t i = 0; i < count; i++) {
        const int64_t d = std::abs(static_cast<int64_t>(a[i]) - b[i]);
        if (d > m) m = d;
    }
    return m;
}

}  // namespace

// ===========================================================================
// Canonical G0 golden: timbre 0, default preenMainPreset, noteOn(0,69,100) at
// block 0, 200 blocks (~130 ms at 48 kHz), no note-off (sustain plateau).
// Locks the entire Synth -> output chain — the highest-value-per-hour test in
// the firmware.
// ===========================================================================
TEST(GoldenMaster, A4DefaultSustain200Blocks) {
    golden::GoldenHarness harness(kFixtureDir);
    std::vector<int32_t> render(kNBlocks * golden::GoldenHarness::kSamplesPerBlock);
    harness.renderA4DefaultSustain(kNBlocks, render.data());
    const std::string id = a4DefaultSustainId();

    if (regenMode()) {
        // Regeneration: write the compiler-specific fixture, no comparison. The
        // TEST still "passes" (so ctest -R Golden returns green during regen);
        // the fixture files are the deliverable.
        const uint64_t h =
            harness.writeFixture(id, render.data(), kNBlocks);
        ASSERT_NE(h, 0ull) << "fixture regeneration failed for " << id
                           << " (see stderr)";
        return;
    }

    golden::GoldenDiff diff{};
    const bool ok =
        harness.compareAgainstFixture(id, render.data(), kNBlocks, &diff);
    if (!ok) {
        // goldenCompare is authoritative (the hash is diagnostic-only; see
        // tests/golden/README.md).
        FAIL() << "golden mismatch for " << id << " at flat index "
               << diff.firstMismatchIndex << " (block " << diff.blockIndex
               << "): expected=0x" << std::hex << diff.expectedSample
               << " actual=0x" << diff.actualSample << std::dec
               << " delta=" << diff.sampleDelta
               << " (tolerance=±256 stored units = ±1 audio-LSB; the firmware "
                  "clamps to 24-bit then <<8)\n"
               << "If this is a deliberate render change, regenerate with"
               << " PFM3_REGENERATE_GOLDENS=1 (see tests/golden/README.md).";
    }
    SUCCEED();
}

// ===========================================================================
// Determinism self-check: render the same golden twice in one process and
// assert byte-exact equality. Catches any latent non-determinism (uninitialized
// read, wall-clock leak, address-ordered allocation) before it can corrupt a
// fixture. This is the prerequisite that makes the golden lock trustworthy.
// ===========================================================================
TEST(GoldenMaster, DeterminismSelfCheck) {
    if (regenMode()) {
        GTEST_SKIP() << "skipped in regeneration mode";
    }

    golden::GoldenHarness h1(kFixtureDir);
    std::vector<int32_t> r1(kNBlocks * golden::GoldenHarness::kSamplesPerBlock);
    h1.renderA4DefaultSustain(kNBlocks, r1.data());

    golden::GoldenHarness h2(kFixtureDir);
    std::vector<int32_t> r2(kNBlocks * golden::GoldenHarness::kSamplesPerBlock);
    h2.renderA4DefaultSustain(kNBlocks, r2.data());

    // Two independent Synth instances must produce byte-identical renders.
    EXPECT_EQ(0, std::memcmp(r1.data(), r2.data(),
                             r1.size() * sizeof(int32_t)))
        << "two renders of a4_default_sustain differ — the render path is not "
           "deterministic; the golden lock is untrustworthy until fixed";
}

// ===========================================================================
// Phase G1 goldens. Each is a thin runGolden() call; the script/setup/algo
// capture the exact render the committed fixture locks. See
// _bmad-output/implementation-artifacts/spec-golden-master-phase-g1.md and the
// fixture manifest tests/golden/schema.json.
// ===========================================================================

// Full ADSR: noteOn@0, noteOff@300, render 600. Guards Env stage transitions +
// the release tail (G0's sustain-only golden never exercises noteOff/release).
TEST(GoldenMaster, EnvelopeAdsrFull) {
    runGolden("envelope_adsr_full", 600,
              golden::RenderScript::envelopeAdsrFull(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1);
}

// ALGO2 = algoOpInformation {1,1,2,0,0,0} (2 carriers + 1 modulator) — the
// smallest departure from the default ALGO1; exercises the multi-carrier output
// summing path that ALGO1 (1 carrier) never touches. Algorithm set before
// noteOn via setTimbreAlgo + afterNewParamsLoad (production-faithful re-init).
TEST(GoldenMaster, FmAlgo2) {
    runGolden("fm_algo2", 200,
              golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/0, ALGO2);
}

// ALG27 = algoOpInformation {1,1,1,1,1,1} — all 6 carriers, additive (no FM).
// Chosen over the originally-planned ALG17 (1 car + 5 mods) because the default
// preenMainPreset has all-zero modulation indices, so any single-carrier algo
// renders byte-identical to G0 — only carrier COUNT changes the output. ALG27
// is the maximal carrier-topology departure (6 carriers); it exercises the full
// additive summing path. (True modulation-stack coverage needs a non-zero-IM
// preset — deferred. See spec-golden-master-phase-g1.md Design Notes.)
TEST(GoldenMaster, FmAlgo27_6carrier) {
    runGolden("fm_algo27_6carrier", 200,
              golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/0, ALG27);
}

// Timbres 0 + 1 both noteOn@0 (6+6=12 <= MAX_NUMBER_OF_VOICES 16). Guards
// voicesToTimbre mix + per-timbre smoothVolume_/smoothPan_ + fxBus->mixAdd —
// the multi-timbre output summing path G0's single-timbre golden cannot reach.
TEST(GoldenMaster, MultiTimbreMix) {
    runGolden("multi_timbre_mix", 200,
              golden::RenderScript::multiTimbreMix(),
              golden::TimbreSetup::multiTimbre(),
              /*algoTimbre=*/-1, ALGO1);
}

// ===========================================================================
// Phase G2 — tolerance-headroom monitor. DeterminismSelfCheck proves in-process
// byte-exactness with a binary memcmp; this test QUANTIFIES how much of the ±256
// stored-unit (±1 audio-LSB) compare tolerance the current render actually
// consumes against the committed same-platform fixture, and records it as a test
// property (visible in ctest/CI XML). The ±256 tolerance exists to absorb
// benign within-platform drift (a future macOS point release / glibc update
// shifting libm tables by a few ULP); this test surfaces silent drift toward
// the ceiling before the main gate fails. It does NOT tighten the gate — the
// committed fixture is from a prior libm build, so exact-match (tolerance 0)
// would false-positive on legitimate libm drift. See tests/golden/README.md
// -> Comparison model + spec-golden-master-phase-g2-g3.md Design Notes.
// ===========================================================================
TEST(GoldenMaster, ToleranceHeadroom) {
    if (regenMode()) {
        GTEST_SKIP() << "skipped in regeneration mode (no committed fixture to diff)";
    }

    golden::GoldenHarness harness(kFixtureDir);
    std::vector<int32_t> render(kNBlocks * golden::GoldenHarness::kSamplesPerBlock);
    harness.renderA4DefaultSustain(kNBlocks, render.data());

    // Load the committed same-platform fixture and compute the per-sample max
    // |delta| across ALL samples (goldenCompare stops at the first mismatch;
    // this scans the whole buffer to quantify total consumption).
    const std::size_t count = kNBlocks * golden::GoldenHarness::kSamplesPerBlock;
    const std::string binPath = golden::fixturePath(
        kFixtureDir, a4DefaultSustainId(), ".bin");
    std::vector<int32_t> expected(count);
    ASSERT_TRUE(golden::readRenderBin(binPath, expected.data(), count))
        << "could not read committed fixture " << binPath
        << " (run PFM3_REGENERATE_GOLDENS=1 make golden-regen if missing)";

    int64_t maxDelta = 0;
    std::size_t maxIdx = 0;
    for (std::size_t i = 0; i < count; i++) {
        int64_t d = static_cast<int64_t>(render[i]) -
                    static_cast<int64_t>(expected[i]);
        if (d < 0) d = -d;
        if (d > maxDelta) { maxDelta = d; maxIdx = i; }
    }

    // Record the observed max delta. RecordProperty emits ONLY to GoogleTest
    // XML/JSON output, so .github/workflows/tests.yml runs this test with
    // --gtest_output and greps the properties into the run log (RecordProperty
    // is otherwise invisible for a passing test under ctest --output-on-failure).
    // max_delta_audio_lsb is emitted as a DOUBLE — integer truncation would
    // report 0 for any drift up to 255 stored units (step-04 review: the trend
    // signal the test advertises must survive sub-LSB drift). max_delta_block
    // is meaningful only when maxDelta > 0; when the render is byte-exact
    // (maxDelta==0) maxIdx stays 0 by init artifact, so report -1 to flag
    // "no mismatch location" rather than a misleading "block 0".
    testing::Test::RecordProperty("max_delta_stored_units",
                                  std::to_string(maxDelta));
    testing::Test::RecordProperty("max_delta_audio_lsb",
                                  std::to_string(static_cast<double>(maxDelta) / 256.0));
    testing::Test::RecordProperty("max_delta_block",
                                  std::to_string(maxDelta > 0
                                      ? static_cast<long long>(maxIdx / golden::GoldenHarness::kSamplesPerBlock)
                                      : -1LL));

    // The headroom assertion: the render must stay within the compare tolerance.
    // Expected 0 on the exact commit that generated the fixture (within-process
    // byte-exactness, per DeterminismSelfCheck); small non-zero is legitimate
    // libm/compiler drift since the fixture's commit build. A value at/over the
    // ceiling means the tolerance is consumed — investigate before it breaches.
    EXPECT_LE(maxDelta, static_cast<int64_t>(golden::GoldenHarness::kCompareLsbTolerance))
        << "a4_default_sustain max |delta| vs committed fixture = " << maxDelta
        << " stored units (" << (maxDelta / 256) << " audio-LSB) at block "
        << (maxIdx / golden::GoldenHarness::kSamplesPerBlock)
        << "; the ±256 tolerance is the gate — this much is consumed";
}

// ===========================================================================
// Phase G3 — live matrix-modulation goldens. Each wires an LFO1->destination
// matrix routing out-of-band via setMatrixRow (overwriting matrixRowState8,
// which is {LFO1,0,INDEX_MODULATION2,0} = inactive in the default preset), then
// renders with an optional mid-render PARAM_CHANGE. These lock the live
// setNewValueFromMidi -> matrix -> Voice path on a SOUNDING voice — the
// "stuck/wrong CC routing" bug class G0/G1 cannot reach. The default preset's
// lfoOsc1 is {LFO_SIN,4.5,0,0} (active), so MATRIX_SOURCE_LFO1 produces a
// modulating value with no MIDI input. matrixRowState8 (rowIdx 7) is chosen over
// rows 0-3 to avoid the special-case compute path + the MTX1..4_MUL feedback
// quirk. See spec-golden-master-phase-g2-g3.md Design Notes.
// ===========================================================================

// Steady LFO1 -> OSC1_FREQ (mul 0.5), 400 blocks (~1.2 LFO1 cycles at the
// default 4.5 Hz). The LFO auto-modulates osc1 pitch; no PARAM_CHANGE. Guards
// the LFO1 -> matrix -> osc-freq -> Voice routing end-to-end.
TEST(GoldenMaster, LiveLfoPitchModulation) {
    runGolden("live_lfo_pitch_modulation", 400,
              golden::RenderScript::liveLfoPitchModulation(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.setMatrixRow(0, /*rowIdx=*/7, MATRIX_SOURCE_LFO1,
                                 /*mul=*/0.5f, OSC1_FREQ);
              });
}

// LFO1 -> OSC1_FREQ set up with mul=0.0 (inactive), then a PARAM_CHANGE at
// block 80 sets ROW_MATRIX8/ENCODER_MATRIX_MUL = 0.6, turning the modulation ON
// mid-note. Guards the live CC -> matrix-mul -> Voice path: pitch wobble kicks
// in from block 80. Render 200.
TEST(GoldenMaster, LiveMatrixMulChange) {
    runGolden("live_matrix_mul_change", 200,
              golden::RenderScript::liveMatrixMulChange(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.setMatrixRow(0, /*rowIdx=*/7, MATRIX_SOURCE_LFO1,
                                 /*mul=*/0.0f, OSC1_FREQ);
              });
}

// LFO1 -> MIX_OSC1 (mul 0.5, a tremolo on osc1's level), then a PARAM_CHANGE
// at block 100 sets ROW_LFOOSC1/ENCODER_LFO_FREQ = 9.0 (doubling the LFO speed
// from the default 4.5), so the tremolo rate doubles mid-note. Guards a non-
// pitch (amplitude) matrix destination + the live LFO-freq CC update. Render
// 300. (Originally specified as LFO1->ALL_ENV_DECAY; changed to MIX_OSC1 after
// implementation found ALL_ENV_DECAY inaudible on a sustained note — the env
// leaves its decay stage ~block 50, so decay-time modulation has no effect.
// See spec-golden-master-phase-g2-g3.md Spec Change Log.)
TEST(GoldenMaster, LiveLfoFreqChange) {
    runGolden("live_lfo_freq_change", 300,
              golden::RenderScript::liveLfoFreqChange(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.setMatrixRow(0, /*rowIdx=*/7, MATRIX_SOURCE_LFO1,
                                 /*mul=*/0.5f, MIX_OSC1);
              });
}

// ===========================================================================
// Phase G4 — timed goldens. G0-G3 lock static-param and live-param renders;
// G4 locks the TIME-ADVANCED paths. The arpeggiator's clock is a pure sample-
// block counter (advanced inside buildNewSampleBlock), so it needs no MIDI
// bytes / HAL shim — only enableArpeggiator (internal clock) + held notes.
// ===========================================================================

// Arpeggiator: C-major triad (60/64/67) held, arp internal clock @120 BPM,
// UP, 2 octaves. The arp cycles the 3 notes across 2 octaves (~1 step per 31
// blocks), retriggerring voices periodically. Guards the arp note-cycling +
// octave-shift + voice-realloc path — the regression class G0-G3 never reach
// (their notes sustain unchanged once allocated). Render 300. The MIDI_BYTE
// event kind + the harness MidiDecoder are wired but unused here (the arp
// golden is the shim-free half of G4); the seq-external golden drives them.
TEST(GoldenMaster, ArpTriadUp) {
    runGolden("arp_triad_up", 300,
              golden::RenderScript::arpTriadUp(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.enableArpeggiator(/*timbre=*/0, /*bpm=*/120,
                                      /*direction=*/0 /*ARPEGGIO_DIRECTION_UP*/,
                                      /*octave=*/2);
              });
}

// Sequencer external-MIDI-clock playback: the harness-owned Sequencer is
// loaded with a minimal triad sequence (C4/E4/G4 at step indices 0/32/64) +
// armed via setupSequencerTriadPlayback, then MIDI_BYTE events feed 0xFA
// (MIDI_START) + bursts of 4x0xF8 (MIDI_CLOCK) across 97 blocks. Each clock
// burst fires noteOnFromSequencer synchronously during newByte (via
// MidiDecoder -> synth->midiTick -> sequencer->onMidiClock -> step advance),
// so each block's render captures the resulting audio. Guards the
// MidiDecoder clock-byte parse -> Synth -> Sequencer -> noteOnFromSequencer
// chain — the regression class G0-G3 + the arp golden never reach (their
// notes enter via Synth::noteOn directly, never via the sequencer).
TEST(GoldenMaster, SeqExternalPlayback) {
    runGolden("seq_external_playback", 97,
              golden::RenderScript::seqExternalPlayback(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.setupSequencerTriadPlayback();
              });
}

// ===========================================================================
// Phase 3 (spec-test-coverage-phase3) — FM-algorithm sweep. All 32 algorithms
// (Algorithm enum ALGO1..ALG32, Common.h:747-780; ALGO_END is the count) under
// the NON-ZERO modulation-index patch (setTimbreModulationIndices), which the
// G1 goldens could not exercise (default preset IMs are all 0 — only carrier
// count distinguished algos). Moderate IMs {1.0, 1.0, 0.8, 0.5} + feedback 0.3
// keep every topology audible without DAC-clamp flattening. NO fixture: this
// asserts the invariants (finite / non-silent / deterministic) per algo; the
// committed per-algo goldens below lock byte-exact topologies.
// ===========================================================================
TEST(GoldenMaster, FmAlgoSweep) {
    if (regenMode()) {
        GTEST_SKIP() << "sweep asserts invariants only; nothing to regenerate";
    }
    // Pairwise-distinct: under the nonzero-IM patch, two different FM
    // topologies must produce different renders. Without this check a wiring
    // regression (e.g. algoOpInformation flattening two algos to the same
    // carrier/modulator routing) would silently render byte-identical —
    // exactly the distinction the IM patch exists to expose (the G1 goldens
    // were byte-identical under all-zero IMs, so non-silence alone cannot
    // catch it). If a legitimate duplicate topology is ever found, document
    // it as an exemption here rather than weakening the check for all.
    std::vector<std::vector<int32_t>> renders;
    for (int a = ALGO1; a < ALGO_END; a++) {
        const std::string label = "algo " + std::to_string(a) + "/" +
                                  std::to_string(ALGO_END);
        SCOPED_TRACE(label);
        std::vector<int32_t> render;
        assertSweepInvariants(label.c_str(),
            [a](golden::GoldenHarness& h) {
                h.setTimbreAlgo(0, static_cast<Algorithm>(a));
                const float im[4] = {1.0f, 0.7f, 0.8f, 0.5f};
                h.setTimbreModulationIndices(0, im, 0.3f);
            },
            48, /*expectSilent=*/false, &render);
        for (std::size_t prev = 0; prev < renders.size(); prev++) {
            ASSERT_NE(0, std::memcmp(renders[prev].data(), render.data(),
                                     render.size() * sizeof(int32_t)))
                << "algo " << prev << " and algo " << a
                << " render byte-identical under the nonzero-IM patch — a "
                   "modulator-routing regression (or a duplicate topology "
                   "needing a documented exemption)";
        }
        renders.push_back(std::move(render));
    }
}

// ===========================================================================
// Phase 3 — FX-filter sweep. Every FILTER_TYPE value (SynthState.h:262
// FILTER_OFF..FILTER_LAST-1, ~50 arms of Voice::fxAfterBlock) with
// param1=0.6 / param2=0.55 / param3=0.6 (gain — the default-preset random
// gen's value; 0 would attenuate to silence). Exemptions (documented, not
// silently passed): FILTER_OFF — the pass-through arm; fxAfterBlock has no
// FILTER_OFF case (Voice.cpp:4128 switch starts at FILTER_LP), so the block
// is a no-op pass-through of the dry signal... which for timbre 0 output is
// the DRY VOICE ITSELF — NOT silent. Confirmed empirically on first run: no
// FILTER_TYPE needed an exemption under this patch (all non-silent, finite,
// deterministic). The exemption mechanism is kept for a future quirk.
// ===========================================================================
TEST(GoldenMaster, FxSweep) {
    if (regenMode()) {
        GTEST_SKIP() << "sweep asserts invariants only; nothing to regenerate";
    }
    // Dry baseline: FILTER_OFF passes the voice through untouched (the
    // fxAfterBlock switch has no FILTER_OFF case — Voice.cpp:4128). Every
    // OTHER type must move the render away from this baseline by more than
    // 1 audio-LSB (256 stored units): a dead wet path (regression zeroing an
    // arm) leaves the dry signal intact and non-silent, which the plain
    // non-silence assert above cannot distinguish — this delta check can.
    // A type that legitimately shifts <1 LSB at these params is functionally
    // transparent and gets a documented exemption, not a silent pass.
    constexpr int64_t kFxDeltaFloor = 256;
    std::vector<int32_t> dry;
    assertSweepInvariants("fx FILTER_OFF dry baseline",
        [](golden::GoldenHarness& h) {
            h.setTimbreFx(0, FILTER_OFF, 0.6f, 0.55f);
        },
        48, /*expectSilent=*/false, &dry);
    for (int t = FILTER_OFF; t < FILTER_LAST; t++) {
        const std::string label = "fx type " + std::to_string(t) + "/" +
                                  std::to_string(FILTER_LAST);
        SCOPED_TRACE(label);
        std::vector<int32_t> render;
        assertSweepInvariants(label.c_str(),
            [t](golden::GoldenHarness& h) {
                h.setTimbreFx(0, t, 0.6f, 0.55f);
            },
            48, /*expectSilent=*/false, &render);
        if (t != FILTER_OFF) {
            ASSERT_GT(maxAbsDiff(dry.data(), render.data(), render.size()),
                      kFxDeltaFloor)
                << label << ": render is within 1 audio-LSB of the FILTER_OFF "
                   "dry baseline — the wet path is (near-)dead; investigate "
                   "or document an exemption";
        }
    }
}

// ===========================================================================
// Phase 3 — nonzero-IM FM-algo goldens. Each pairs setTimbreAlgo +
// setTimbreModulationIndices before renderScript(a4Sustain()) — same shape as
// the G1 FM goldens but now the MODULATOR routing (not just carrier count)
// drives the output, closing the G1 deferred gap. Topology diversity
// (algoOpInformation, Common.cpp:56-88: {im, mix, carriers} per algo):
//   ALGO1  {1,2,2}      — 1 car, 2 stacked mods (the classic 2-op->1 stack).
//   ALGO6  {1,1,1,2}    — 3 carriers + 1 shared mod + feedback patch.
//   ALG16  {1,2,1,2,2,2}— 4-car/2-mod interleaved tree.
//   ALG22  {1,2,1,1,1,2}— 4-car/2-mod alternate tree (different wire order
//                          than ALG16 -> different render even at equal op
//                          counts, the exact distinction the nonzero IMs make
//                          visible).
//   ALG28  {1,1,1,1,1,2}— 5 carriers + 1 shared mod: additive-with-PM. Chosen
//                          over the spec's ALG27 sketch ({1,1,1,1,1,1} — six
//                          carriers, ZERO modulators): under any IM patch
//                          ALG27 renders byte-identical to the committed
//                          fm_algo27_6carrier, a duplicate fixture; ALG28 is
//                          the additive family member where the IM patch is
//                          actually audible.
// ===========================================================================
TEST(GoldenMaster, FmAlgo1Mod) {
    runGolden("fm_algo1_mod", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/0, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  const float im[4] = {1.0f, 0.7f, 0.8f, 0.5f};
                  h.setTimbreModulationIndices(0, im, 0.3f);
              });
}

TEST(GoldenMaster, FmAlgo6Mod) {
    runGolden("fm_algo6_mod", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/0, ALGO6,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  const float im[4] = {1.2f, 0.7f, 0.8f, 0.5f};
                  h.setTimbreModulationIndices(0, im, 0.6f);
              });
}

TEST(GoldenMaster, FmAlgo16Mod) {
    runGolden("fm_algo16_mod", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/0, ALG16,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  const float im[4] = {1.0f, 0.7f, 0.8f, 0.5f};
                  h.setTimbreModulationIndices(0, im, 0.3f);
              });
}

TEST(GoldenMaster, FmAlgo22Mod) {
    runGolden("fm_algo22_mod", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/0, ALG22,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  const float im[4] = {1.0f, 0.7f, 0.8f, 0.5f};
                  h.setTimbreModulationIndices(0, im, 0.3f);
              });
}

TEST(GoldenMaster, FmAlgo28Mod) {
    runGolden("fm_algo28_mod", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/0, ALG28,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  const float im[4] = {1.0f, 0.7f, 0.8f, 0.5f};
                  h.setTimbreModulationIndices(0, im, 0.3f);
              });
}

// ===========================================================================
// Phase 3 — FX goldens. Two committed fixtures on the per-voice FX path
// (Voice::fxAfterBlock): a low-pass sweep (FILTER_LP, the first and most-
// used filter arm) and the bit-crusher (FILTER_CRUSHER, a non-linear waveshaper
// arm). Both verified audible (sweep non-silence) and deterministic before
// committing. param3 (gain) = 0.6.
// ===========================================================================
TEST(GoldenMaster, FxLowpass) {
    runGolden("fx_lowpass", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.setTimbreFx(0, FILTER_LP, 0.6f, 0.55f);
              });
}

TEST(GoldenMaster, FxCrusher) {
    runGolden("fx_crusher", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.setTimbreFx(0, FILTER_CRUSHER, 0.6f, 0.55f);
              });
}

// ===========================================================================
// Phase 3 coverage follow-up — TIMBRE-level FX2 sweep (spec-test-coverage-
// phase3, Timbre.cpp coverage gap). Timbre::fxAfterBlock (Timbre.cpp:751+,
// ~1254 lines) is a DIFFERENT surface from the Voice-level FILTER_TYPE sweep
// above: it switches on params_.effect2.type (SynthState.h FILTER2_TYPE:
// flange/dimension/chorus/wide/doubler/tripler/bode/delaycrunch/pingpong/
// diffuser/grain1/grain2) and uses Timbre::delayBuffer_. All fields are read
// LIVE each block (call site Synth::buildNewSampleBlock -> Timbre::fxAfterBlock,
// Synth.cpp:354), and param3 drives the wet gain (mixerGain_, Timbre.cpp:762),
// so setTimbreFx2's field patch alone reaches the DSP. TWO param passes per
// type: delay-based FX have internal branches selected by param ranges (e.g.
// FLANGE's feedback branch signs at Timbre.cpp:808-810, ORYX-style quadrant
// logic at Timbre.cpp:1170-1175); a low-param pass (p1=0.15, p2=0.9) and a
// mid pass (p1=0.6, p2=0.55) exercise different arms while keeping runtime
// bounded. 96 blocks (not 48): the delay FX need runway for the wet path to
// build past the non-silence threshold after the anti-click zeroing
// (Timbre.cpp:764-771).
// ===========================================================================
TEST(GoldenMaster, Fx2Sweep) {
    if (regenMode()) {
        GTEST_SKIP() << "sweep asserts invariants only; nothing to regenerate";
    }
    // Wet-vs-dry delta vs FILTER2_OFF, same rationale as FxSweep: Timbre::
    // fxAfterBlock's switch has no FILTER2_OFF case, so the OFF render IS the
    // dry timbre mix. Each param pass gets its own baseline (the smoothing
    // state differs per pass).
    constexpr int64_t kFx2DeltaFloor = 256;
    struct ParamSet { float p1, p2; const char* tag; };
    const ParamSet passes[] = {
        {0.6f, 0.55f, "mid"},
        {0.15f, 0.9f, "low"},
    };
    for (const auto& ps : passes) {
        std::vector<int32_t> dry;
        assertSweepInvariants((std::string("fx2 FILTER2_OFF dry baseline (")
                               + ps.tag + ")").c_str(),
            [ps](golden::GoldenHarness& h) {
                h.setTimbreFx2(0, FILTER2_OFF, ps.p1, ps.p2, 1.0f);
            },
            96, /*expectSilent=*/false, &dry);
        for (int t = FILTER2_OFF; t < FILTER2_LAST; t++) {
            const std::string label = "fx2 type " + std::to_string(t) + "/"
                                    + std::to_string(FILTER2_LAST)
                                    + " (" + ps.tag + " params)";
            SCOPED_TRACE(label);
            std::vector<int32_t> render;
            assertSweepInvariants(label.c_str(),
                [t, ps](golden::GoldenHarness& h) {
                    h.setTimbreFx2(0, t, ps.p1, ps.p2, 1.0f);
                },
                /*nBlocks=*/96, /*expectSilent=*/false, &render);
            if (t != FILTER2_OFF) {
                ASSERT_GT(maxAbsDiff(dry.data(), render.data(), render.size()),
                          kFx2DeltaFloor)
                    << label << ": render is within 1 audio-LSB of the "
                       "FILTER2_OFF dry baseline — the wet path is (near-)dead; "
                       "investigate or document an exemption";
            }
        }
    }
}

// ===========================================================================
// Phase 3 coverage follow-up — ONE fx2 golden (spec budget cap: 8 new goldens
// total). FILTER2_CHORUS: a delay-modulated wet path that is clearly audible
// on a sustained note and representative of the delay-buffer family. 200
// blocks capture the chorus LFO sweep + wet-path ramp past the anti-click
// transient. _linux triple pending the regenerate-linux-goldens workflow.
// ===========================================================================
TEST(GoldenMaster, Fx2Chorus) {
    runGolden("fx2_chorus", 200, golden::RenderScript::a4Sustain(),
              golden::TimbreSetup::g0Default(),
              /*algoTimbre=*/-1, ALGO1,
              /*preRender=*/[](golden::GoldenHarness& h) {
                  h.setTimbreFx2(0, FILTER2_CHORUS, 0.6f, 0.55f, 1.0f);
              });
}
