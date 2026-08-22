// Host-side coverage for firmware/Src/synth/LfoStepSeq.cpp — the step-
// sequencer LFO (one of the 7 per-voice LFOs, wired as MATRIX_SOURCE_LFOSEQ1/2).
//
// Regression target (per test-coverage-plan.md Phase 1, row 2):
//   LFO step bugs. LfoStepSeq advances a 16-step sequencer whose step VALUES
//   are indices into the global expValues[16] ratio table, gates each step by
//   a gate length (param + a modulatable matrix destination), re-syncs to an
//   external MIDI clock (5 clock-division cases), and restarts on noteOn. The
//   emitted value lands in the caller's REAL Matrix source slot — a regression
//   in step advance/wrap, gate logic, or clock sync fails loudly here.
//
// Fixture notes (see tests/SEAM.md):
//   * Params/steps/Matrix are CALLER-OWNED by design (Voice.cpp:4069 init
//     pattern) — the fixture owns them on its frame, exactly like Voice does.
//   * LfoStepSeq's step engine privates (phase/phaseStep/target/currentValue/
//     gated) are NOT initialized by init()/noteOn() — the firmware relies on
//     the Voice pool's BSS zero-init. The fixture reproduces that with the
//     golden_harness memset+placement-new pattern (zeroed storage, then the
//     ctor re-establishes the vptr), so currentValue/target/phase start at 0
//     deterministically, matching firmware boot state.
//   * Private-state access (phase/phaseStep/gated asserts) via the scoped
//     `#define private public` include pattern from midi_decoder_test.cpp:
//     every header LfoStepSeq.h reaches is pre-included first, so the macro
//     affects ONLY the LfoStepSeq class body. Zero firmware surface.
//   * The global expValues[16] (LfoStepSeq.cpp) is declared extern and only
//     ever READ here — never mutated (shared-state hygiene, tests/SEAM.md).

// Pre-include the full closure LfoStepSeq.h reaches (Lfo.h ->
// SynthStateAware.h/SynthState.h + Matrix.h/Common.h) so the #define below
// re-parses only LfoStepSeq.h itself.
#include "Lfo.h"

#define private public  // NOLINT: scoped to LfoStepSeq.h only (see header)
#include "LfoStepSeq.h"
#undef private

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

// Global 16-entry exponential ratio table (LfoStepSeq.cpp:21). Read-only here.
extern float expValues[];

namespace {

// Mirrors PREENFM_FREQUENCY_INVERSED_LFO (Common.h): (1/Fs)*32 — the per-call
// (per 32-sample block) LFO phase increment scale.
constexpr float kInvLfo = (1.0f / 47916.0f) * 32.0f;

// One full step cycle at bpm=240 needs ~16/ (240*0.0667*kInvLfo) calls; run
// enough calls to cover a full 16-step wrap plus the catch-down ramp.
constexpr int kAdvanceCalls = 2200;

std::vector<float> CollapseRuns(const std::vector<float>& v) {
    std::vector<float> runs;
    for (float x : v) {
        if (runs.empty() || runs.back() != x) runs.push_back(x);
    }
    return runs;
}

class LfoStepSeqTest : public ::testing::Test {
protected:
    MatrixRowParams rows_[MATRIX_SIZE];
    Matrix matrix_;
    StepSequencerParams params_;
    StepSequencerSteps steps_;

    // Zeroed backing + placement-new: valid vptr, BSS-zero data members —
    // the firmware's Voice-pool initialization state.
    struct LfoStepSeqBacking {
        alignas(LfoStepSeq) unsigned char bytes[sizeof(LfoStepSeq)];
    };
    LfoStepSeqBacking backing_;
    LfoStepSeq* lfo_;

    void SetUp() override {
        std::memset(&backing_, 0, sizeof(backing_));
        lfo_ = new (&backing_) LfoStepSeq();

        std::memset(&params_, 0, sizeof(params_));
        std::memset(&steps_, 0, sizeof(steps_));
        std::memset(rows_, 0, sizeof(rows_));
        for (int i = 0; i < MATRIX_SIZE; i++) rows_[i].source = MATRIX_SOURCE_NONE;
        matrix_.init(rows_);
        matrix_.resetSources();
        matrix_.resetAllDestination();

        // Firmware defaults: bpm 120-ish, gate full-open (>= 1 -> un-gated),
        // steps 0..15.
        params_.bpm = 120.0f;
        params_.gate = 1.0f;
        for (int i = 0; i < 16; i++) steps_.steps[i] = (char)i;

        lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1,
                   LFOSEQ1_GATE);
    }

    float Source() { return matrix_.getSource(MATRIX_SOURCE_LFOSEQ1); }
};

// init -> valueChanged(0) converts bpm to the per-call phase increment:
// bpm * (4/60) * PREENFM_FREQUENCY_INVERSED_LFO. The formula is recomputed
// here so a drift in the constant chain is caught.
TEST_F(LfoStepSeqTest, InitComputesPhaseStepFromBpm) {
    EXPECT_NEAR(lfo_->phaseStep, 120.0f * 0.066666666666666f * kInvLfo, 1e-9f);
    // valueChanged on a non-rate encoder (>= 2) is a no-op.
    const float before = lfo_->phaseStep;
    lfo_->valueChanged(2);
    EXPECT_EQ(lfo_->phaseStep, before);
}

// With bpm in the MIDI-clock range (>= LFO_SEQ_MIDICLOCK_DIV_4 = 241),
// valueChanged leaves phaseStep alone — the clock owns the rate. (init's own
// midiClock(0,true) DID set phaseStep = 2*invTab[1536] for bpm 243; the
// encoder path must not overwrite it with a free-run value.)
TEST_F(LfoStepSeqTest, ValueChangedSkipsBpmAtOrAboveMidiClockRange) {
    params_.bpm = 243.0f;
    lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1, LFOSEQ1_GATE);
    EXPECT_FLOAT_EQ(lfo_->phaseStep, 2.0f * (1.0f / 1536.0f))
        << "init's midiClock seeded the clock-derived rate";
    lfo_->valueChanged(0);
    EXPECT_FLOAT_EQ(lfo_->phaseStep, 2.0f * (1.0f / 1536.0f))
        << "valueChanged must leave the clock-derived rate alone";
}

// THE step-advance lock: at gate >= 1 (un-gated) the sequencer emits
// expValues[step] for each step 0..15, then wraps to step 0. currentValue
// slews toward the new target by 1 per call, so after the wrap (target drops
// 15 -> 0) the emitted values walk DOWN one expValues index per call. The
// run-collapse of the emitted source pins the exact walk.
TEST_F(LfoStepSeqTest, AdvanceFollowsExpValuesSequenceAndWrapsAtLength) {
    params_.bpm = 240.0f;  // fastest free-run rate
    lfo_->valueChanged(0);
    lfo_->noteOn();  // phase=0, target=steps[0]=0

    std::vector<float> emitted;
    emitted.reserve(kAdvanceCalls);
    for (int i = 0; i < kAdvanceCalls; i++) {
        lfo_->nextValueInMatrix();
        emitted.push_back(Source());
    }
    const auto runs = CollapseRuns(emitted);
    ASSERT_GT(runs.size(), 17u) << "no wrap: sequencer did not advance";
    for (int k = 0; k < 16; k++) {
        SCOPED_TRACE(k);
        EXPECT_FLOAT_EQ(runs[k], expValues[k])
            << "step " << k << " must emit expValues[" << k << "]";
    }
    // Wrap: step 15 -> step 0 drops the target to 0; currentValue slews DOWN
    // from 15, one index per call (this also covers the currentValue-- path):
    // runs[16..30] == expValues[14..0], then step 1 pulls it back up.
    ASSERT_GT(runs.size(), 31u);
    for (int k = 0; k <= 14; k++) {
        SCOPED_TRACE(k);
        EXPECT_FLOAT_EQ(runs[16 + k], expValues[14 - k]);
    }
    EXPECT_FLOAT_EQ(runs[31], expValues[1])
        << "step 1 of the second cycle re-opens the ascent";
}

// Gated behavior (0 < gate < 1): each step's value is held while the step's
// fractional phase is below the gate length, then the target drops to 0 (gate
// closed) until the next step re-opens it. The emitted source therefore
// alternates between expValues[step] and 0.
TEST_F(LfoStepSeqTest, GateBetweenZeroAndOneChopsEachStep) {
    params_.gate = 0.4f;
    for (int i = 0; i < 16; i++) steps_.steps[i] = (char)8;
    lfo_->valueChanged(0);
    lfo_->noteOn();

    float maxV = -1e9f, minV = 1e9f;
    int zeros = 0, fulls = 0;
    for (int i = 0; i < kAdvanceCalls; i++) {
        lfo_->nextValueInMatrix();
        const float v = Source();
        if (v > maxV) maxV = v;
        if (v < minV) minV = v;
        if (v == 0.0f) zeros++;
        if (v == expValues[8]) fulls++;
    }
    EXPECT_FLOAT_EQ(maxV, expValues[8]) << "step value 8 must be reached";
    EXPECT_FLOAT_EQ(minV, 0.0f) << "gate must close (target 0)";
    EXPECT_GT(zeros, 50) << "gate-closed runs must occur repeatedly";
    EXPECT_GT(fulls, 50) << "gate-open runs must occur repeatedly";
}

// Gate <= 0 forces target 0 permanently (the gatePlusMatrix <= 0 branch).
// Driven from a pre-opened state (params are caller-owned; the firmware
// mutates them the same way via UI/NRPN).
TEST_F(LfoStepSeqTest, GateAtOrBelowZeroForcesTargetZero) {
    for (int i = 0; i < 16; i++) steps_.steps[i] = (char)15;
    lfo_->valueChanged(0);
    lfo_->noteOn();
    for (int i = 0; i < 300; i++) lfo_->nextValueInMatrix();
    ASSERT_FLOAT_EQ(Source(), expValues[15]) << "precondition: reached 15";

    params_.gate = 0.0f;  // gate closes
    for (int i = 0; i < 300; i++) lfo_->nextValueInMatrix();
    EXPECT_FLOAT_EQ(Source(), 0.0f) << "gate<=0 must force target 0";
    for (int i = 0; i < 100; i++) lfo_->nextValueInMatrix();
    EXPECT_FLOAT_EQ(Source(), 0.0f) << "and it must STAY 0";
}

TEST_F(LfoStepSeqTest, HostileStepCharsClampIntoExpValuesDomain) {
    // Regression (6.2): steps[] holds raw chars from the preset; the UI
    // clamps to [0,15] but a corrupt bank does not. A hostile char (<0 or
    // >15) used to read expValues[] out of bounds. The step VALUE is now
    // clamped at the use site — every emitted source stays in the table.
    const int hostile[] = {100, -5, 127, -128};
    for (int h : hostile) {
        SCOPED_TRACE(h);
        for (int i = 0; i < 16; i++) steps_.steps[i] = static_cast<int8_t>(h);
        lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1,
                   LFOSEQ1_GATE);
        lfo_->noteOn();
        for (int i = 0; i < kAdvanceCalls; i++) lfo_->nextValueInMatrix();
        // Walked all the way to the clamped target and never left the table.
        int expected = h < 0 ? 0 : 15;
        EXPECT_FLOAT_EQ(Source(), expValues[expected])
            << "hostile step " << h << " must clamp to expValues[" << expected
            << "]";
        // Intermediate catch-down walk: every intermediate source is a table
        // entry (indices are ints, so compare against the full table).
        bool inTable = false;
        for (int t = 0; t < 16; t++) {
            if (Source() == expValues[t]) inTable = true;
        }
        EXPECT_TRUE(inTable) << "emitted value must always be in expValues[]";
    }
}

// The gate length is modulated by the matrix destination the LFO was inited
// with (matrixGateDestination = LFOSEQ1_GATE): gate + destination is the
// effective gate. Routing 0.3 into LFOSEQ1_GATE with param gate 0.2 must chop
// steps exactly like a plain 0.5 gate does.
TEST_F(LfoStepSeqTest, GateLengthIsModulatedByMatrixDestination) {
    params_.gate = 0.2f;
    for (int i = 0; i < 16; i++) steps_.steps[i] = (char)12;
    rows_[4].source = MATRIX_SOURCE_MODWHEEL;
    rows_[4].mul = 0.15f;
    rows_[4].dest1 = LFOSEQ1_GATE;
    rows_[4].dest2 = (DestinationEnum)0;  // unused sink
    matrix_.setSource(MATRIX_SOURCE_MODWHEEL, 2.0f);  // -> dest 0.3
    matrix_.computeAllDestinations();
    ASSERT_NEAR(matrix_.getDestination(LFOSEQ1_GATE), 0.3f, 1e-6f);

    lfo_->valueChanged(0);
    lfo_->noteOn();
    float maxV = -1e9f, minV = 1e9f;
    for (int i = 0; i < kAdvanceCalls; i++) {
        matrix_.computeAllDestinations();
        lfo_->nextValueInMatrix();
        const float v = Source();
        if (v > maxV) maxV = v;
        if (v < minV) minV = v;
    }
    EXPECT_FLOAT_EQ(maxV, expValues[12]) << "effective gate 0.5 must open";
    EXPECT_FLOAT_EQ(minV, 0.0f) << "effective gate 0.5 must close";
}

// midiClock(songPosition, computeStep) re-syncs both the step phase and the
// step rate for all 5 clock-division bpm settings. Realistic args follow the
// firmware callers (Voice::midiClockSongPositionStep passes the incremented
// songPosition with computeStep=true on even steps; midiClockContinue passes
// false). ticks is 0 right after init (init's own midiClock(0,true) consumed
// the seeded 1536), so phaseStep lands on factor * invTab[0] = factor.
TEST_F(LfoStepSeqTest, MidiClockResyncsPhaseAndRatePerDivision) {
    struct Case {
        float bpm;
        float expectedPhaseAtSp8;
        float expectedFactor;
    };
    const Case cases[] = {
        {241.0f, (8 & 0x3f) * 0.25f, 0.5f},   // LFO_SEQ_MIDICLOCK_DIV_4
        {242.0f, (8 & 0x1f) * 0.5f, 1.0f},    // LFO_SEQ_MIDICLOCK_DIV_2
        {243.0f, (float)(8 & 0xF), 2.0f},     // LFO_SEQ_MIDICLOCK
        {244.0f, (float)((8 << 1) & 0xF), 4.0f},  // _TIME_2
        {245.0f, (float)((8 << 1) & 0xF), 8.0f},  // _TIME_4
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.bpm);
        params_.bpm = c.bpm;
        lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1,
                   LFOSEQ1_GATE);
        lfo_->midiClock(8, true);
        EXPECT_NEAR(lfo_->phase, c.expectedPhaseAtSp8, 1e-6f);
        EXPECT_FLOAT_EQ(lfo_->phaseStep, c.expectedFactor);
    }
}

// FIXED (spec 4.5): corrupt/unmatched sync bpm values (246-255, e.g. from a
// bad bank) previously matched NO switch arm in midiClock, leaving a zero or
// stale phaseStep. The default arm snaps them to TIME_4 (245) behavior —
// the nearest supported division.
TEST_F(LfoStepSeqTest, MidiClockCorruptBpmFallsBackToTime4) {
    for (float bpm : {246.0f, 250.0f, 255.0f}) {
        SCOPED_TRACE(bpm);
        params_.bpm = bpm;
        lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1,
                   LFOSEQ1_GATE);
        lfo_->midiClock(8, true);
        EXPECT_NEAR(lfo_->phase, (float)((8 << 1) & 0xF), 1e-6f)
            << "phase must snap like TIME_4";
        EXPECT_FLOAT_EQ(lfo_->phaseStep, 8.0f)
            << "rate must snap like TIME_4";
    }
}

// FIXED (spec folded-B): a NaN/-Inf/+Inf bpm previously hit the UB
// float->int cast in midiClock's switch dispatch. Non-finite values now
// fail-safe to the TIME_4 arm before the cast — same behavior as the
// unmatched 246-255 corrupt values above.
// Review patch: finite-but-out-of-int-range bpms (1e30f, -1e30f, 300) are
// still UB in the (int) cast — the guard now covers the [0,255] enum domain,
// so they fail-safe to the TIME_4 arm like the NaN/Inf cases above.
TEST_F(LfoStepSeqTest, MidiClockOutOfRangeFiniteBpmFallsBackToTime4) {
    for (float bpm : {1e30f, -1e30f, 300.0f}) {
        SCOPED_TRACE(bpm);
        params_.bpm = bpm;
        lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1,
                   LFOSEQ1_GATE);
        lfo_->midiClock(8, true);
        EXPECT_NEAR(lfo_->phase, (float)((8 << 1) & 0xF), 1e-6f)
            << "phase must snap like TIME_4";
        EXPECT_FLOAT_EQ(lfo_->phaseStep, 8.0f)
            << "rate must snap like TIME_4";
    }
}

TEST_F(LfoStepSeqTest, MidiClockNonFiniteBpmFallsBackToTime4) {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (float bpm : {inf, -inf, nan}) {
        SCOPED_TRACE(bpm);
        params_.bpm = bpm;
        lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1,
                   LFOSEQ1_GATE);
        lfo_->midiClock(8, true);
        EXPECT_NEAR(lfo_->phase, (float)((8 << 1) & 0xF), 1e-6f)
            << "phase must snap like TIME_4";
        EXPECT_FLOAT_EQ(lfo_->phaseStep, 8.0f)
            << "rate must snap like TIME_4";
    }
}

// Odd songPositions are ignored by every division case ((songPosition & 1)
// must be 0) — nothing moves. And with computeStep=false (midiClockContinue),
// the phase still snaps but the rate (phaseStep) is NOT recomputed.
TEST_F(LfoStepSeqTest, MidiClockOddPositionIsNoOpAndContinueKeepsRate) {
    params_.bpm = 243.0f;
    lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1, LFOSEQ1_GATE);
    lfo_->midiClock(8, true);
    const float phaseBefore = lfo_->phase;
    const float stepBefore = lfo_->phaseStep;

    lfo_->midiClock(9, true);  // odd: ignored
    EXPECT_EQ(lfo_->phase, phaseBefore);
    EXPECT_EQ(lfo_->phaseStep, stepBefore);

    lfo_->midiClock(4, false);  // even, no compute: phase snaps, rate kept
    EXPECT_FLOAT_EQ(lfo_->phase, 4.0f);
    EXPECT_EQ(lfo_->phaseStep, stepBefore);
}

// noteOn restarts a free-running sequencer at step 0 (phase=0,
// target=steps[0]); a midi-synced one (bpm >= 241) is NOT restarted — the
// clock owns the phase.
TEST_F(LfoStepSeqTest, NoteOnRestartsFreeRunButNotMidiSynced) {
    params_.bpm = 240.0f;
    lfo_->valueChanged(0);
    lfo_->noteOn();
    for (int i = 0; i < 500; i++) lfo_->nextValueInMatrix();
    ASSERT_GT(lfo_->phase, 1.0f) << "precondition: advanced past step 0";

    lfo_->noteOn();
    EXPECT_FLOAT_EQ(lfo_->phase, 0.0f);
    EXPECT_EQ(lfo_->target, 0);

    params_.bpm = 243.0f;
    lfo_->init(&params_, &steps_, &matrix_, MATRIX_SOURCE_LFOSEQ1, LFOSEQ1_GATE);
    lfo_->midiClock(8, true);
    const float phaseAt8 = lfo_->phase;
    lfo_->noteOn();
    EXPECT_FLOAT_EQ(lfo_->phase, phaseAt8)
        << "midi-synced sequencer must not restart on noteOn";
}

// noteOff is an empty stub — call it so the line is covered and locked as a
// no-op.
TEST_F(LfoStepSeqTest, NoteOffIsNoOp) {
    params_.bpm = 240.0f;
    lfo_->valueChanged(0);
    lfo_->noteOn();
    lfo_->nextValueInMatrix();
    const float v = Source();
    const float phase = lfo_->phase;
    lfo_->noteOff();
    EXPECT_FLOAT_EQ(lfo_->phase, phase);
    EXPECT_FLOAT_EQ(Source(), v);
}

}  // namespace
