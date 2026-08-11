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
// Determinism model: the render is fully reproducible on host. The HAL RNG is
// gated to seed 0 under PFM3_HOST (Synth.cpp:285) and the downstream noise[]
// fill loop is shared host/firmware (same LCG, same table). The smoothVolume_/
// smoothPan_ one-pole transient (blocks ~0-10) is deterministic and is part of
// the locked behavior — all 200 blocks are captured (no warm-start skip).

#include "golden_harness.h"

#include <cstdlib>
#include <cstring>
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
    return std::getenv("PFM3_REGENERATE_GOLDENS") != nullptr;
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

    if (regenMode()) {
        // Regeneration: write the fixture, no comparison. The TEST still
        // "passes" (so ctest -R Golden returns green during regen); the
        // fixture files are the deliverable.
        const uint64_t h =
            harness.writeFixture("a4_default_sustain", render.data(), kNBlocks);
        ASSERT_NE(h, 0ull) << "fixture regeneration failed (see stderr)";
        return;
    }

    golden::GoldenDiff diff{};
    const bool ok = harness.compareAgainstFixture(
        "a4_default_sustain", render.data(), kNBlocks, &diff);
    if (!ok) {
        if (diff.hashMismatch) {
            // hash-level mismatch (goldenCompare passed within tolerance);
            // the expected/actual hash values are printed to stderr by the harness.
            FAIL() << "golden hash mismatch for a4_default_sustain: "
                      "goldenCompare passed within tolerance but the "
                      "tolerance-normalized hash differs (expected/actual "
                      "hashes on stderr).\nIf this is a deliberate render "
                      "change, regenerate with PFM3_REGENERATE_GOLDENS=1 "
                      "(see tests/golden/README.md).";
        } else {
            FAIL() << "golden mismatch for a4_default_sustain at flat index "
                   << diff.firstMismatchIndex << " (block " << diff.blockIndex
                   << "): expected=0x" << std::hex << diff.expectedSample
                   << " actual=0x" << diff.actualSample << std::dec
                   << " delta=" << diff.sampleDelta << " (tolerance=±1 LSB)\n"
                   << "If this is a deliberate render change, regenerate with"
                   << " PFM3_REGENERATE_GOLDENS=1 (see tests/golden/README.md).";
        }
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
