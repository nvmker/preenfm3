// Host-side coverage for firmware/Src/synth/LfoOsc.cpp — the 3 oscillator
// LFOs (MATRIX_SOURCE_LFO1..3), per-LfoType waveform generators.
//
// Regression target (per test-coverage-plan.md Phase 1, row 3):
//   LFO pitch/shape drift. LfoOsc advances a phase accumulator by
//   freq * PREENFM_FREQUENCY_INVERSED_LFO per call and emits, per LfoType:
//   triangle/saw/square closed forms, sine via the Osc wavetable (sinTable,
//   index phase*waveTables[0].max), and 4 noise families (S&H / brownian /
//   wandering / flow) that sample the GLOBAL noise[32] table on phase wrap.
//   All outputs land in the caller's REAL Matrix source slot. A regression in
//   the increment, a table-index change, a wrap change, or the random-sample
//   plumbing fails loudly here.
//
// Method: a REFERENCE MODEL in the test replicates the firmware's float ops
// in the same order (phase += freq*invLfo; wrap at >=1; shape formula; ramp
// scaling; += bias) and every emitted sample is asserted against it — the
// synth_math_test.cpp golden style, but derivable rather than baked, so a
// table/constant change shows up as a precise per-sample diff.
//
// Fixture notes (see tests/SEAM.md):
//   * Params + Matrix + initPhase pointer are caller-owned by design
//     (Voice.cpp:4060 init pattern) — the fixture owns them on its frame.
//   * valueChanged(0..3) is run after init, mirroring Voice::afterNewParamsLoad
//     (Voice.h:357). noteOn() then establishes a deterministic phase — the
//     firmware only reads the LFO once a note is playing.
//   * The random LFOs read the global noise[32] (Osc.cpp:25) ONLY on phase
//     wrap; noise[0] is SET EXPLICITLY per test (and changed mid-test to prove
//     the sample-and-hold timing). Shared-state hygiene: never rely on
//     leftover noise[] contents.
//   * FIXED (6.3): keybRamp <= -0.02 used to make the inline
//     valueChanged(ENCODER_LFO_KSYNC) compute invTab[(int)(keybRamp*50.0f)]
//     with a NEGATIVE index (LfoOsc.h) — an OOB global read. The index is
//     now clamped to the invTab[2048] domain; hostile ramps are exercised
//     in NegativeKeybRampResyncsPhaseAndSkipsRamp (and valid ramps up to 4.0
//     from the PAD random preset are covered by KeyboardSyncRampScalesOutput).
//   * Private-state access (phase/currentFreq/ramp flags) via the scoped
//     `#define private public` include pattern from midi_decoder_test.cpp;
//     every header LfoOsc.h reaches (Lfo.h, Osc.h) is pre-included first.
//   * LfoOsc's phase/currentFreq/isNotMidiSynchronized are not written by
//     init(); the firmware relies on the Voice pool's BSS zero-init. The
//     fixture reproduces that with the golden_harness memset+placement-new
//     pattern (zeroed storage, ctor re-establishes the vptr).

// Pre-include the full closure LfoOsc.h reaches (Lfo.h -> SynthStateAware.h/
// SynthState.h + Matrix.h/Common.h; Osc.h -> same + waves globals) so the
// #define below re-parses only LfoOsc.h itself.
#include "Lfo.h"
#include "Osc.h"

#define private public  // NOLINT: scoped to LfoOsc.h only (see header)
#include "LfoOsc.h"
#undef private

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>

// Global 32-entry random table filled by Synth::buildNewSampleBlock (defined
// in Osc.cpp, linked). Read/write of noise[0] only, always set explicitly.
extern float noise[32];

namespace {

// Mirrors PREENFM_FREQUENCY_INVERSED_LFO (Common.h): (1/Fs)*32.
constexpr float kInvLfo = (1.0f / 47916.0f) * 32.0f;
// Waveform tolerance: model and firmware execute identical float ops in the
// same order; the slack only absorbs cross-hoster FPU noise.
constexpr float kTol = 1e-6f;

// Closed-form/table reference for the deterministic shapes, replicating
// LfoOsc::nextValueInMatrix's per-case math on a post-wrap phase.
float ShapeValue(LfoType shape, float phase) {
    switch (shape) {
    case LFO_TRIANGLE:
        return phase < 0.5f ? phase * 4.0f - 1.0f
                            : 1.0f - (phase - 0.5f) * 4.0f;
    case LFO_SAW:
        return -1.0f + phase * 2.0f;
    case LFO_SIN: {
        int i = (int)(phase * waveTables[0].max);
        i &= waveTables[0].max;
        return sinTable[i];
    }
    case LFO_SQUARE:
        return phase < 0.5 ? -1.0f : 1.0f;
    default:
        return 0.0f;
    }
}

class LfoOscTest : public ::testing::Test {
protected:
    MatrixRowParams rows_[MATRIX_SIZE];
    Matrix matrix_;
    LfoParams params_;
    float initPhase_;

    struct LfoOscBacking {
        alignas(LfoOsc) unsigned char bytes[sizeof(LfoOsc)];
    };
    LfoOscBacking backing_;
    LfoOsc* lfo_;

    void SetUp() override {
        ResetLfo();
    }

    // Rebuild the LFO object from zeroed storage (BSS-equivalent) — used by
    // SetUp and by multi-shape tests that must not inherit the previous
    // shape's random-family state (init() does not reset noiseLp/
    // nextRandomValue, which the random shapes carry across shapes).
    void ResetLfo() {
        std::memset(&backing_, 0, sizeof(backing_));
        lfo_ = new (&backing_) LfoOsc();

        std::memset(&params_, 0, sizeof(params_));
        initPhase_ = 0.0f;
        std::memset(rows_, 0, sizeof(rows_));
        for (int i = 0; i < MATRIX_SIZE; i++) rows_[i].source = MATRIX_SOURCE_NONE;
        matrix_.init(rows_);
        matrix_.resetSources();
        matrix_.resetAllDestination();
    }

    // Configure params and (re)run init + the afterNewParamsLoad encoder
    // sweep, exactly as the firmware wires a voice.
    void Configure(LfoType shape, float freq, float bias = 0.0f,
                   float keybRamp = 0.0f) {
        params_.shape = (float)shape;
        params_.freq = freq;
        params_.bias = bias;
        params_.keybRamp = keybRamp;
        lfo_->init(&params_, &initPhase_, &matrix_, MATRIX_SOURCE_LFO1,
                   LFO1_FREQ);
        for (int j = 0; j < NUMBER_OF_ENCODERS_PFM2; j++) lfo_->valueChanged(j);
    }

    float Source() { return matrix_.getSource(MATRIX_SOURCE_LFO1); }
};

// THE waveform lock: triangle/saw/sin/square at a representative free-run
// freq, every sample over 200 calls (8 phase wraps) asserted against the
// reference model. bias is added on top (the += lfo->bias line).
TEST_F(LfoOscTest, WaveformsMatchReferenceModelSampleBySample) {
    const LfoType shapes[] = {LFO_TRIANGLE, LFO_SAW, LFO_SIN, LFO_SQUARE};
    for (LfoType shape : shapes) {
        SCOPED_TRACE(shape);
        Configure(shape, 60.0f, /*bias=*/0.05f);
        ASSERT_TRUE(lfo_->isNotMidiSynchronized);
        lfo_->noteOn();  // phase = initPhase = 0, currentRamp = 0
        float phase = initPhase_;
        for (int i = 0; i < 200; i++) {
            phase += (60.0f + 0.0f) * kInvLfo;  // currentFreq = freq + dest(0)
            if (phase >= 1.0f) phase -= 1.0f;
            lfo_->nextValueInMatrix();
            const float expected = ShapeValue(shape, phase) + 0.05f;
            EXPECT_NEAR(Source(), expected, kTol) << "sample " << i;
        }
    }
}

// freq -> phase increment: the private phase accumulator advances by
// freq * PREENFM_FREQUENCY_INVERSED_LFO per call (mod 1), and a matrix
// destination routed into LFO1_FREQ adds to it.
TEST_F(LfoOscTest, FreqDrivesPhaseIncrementPlusMatrixDestination) {
    Configure(LFO_TRIANGLE, 90.0f);
    lfo_->noteOn();
    float phase = 0.0f;
    for (int i = 0; i < 100; i++) {
        phase += 90.0f * kInvLfo;
        if (phase >= 1.0f) phase -= 1.0f;
        lfo_->nextValueInMatrix();
    }
    EXPECT_NEAR(lfo_->phase, phase, kTol);

    // Modulation: +0.5 into LFO1_FREQ makes the effective freq 90.5.
    rows_[4].source = MATRIX_SOURCE_MODWHEEL;
    rows_[4].mul = 0.25f;
    rows_[4].dest1 = LFO1_FREQ;
    rows_[4].dest2 = (DestinationEnum)0;
    matrix_.setSource(MATRIX_SOURCE_MODWHEEL, 2.0f);  // -> +0.5
    matrix_.computeAllDestinations();
    const float phaseBefore = lfo_->phase;
    float expected = phaseBefore;
    for (int i = 0; i < 50; i++) {
        expected += (90.0f + 0.5f) * kInvLfo;
        if (expected >= 1.0f) expected -= 1.0f;
        matrix_.computeAllDestinations();
        lfo_->nextValueInMatrix();
    }
    EXPECT_NEAR(lfo_->phase, expected, kTol);
}

TEST_F(LfoOscTest, NegativeMatrixModulationPreservesFreeRunPhaseSemantics) {
    Configure(LFO_TRIANGLE, 0.1f);
    ASSERT_TRUE(lfo_->isNotMidiSynchronized);
    lfo_->noteOn();

    rows_[4].source = MATRIX_SOURCE_MODWHEEL;
    rows_[4].mul = -1.0f;
    rows_[4].dest1 = LFO1_FREQ;
    rows_[4].dest2 = (DestinationEnum)0;
    matrix_.setSource(MATRIX_SOURCE_MODWHEEL, 1.0f);
    matrix_.computeAllDestinations();

    lfo_->nextValueInMatrix();
    EXPECT_NEAR(lfo_->phase, (0.1f - 1.0f) * kInvLfo, kTol)
        << "valid negative free-run modulation must not be reset by the "
           "MIDI-synchronized phase guard";
}

// S&H randomness (LFO_RANDOM): the value holds noise[0] sampled at the last
// phase wrap (and at noteOn). Changing noise[0] mid-flight proves the hold:
// the output flips exactly on the wrap call, deterministically.
TEST_F(LfoOscTest, RandomHoldsNoise0SampledOnWrap) {
    Configure(LFO_RANDOM, 99.0f);  // fastest free-run (< midi-clock range)
    noise[0] = 0.37f;
    lfo_->noteOn();  // retrigs: currentRandomValue = noise[0]
    float phase = 0.0f;
    int flipCall = -1;
    for (int i = 0; i < 40; i++) {
        if (i == 25) noise[0] = -0.5f;  // new sample source mid-flight
        phase += 99.0f * kInvLfo;
        const bool wrapped = phase >= 1.0f;
        if (wrapped) phase -= 1.0f;
        if (i >= 25 && wrapped) flipCall = i;  // first wrap at/after the flip
        lfo_->nextValueInMatrix();
        const float want = (flipCall < 0) ? 0.37f : -0.5f;
        EXPECT_NEAR(Source(), want, kTol) << "sample " << i;
    }
    EXPECT_GE(flipCall, 0) << "a wrap after i=25 must have occurred";
}

// The brownian / wandering / flow family: a reference model replicating the
// exact state updates on wrap + noteOn, with noise[0] changed twice, every
// emitted sample asserted.
TEST_F(LfoOscTest, BrownianWanderingFlowMatchReferenceModel) {
    const LfoType shapes[] = {LFO_BROWNIAN, LFO_WANDERING, LFO_FLOW};
    for (LfoType shape : shapes) {
        SCOPED_TRACE(shape);
        ResetLfo();
        Configure(shape, 99.0f);
        noise[0] = 0.8f;

        // Model state (matches the memset-zeroed LfoOsc + noteOn effects).
        float mCur = 0.0f, mNext = 0.0f, mLp = 0.0f;
        // noteOn for shape >= LFO_BROWNIAN: lp filter + cur = lp.
        mLp = noise[0] * 0.4f + mLp * 0.6f;
        mCur = mLp;
        lfo_->noteOn();

        float phase = 0.0f;
        for (int i = 0; i < 80; i++) {
            if (i == 30) noise[0] = -0.6f;
            if (i == 60) noise[0] = 0.25f;
            phase += 99.0f * kInvLfo;
            if (phase >= 1.0f) {
                phase -= 1.0f;
                switch (shape) {
                case LFO_BROWNIAN:
                    mLp = noise[0] * 0.4f + mLp * 0.6f;
                    mCur = mLp;
                    break;
                case LFO_WANDERING:
                    mCur = mNext;
                    mNext = noise[0];
                    break;
                case LFO_FLOW:
                    mLp = noise[0] * 0.4f + mLp * 0.6f;
                    mCur = mNext;
                    mNext = mLp;
                    break;
                default:
                    break;
                }
            }
            lfo_->nextValueInMatrix();
            float expected = mCur;
            if (shape == LFO_WANDERING || shape == LFO_FLOW) {
                expected = phase * (mNext - mCur) + mCur;
            }
            EXPECT_NEAR(Source(), expected, kTol) << "sample " << i;
        }
    }
}

// An out-of-range shape (>= LFO_TYPE_MAX) hits the defensively-initialized
// lfoValue (0) — the UB guard added for corrupted params. Locks that the
// guard ships 0 (+bias) instead of garbage.
TEST_F(LfoOscTest, OutOfRangeShapeEmitsZeroPlusBias) {
    Configure((LfoType)LFO_TYPE_MAX, 60.0f, /*bias=*/0.125f);
    lfo_->noteOn();
    for (int i = 0; i < 10; i++) {
        lfo_->nextValueInMatrix();
        EXPECT_FLOAT_EQ(Source(), 0.125f) << "sample " << i;
    }
}

// Keyboard-sync ramp (keybRamp in (0,1]): noteOn restarts the ramp at 0, so
// the FIRST emitted value is scaled by 0 (== bias exactly) and subsequent
// values ramp up by currentRamp * rampInv where rampInv = 50*invTab[ramp*50]
// and currentRamp advances by PREENFM_FREQUENCY_INVERSED_LFO per call.
TEST_F(LfoOscTest, KeyboardSyncRampScalesOutputFromZeroOnNoteOn) {
    Configure(LFO_TRIANGLE, 60.0f, /*bias=*/0.1f, /*keybRamp=*/0.5f);
    const float rampInv = 50 * (1.0f / 25.0f);  // 50 * invTab[25]
    lfo_->noteOn();  // currentRamp = 0

    float phase = 0.0f;
    float currentRamp = 0.0f;
    for (int i = 0; i < 6; i++) {
        phase += 60.0f * kInvLfo;
        if (phase >= 1.0f) phase -= 1.0f;
        lfo_->nextValueInMatrix();
        const float expected =
            ShapeValue(LFO_TRIANGLE, phase) * currentRamp * rampInv + 0.1f;
        EXPECT_NEAR(Source(), expected, kTol) << "sample " << i;
        currentRamp += kInvLfo;
    }

    // Continue through the threshold: once currentRamp >= keybRamp, the
    // firmware stops applying the ramp multiplier and emits the full waveform
    // (+bias). This catches an off-by-one or a ramp that never terminates.
    bool reachedUnscaled = false;
    for (int i = 6; i < 900; i++) {
        phase += 60.0f * kInvLfo;
        if (phase >= 1.0f) phase -= 1.0f;
        const float rampBefore = currentRamp;
        lfo_->nextValueInMatrix();
        float expected = ShapeValue(LFO_TRIANGLE, phase);
        if (rampBefore < params_.keybRamp) {
            expected *= rampBefore * rampInv;
            currentRamp += kInvLfo;
        } else {
            reachedUnscaled = true;
        }
        expected += 0.1f;
        EXPECT_NEAR(Source(), expected, kTol) << "sample " << i;
    }
    EXPECT_TRUE(reachedUnscaled) << "keyboard-sync ramp never reached full output";
    EXPECT_GE(lfo_->currentRamp, params_.keybRamp);
}

// Negative keybRamp ("KSyn off"): valueChanged(ENCODER_LFO_KSYNC) resyncs
// phase to 0, and noteOn sets currentRamp = 1 so no ramp scaling applies.
// (keybRamp=-0.01 keeps (int)(keybRamp*50) at 0 — the only negative range
// that reads invTab in-bounds; see report.)
TEST_F(LfoOscTest, NegativeKeybRampResyncsPhaseAndSkipsRamp) {
    Configure(LFO_TRIANGLE, 60.0f, /*bias=*/0.0f, /*keybRamp=*/-0.01f);
    lfo_->noteOn();
    for (int i = 0; i < 50; i++) lfo_->nextValueInMatrix();
    ASSERT_GT(lfo_->phase, 0.0f) << "precondition: phase advanced";

    lfo_->valueChanged(3);  // ENCODER_LFO_KSYNC
    EXPECT_FLOAT_EQ(lfo_->phase, 0.0f) << "negative ramp resyncs phase";
    lfo_->noteOn();
    EXPECT_EQ(lfo_->currentRamp, 1.0f) << "KSyn off: no ramp";

    float phase = lfo_->phase;
    for (int i = 0; i < 20; i++) {
        phase += 60.0f * kInvLfo;
        if (phase >= 1.0f) phase -= 1.0f;
        lfo_->nextValueInMatrix();
        EXPECT_NEAR(Source(), ShapeValue(LFO_TRIANGLE, phase), kTol)
            << "unscaled output, sample " << i;
    }
}

TEST_F(LfoOscTest, HostileKeybRampClampsInvTabIndexInBounds) {
    // Regression (6.3): keybRamp <= -0.02 made valueChanged(KSYNC) compute
    // invTab[negative] — an OOB global read (ASAN-clean only by luck of the
    // adjacent global layout); the cast itself was UB for NaN/huge values.
    // The index is now guarded before the cast and clamped to [0, 2047].
    // Every hostile ramp must yield rampInv = 50 * invTab[0] = 50, like the
    // safe -0.01 "off" case. Negative ramps additionally resync the phase;
    // NaN does not (NaN < 0 is false — comparison semantics, not the bug).
    const float hostileRamps[] = {
        -0.02f, -0.5f, -1.0f, -12345.0f,
        -std::numeric_limits<float>::infinity(),
    };
    for (float ramp : hostileRamps) {
        SCOPED_TRACE(::testing::PrintToString(ramp));
        Configure(LFO_TRIANGLE, 60.0f, /*bias=*/0.0f, /*keybRamp=*/ramp);
        lfo_->noteOn();
        for (int i = 0; i < 50; i++) lfo_->nextValueInMatrix();
        ASSERT_GT(lfo_->phase, 0.0f) << "precondition: phase advanced";

        lfo_->valueChanged(3);  // ENCODER_LFO_KSYNC
        EXPECT_FLOAT_EQ(lfo_->rampInv, 50.0f)
            << "hostile ramp must clamp to invTab[0], like the -0.01 off case";
        EXPECT_FLOAT_EQ(lfo_->phase, 0.0f) << "negative ramp resyncs phase";
        EXPECT_LT(lfo_->ramp, 0.0f) << "ramp itself keeps its (negative) value";
    }

    // NaN: same index clamp, but the resync comparison (ramp < 0) is false —
    // the OOB read is fixed without changing comparison semantics.
    Configure(LFO_TRIANGLE, 60.0f, /*bias=*/0.0f,
              /*keybRamp=*/std::numeric_limits<float>::quiet_NaN());
    lfo_->noteOn();
    for (int i = 0; i < 50; i++) lfo_->nextValueInMatrix();
    lfo_->valueChanged(3);
    EXPECT_FLOAT_EQ(lfo_->rampInv, 50.0f)
        << "NaN ramp must clamp to invTab[0]";
    EXPECT_GT(lfo_->phase, 0.0f) << "NaN is not negative: no resync";
}

// noteOn resets the phase to the params' initPhase pointer (the per-voice
// "phase" encoder value, owned by the caller).
TEST_F(LfoOscTest, NoteOnResetsPhaseToInitPhase) {
    initPhase_ = 0.3f;
    Configure(LFO_TRIANGLE, 60.0f);
    lfo_->noteOn();
    ASSERT_FLOAT_EQ(lfo_->phase, 0.3f);
    for (int i = 0; i < 100; i++) lfo_->nextValueInMatrix();
    ASSERT_GT(lfo_->phase, 0.3f);
    lfo_->noteOn();
    EXPECT_FLOAT_EQ(lfo_->phase, 0.3f);
}

// A midi-synced freq (freq*10 >= LFO_MIDICLOCK_MC_DIV_16): the free-run path
// is disabled (isNotMidiSynchronized false — the freq line in
// nextValueInMatrix is skipped) and midiClock owns phase + rate.
TEST_F(LfoOscTest, MidiSyncedLfoUsesClockPhaseAndRate) {
    initPhase_ = 0.3f;
    Configure(LFO_TRIANGLE, 100.0f);  // (int)(100*10+.05) == MC_DIV_16
    ASSERT_FALSE(lfo_->isNotMidiSynchronized);
    // init's own midiClock(0, true) already snapped: phase = 0 + initPhase,
    // rate from the seeded ticks (1536).
    ASSERT_NEAR(lfo_->phase, 0.3f, kTol);
    ASSERT_NEAR(lfo_->currentFreq,
                47916.0f / 32.0f / 32.0f * (1.0f / 1536.0f), kTol);

    // Even position + computeStep: rate recomputed from ticks=0, phase snapped.
    lfo_->midiClock(6, true);
    EXPECT_NEAR(lfo_->phase, (6 & 0x3E) * 0.015625f + 0.3f, kTol);
    EXPECT_NEAR(lfo_->currentFreq, 47916.0f / 32.0f / 32.0f, kTol);

    // Advance: phase moves by currentFreq * invLfo (NOT by lfo->freq).
    float phase = lfo_->phase;
    for (int i = 0; i < 10; i++) {
        phase += lfo_->currentFreq * kInvLfo;
        if (phase >= 1.0f) phase -= 1.0f;
        lfo_->nextValueInMatrix();
        EXPECT_NEAR(Source(), ShapeValue(LFO_TRIANGLE, phase), kTol)
            << "sample " << i;
    }
    // Odd position: ignored entirely.
    const float phaseBefore = lfo_->phase;
    lfo_->midiClock(7, true);
    EXPECT_EQ(lfo_->phase, phaseBefore);

    // Mask-wrap boundary: songPosition 64 (one full DIV_16 cycle of the
    // 0x3E mask) snaps phase back to exactly initPhase — same as position 0.
    lfo_->midiClock(64, true);
    EXPECT_NEAR(lfo_->phase, 0.3f, kTol) << "sp=64 must wrap like sp=0";
    lfo_->midiClock(0, true);
    EXPECT_NEAR(lfo_->phase, 0.3f, kTol) << "sp=0 baseline";
}

// Free-run LFO ((int)(freq*10+.05) matches NO clock-division case): the
// midiClock switch falls through every arm — a silent no-op that leaves
// phase and currentFreq untouched. Guards the clock-vs-free-run isolation
// (a regression that lets clock ticks bleed into a free-run LFO fails here).
TEST_F(LfoOscTest, MidiClockOnFreeRunLfoIsSilentNoOp) {
    initPhase_ = 0.3f;
    Configure(LFO_TRIANGLE, 60.0f);  // (int)(60*10+.05) == 600: no case
    ASSERT_TRUE(lfo_->isNotMidiSynchronized);
    const float phaseBefore = lfo_->phase;
    const float freqBefore = lfo_->currentFreq;
    lfo_->midiClock(8, true);
    lfo_->midiClock(0, false);
    EXPECT_EQ(lfo_->phase, phaseBefore) << "free-run phase must not move";
    EXPECT_EQ(lfo_->currentFreq, freqBefore) << "free-run rate must not move";
}

// All 9 clock-division cases snap phase + rate per their masks/factors
// (realistic even songPosition, computeStep=true, ticks=0 right after init).
TEST_F(LfoOscTest, MidiClockAllDivisionsSnapPhaseAndRate) {
    struct Case {
        float freq;
        float expectedPhase;                        // at songPosition 8
        float expectedFreq;                         // PREENFM_FREQUENCY/BLOCK_SIZE*factor
    };
    const Case cases[] = {
        {100.0f, (8 & 0x3E) * 0.015625f, 47916.0f / 32.0f / 32.0f},  // DIV_16
        {100.1f, (8 & 0x1E) * 0.03125f, 47916.0f / 32.0f / 16.0f},   // DIV_8
        {100.2f, (8 & 0xE) * 0.0625f, 47916.0f / 32.0f / 8.0f},      // DIV_4
        {100.3f, (8 & 0x6) * 0.125f, 47916.0f / 32.0f / 4.0f},       // DIV_2
        {100.4f, (8 & 0x2) * 0.25f, 47916.0f / 32.0f / 2.0f},        // MC
        {100.5f, 0.0f, 47916.0f / 32.0f},                            // TIME_2
        {100.6f, 0.0f, 47916.0f / 32.0f * 3.0f},                     // TIME_3
        {100.7f, 0.0f, 47916.0f / 32.0f * 2.0f},                     // TIME_4
        {100.8f, 0.0f, 47916.0f / 32.0f * 4.0f},                     // TIME_8
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.freq);
        Configure(LFO_TRIANGLE, c.freq);
        lfo_->midiClock(8, true);
        EXPECT_NEAR(lfo_->phase, c.expectedPhase, kTol);
        EXPECT_NEAR(lfo_->currentFreq, c.expectedFreq, kTol);

        // Post-advance wrap: TIME_3/4/8 advance the phase by 3/2/4 per
        // block; after several blocks the phase must stay in [0, 1).
        for (int block = 0; block < 8; block++) {
            lfo_->nextValueInMatrix();
            EXPECT_GE(lfo_->phase, 0.0f) << "phase escaped [0,1) after block " << block;
            EXPECT_LT(lfo_->phase, 1.0f) << "phase escaped [0,1) after block " << block;
        }
    }
}

// Regression for the pre-fix escape: TIME_8 advances the phase by 4 per
// block, so the old per-waveform single `phase -= 1` left the phase in
// [0,4) and the modulation out of range for several consecutive blocks.
TEST_F(LfoOscTest, MidiClockTime8PhaseStaysWrappedOverManyBlocks) {
    Configure(LFO_TRIANGLE, 100.8f);  // (int)(100.8*10+.05) == 1008: TIME_8
    ASSERT_FALSE(lfo_->isNotMidiSynchronized);
    lfo_->midiClock(0, true);
    for (int block = 0; block < 32; block++) {
        lfo_->nextValueInMatrix();
        ASSERT_GE(lfo_->phase, 0.0f) << "phase escaped [0,1) after block " << block;
        ASSERT_LT(lfo_->phase, 1.0f) << "phase escaped [0,1) after block " << block;
        // Triangle value stays a valid waveform sample in [-1, 1].
        EXPECT_LE(Source(), 1.0f);
        EXPECT_GE(Source(), -1.0f);
    }
}

// A hostile rate (NaN / inf / huge float from a corrupt preset) must fail
// safe to phase 0 in the float domain — never reach the undefined (int) cast
// in the per-block wrap (review finding on the 4.4 fix).
TEST_F(LfoOscTest, HostileFreqFailsSafeToPhaseZeroNotUndefinedCast) {
    Configure(LFO_TRIANGLE, 100.8f);
    lfo_->midiClock(0, true);
    const float hostileRates[] = { 1.0e30f, std::numeric_limits<float>::infinity(),
                                   -std::numeric_limits<float>::infinity(),
                                   std::numeric_limits<float>::quiet_NaN() };
    for (float rate : hostileRates) {
        lfo_->currentFreq = rate;
        lfo_->phase = 0.25f;
        for (int block = 0; block < 4; block++) {
            lfo_->nextValueInMatrix();
            EXPECT_TRUE(lfo_->phase >= 0.0f && lfo_->phase < 1.0f)
                << "rate " << rate << " left phase at " << lfo_->phase;
        }
        EXPECT_LE(Source(), 1.0f);
        EXPECT_GE(Source(), -1.0f);
    }
}

// valueChanged(ENCODER_LFO_FREQ) is the free-run/midi-sync switch.
TEST_F(LfoOscTest, ValueChangedFreqTogglesMidiSyncFlag) {
    Configure(LFO_TRIANGLE, 50.0f);
    EXPECT_TRUE(lfo_->isNotMidiSynchronized);
    Configure(LFO_TRIANGLE, 100.4f);
    EXPECT_FALSE(lfo_->isNotMidiSynchronized);
    // Flipping back re-enables.
    params_.freq = 50.0f;
    lfo_->valueChanged(1);  // ENCODER_LFO_FREQ
    EXPECT_TRUE(lfo_->isNotMidiSynchronized);
}

// noteOff is an empty stub — cover + lock as no-op.
TEST_F(LfoOscTest, NoteOffIsNoOp) {
    Configure(LFO_TRIANGLE, 60.0f);
    lfo_->noteOn();
    lfo_->nextValueInMatrix();
    const float v = Source();
    lfo_->noteOff();
    EXPECT_FLOAT_EQ(Source(), v);
}

}  // namespace
