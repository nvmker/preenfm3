// Host-side coverage for firmware/Src/synth/FxBus.cpp — the global send-FX
// bus (Dattorro-style plate reverb) fed by every timbre's post-mix output.
//
// Regression target (per test-coverage-plan.md Phase 1, row 5):
//   Effect mis-routing. FxBus::mixAdd accumulates each timbre's stereo block
//   into sampleBlock_ scaled by a panTable level derived from the send amount;
//   processBlock runs the full reverb tank (input notch/hp/lp filters, pre-
//   delay, 4 input diffusers, 4 allpass/delay tank branches with damping,
//   modulated by 3 LFOs) only when totalSent != 0; the 19 reverb presets +
//   slow param changes flow through a DEFERRED-change machine (slowParamChange
//   -> 100-mixSumInit wait -> presetChanged/paramChanged) so the audio thread
//   never recomputes coefficients mid-block. The pure delay-interpolation and
//   LFO helpers are asserted against hand-computed values.
//
// Fixture notes (see tests/SEAM.md):
//   * The 14 delay/diffuser/input buffers are STATIC class members shared by
//     every FxBus instance in the process (incl. Synth's own): init() runs in
//     EVERY SetUp per the shared-state hygiene rule. Tests never assume
//     leftover buffer contents.
//   * init() does NOT compute the derived coefficients — the firmware always
//     follows init with presetChanged()/paramChanged() (e.g. via
//     MixerState::restoreFullStateVersion6). SetUp calls paramChanged() so
//     lfoSpeed/sampleMultipler/etc. are deterministic.
//   * panTable (Timbre.cpp) is the REAL firmware table, extern-declared —
//     mixAdd level goldens are asserted against it, not re-derived.
//   * LATENT FIRMWARE SMELL (flagged, not fixed): delay2ReadPos/delay4ReadPos
//     (FxBus.h) have NO initializers and are READ by the first processBlock
//     before the index-increment block writes them — safe on firmware only
//     because every FxBus instance lives in BSS (zero-init). ASAN proved it:
//     a stack-constructed FxBus reads garbage (0xBEBEBEBE) as the delay index
//     and faults. The fixture reproduces the firmware's BSS state with the
//     golden_harness memset+placement-new pattern (zeroed storage, ctor
//     re-establishes the vptr), which is also the honest characterization
//     stance: the firmware behavior under test is the zero-init one.
//   * somethingChanged / waitCountBeforeChange / nextPresetNum are PUBLIC by
//     design (the deferred-change machine's handshake) — no private access
//     needed anywhere in this suite.

#include <gtest/gtest.h>

#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>

#include "FxBus.h"  // firmware-under-test (host-compilable, no seam needed)

// Real firmware pan leveling table (Timbre.cpp, linked).
extern float panTable[];

namespace {

constexpr int kStereo = BLOCK_SIZE * 2;  // 64 floats per stereo block

class FxBusTest : public ::testing::Test {
protected:
    // BSS-equivalent backing for the FxBus under test (see header note):
    // zeroed storage + placement-constructed -> all data members start at 0,
    // exactly like the firmware's globally-allocated instances.
    struct FxBusBacking {
        alignas(FxBus) unsigned char bytes[sizeof(FxBus)];
    };
    FxBusBacking backing_;
    FxBus* bus_;
    float in_[kStereo];

    void SetUp() override {
        std::memset(&backing_, 0, sizeof(backing_));
        bus_ = new (&backing_) FxBus();
        bus_->init();         // zero the shared static buffers + defaults
        bus_->paramChanged(); // compute derived coefficients (firmware order)
        for (int i = 0; i < kStereo; i++) in_[i] = 1.0f;
    }

    // One deferred-change machine cycle: a timbre sends into the bus, then
    // the block is summed (mixSumInit zeroes sampleBlock_, resets totalSent
    // and steps the deferred-change wait counter).
    void Cycle() {
        bus_->mixAdd(in_, 1.0f, 1.0f);
        bus_->mixSumInit();
    }
};

// init() -> setDefaultValue(): the 13 master-FX params land on their
// documented defaults.
TEST_F(FxBusTest, InitSetsMasterFxDefaults) {
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_PREDELAYTIME], 0.54f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_PREDELAYMIX], 0.35f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_SIZE], 0.41f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_DIFFUSION], 0.84f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_DAMPING], 0.63f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_DECAY], 0.74f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_LFODEPTH], 0.28f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_LFOSPEED], 0.69f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_INPUTBASE], 0.36f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_INPUTWIDTH], 0.46f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_NOTCHBASE], 0.5f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_NOTCHSPREAD], 0.69f);
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_LOOPHP], 0.34f);
}

// mixAdd accumulates inStereo * level where level = -panTable[(int)(send *
// 255)] * 0.0625 * reverbLevel. With a zeroed sampleBlock (via mixSumInit
// after a first priming mixAdd), the accumulated values are exact.
TEST_F(FxBusTest, MixAddAccumulatesPannedLevelIntoBlock) {
    // Prime totalSent, then zero the block through the legit firmware path.
    bus_->mixAdd(in_, 1.0f, 1.0f);
    bus_->mixSumInit();

    bus_->mixAdd(in_, /*send=*/1.0f, /*reverbLevel=*/1.0f);
    const float* blk = bus_->getSampleBlock();
    const float level1 = -panTable[(int)(1.0f * 255)] * 0.0625f * 1.0f;
    for (int i = 0; i < kStereo; i++) {
        SCOPED_TRACE(i);
        EXPECT_FLOAT_EQ(blk[i], in_[i] * level1);
    }

    // Re-zero, then a mid send (0.5 -> panTable[127]) at half reverb level.
    bus_->mixSumInit();
    bus_->mixAdd(in_, /*send=*/0.5f, /*reverbLevel=*/0.5f);
    const float level2 = -panTable[(int)(0.5f * 255)] * 0.0625f * 0.5f;
    EXPECT_FLOAT_EQ(bus_->getSampleBlock()[0], in_[0] * level2);
    EXPECT_FLOAT_EQ(bus_->getSampleBlock()[63], in_[63] * level2);
    // And the pan table really levels: mid send is quieter than full send.
    EXPECT_GT(level1 * level1, 0.0f);
    EXPECT_LT(std::fabs(level2), std::fabs(level1));

    // FIXED (spec 2.2): send > 1 (corrupt bank / external MIDI) clamps the
    // derived panTable index to 255 — no out-of-bounds table read.
    bus_->mixSumInit();
    bus_->mixAdd(in_, /*send=*/1.5f, /*reverbLevel=*/1.0f);
    const float levelClamped = -panTable[255] * 0.0625f * 1.0f;
    EXPECT_FLOAT_EQ(bus_->getSampleBlock()[0], in_[0] * levelClamped);

    bus_->mixSumInit();
    bus_->mixAdd(in_, /*send=*/1000.0f, /*reverbLevel=*/1.0f);
    EXPECT_FLOAT_EQ(bus_->getSampleBlock()[0], in_[0] * levelClamped);

    bus_->mixSumInit();
    bus_->mixAdd(in_, std::numeric_limits<float>::infinity(), 1.0f);
    EXPECT_FLOAT_EQ(bus_->getSampleBlock()[0], in_[0] * levelClamped);

    bus_->mixSumInit();
    bus_->mixAdd(in_, std::numeric_limits<float>::max(), 1.0f);
    EXPECT_FLOAT_EQ(bus_->getSampleBlock()[0], in_[0] * levelClamped);
}

// send <= 0 is fully skipped: totalSent stays 0, so BOTH mixSumInit and
// processBlock take their early-return gates (sampleBlock_ is not even
// zeroed, and processBlock leaves outBuff untouched).
TEST_F(FxBusTest, NonPositiveSendSkipsBothEarlyReturnGates) {
    bus_->mixAdd(in_, 1.0f, 1.0f);  // prime
    bus_->mixSumInit();             // zero block, totalSent == 0

    bus_->mixAdd(in_, 0.0f, 1.0f);
    bus_->mixAdd(in_, -0.3f, 1.0f);
    for (int i = 0; i < kStereo; i++) {
        EXPECT_FLOAT_EQ(bus_->getSampleBlock()[i], 0.0f) << "block dirtied";
    }
    // mixSumInit's early return: a dirty marker in the block SURVIVES a
    // mixSumInit while totalSent == 0.
    bus_->getSampleBlock()[0] = 5.0f;  // public accessor returns non-const ptr
    bus_->mixSumInit();
    EXPECT_FLOAT_EQ(bus_->getSampleBlock()[0], 5.0f)
        << "totalSent==0: mixSumInit must early-return before zeroing";
    // processBlock's early return: outBuff untouched. Distinct nonzero
    // sentinels catch both zero-fill and accidental partial writes.
    int32_t out[kStereo];
    int32_t expected[kStereo];
    for (int i = 0; i < kStereo; i++) {
        out[i] = 0x12340000 + i;
        expected[i] = out[i];
    }
    bus_->processBlock(out);
    for (int i = 0; i < kStereo; i++) EXPECT_EQ(out[i], expected[i]);
}

// Pure helper: linear interpolation with wraparound at index 0 (y0 comes
// from the buffer END via bufferLenM1). Hand-computed.
TEST_F(FxBusTest, DelayInterpolationHandComputed) {
    float buf[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    // readPos 3.25: y1=buf[3]=40, y0=buf[2]=30, x=0.75 -> 30+0.75*10=37.5
    EXPECT_FLOAT_EQ(bus_->delayInterpolation(3.25f, buf, 7), 37.5f);
    // Integer readPos: x=1 -> exactly y1.
    EXPECT_FLOAT_EQ(bus_->delayInterpolation(3.0f, buf, 7), 40.0f);
    // readPos 0.5: y1=buf[0]=10, y0=buf[0+7]=buf[7]=80, x=0.5 -> 80+0.5*(10-80)=45
    EXPECT_FLOAT_EQ(bus_->delayInterpolation(0.5f, buf, 7), 45.0f);
}

// Pure helper: allpass-style interpolation y1 + x*(y0 - prevVal), with the
// same index-0 wraparound. Hand-computed.
TEST_F(FxBusTest, DelayAllpassInterpolationHandComputed) {
    float buf[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    // readPos 2.5, prev 7: 30 + 0.5*(20-7) = 36.5
    EXPECT_FLOAT_EQ(bus_->delayAllpassInterpolation(2.5f, buf, 7, 7.0f), 36.5f);
    // Wrap: readPos 0.25, prev 0: y1=10, y0=buf[7]=80, x=0.75 -> 10+60=70
    EXPECT_FLOAT_EQ(bus_->delayAllpassInterpolation(0.25f, buf, 7, 0.0f), 70.0f);
}

// Pure helper: lfoProcess advances the triangle by inc*lfoSpeed (lfoSpeed =
// LFOSPEED^3 after paramChanged with the 0.69 default), clamps at [0,1] with
// direction flip, and low-passes into lfo via (lfo*3999 + tri)*0.00025.
TEST_F(FxBusTest, LfoProcessExactAndClamped) {
    float ls = 0.69f;
    ls *= ls * ls;  // paramChanged's temp = lfoSpeedLinear^3

    // Free step, exact ops replicated.
    float lfo = 0.0f, tri = 0.5f, inc = 0.00037f;
    bus_->lfoProcess(&lfo, &tri, &inc);
    EXPECT_FLOAT_EQ(tri, 0.5f + 0.00037f * ls);
    EXPECT_FLOAT_EQ(lfo, (0.0f * 3999.0f + (0.5f + 0.00037f * ls)) * 0.00025f);
    EXPECT_FLOAT_EQ(inc, 0.00037f);

    // Upper clamp: big positive inc -> tri pinned to 1, inc flips.
    lfo = 0.3f; tri = 0.9999f; inc = 5.0f;
    bus_->lfoProcess(&lfo, &tri, &inc);
    EXPECT_FLOAT_EQ(tri, 1.0f);
    EXPECT_FLOAT_EQ(inc, -5.0f);
    EXPECT_FLOAT_EQ(lfo, (0.3f * 3999.0f + 1.0f) * 0.00025f);

    // Lower clamp: big negative inc -> tri pinned to 0, inc flips back.
    lfo = 0.3f; tri = 0.00001f; inc = -5.0f;
    bus_->lfoProcess(&lfo, &tri, &inc);
    EXPECT_FLOAT_EQ(tri, 0.0f);
    EXPECT_FLOAT_EQ(inc, 5.0f);
    EXPECT_FLOAT_EQ(lfo, (0.3f * 3999.0f + 0.0f) * 0.00025f);
}

// processBlock: an impulse sent into the bus produces a NONZERO wet output on
// the very first block (the input allpass chain responds immediately), stays
// finite across 8 blocks, and accumulates into outBuff (+=, not =).
TEST_F(FxBusTest, ProcessBlockImpulseProducesNonZeroWetOutput) {
    float impulse[kStereo];
    std::memset(impulse, 0, sizeof(impulse));
    impulse[0] = 1.0f;
    impulse[1] = 1.0f;

    bus_->mixAdd(impulse, 1.0f, 1.0f);
    int32_t out[kStereo];
    std::memset(out, 0, sizeof(out));
    bus_->processBlock(out);
    int nonzero = 0;
    for (int i = 0; i < kStereo; i++) {
        EXPECT_LT(std::abs((long long)out[i]), 1LL << 30) << "insane sample";
        if (out[i] != 0) nonzero++;
    }
    EXPECT_GT(nonzero, 0) << "wet path must respond to the first impulse";

    // More blocks with SILENT input: the tank must decay on its own rather
    // than being re-excited by another impulse. mixAdd(silence, send>0) keeps
    // processBlock active while contributing zero new samples.
    float silence[kStereo] = {};
    int32_t out2[kStereo];
    std::memset(out2, 0, sizeof(out2));
    for (int b = 0; b < 8; b++) {
        bus_->mixSumInit();
        bus_->mixAdd(silence, 1.0f, 1.0f);
        bus_->processBlock(out2);
        for (int i = 0; i < kStereo; i++) {
            EXPECT_LT(std::abs((long long)out2[i]), 1LL << 30);
        }
    }
    int tailNonzero = 0;
    for (int i = 0; i < kStereo; i++) {
        if (out2[i] != 0) tailNonzero++;
    }
    EXPECT_GT(tailNonzero, 0) << "reverb tail must persist after the impulse";

    // Dry-signal preservation (spec I/O: "dry passes scaled"): processBlock's
    // output stage is ADDITIVE — outBuff[i] += (int32_t)(wet*sampleMultipler)
    // (FxBus.cpp output loop) — so dry content the caller already placed in
    // outBuff passes through plus the wet contribution. Linearity proof: two
    // PRISTINE buses (BSS-equivalent backing + init(), which re-zeroes the
    // shared STATIC delay lines, + paramChanged — the SetUp recipe) run the
    // identical wet input, differing ONLY in the dry preload: every sample
    // must differ by exactly the preload. (Re-initing the SAME bus is not
    // enough: init() does not reset per-instance LFO phase, so a bus with
    // processing history renders a slightly different wet — itself a
    // characterization of how much instance state survives init().)
    const int32_t kDry = 1234567;
    FxBusBacking b1, b2;
    std::memset(&b1, 0, sizeof(b1));
    std::memset(&b2, 0, sizeof(b2));
    FxBus* busA = new (&b1) FxBus();
    busA->init();
    busA->paramChanged();
    busA->mixAdd(impulse, 1.0f, 1.0f);
    std::memset(out, 0, sizeof(out));
    busA->processBlock(out);  // pass 1: dry == 0
    FxBus* busB = new (&b2) FxBus();
    busB->init();
    busB->paramChanged();
    busB->mixAdd(impulse, 1.0f, 1.0f);
    int32_t outDry[kStereo];
    for (int i = 0; i < kStereo; i++) outDry[i] = kDry;
    busB->processBlock(outDry);  // pass 2: identical wet, dry == kDry
    for (int i = 0; i < kStereo; i++) {
        EXPECT_EQ(outDry[i] - out[i], kDry)
            << "dry must pass through additively; sample " << i;
    }
}

// THE deferred-preset machine: slowParamChange arms it (somethingChanged,
// waitCount 100); it does NOT fire for 100 mixSumInit cycles (each cycle
// needs a mixAdd first — totalSent == 0 skips the machine entirely), fires on
// the 101st, and applies presetChanged(nextPresetNum) for every preset value:
// sizes 0-4 (presets 0..14), specials 15..18, and out-of-range defaults.
TEST_F(FxBusTest, DeferredPresetChangeFiresAfterWaitCountForAllPresets) {
    struct P { int num; float size; };
    // Preset 0 is EXCLUDED here: currentPresetNum starts at 0, so
    // nextPresetNum==0 takes the paramChanged arm (see the next test, which
    // drives preset 0 after a real preset has changed currentPresetNum).
    const P presets[] = {
        {1, 0.13f},  {2, 0.23f},  {3, 0.26f},  {4, 0.26f},
        {5, 0.26f},  {6, 0.465f}, {7, 0.465f}, {8, 0.465f},
        {9, 0.775f}, {10, 0.775f}, {11, 0.775f},
        {12, 0.87f}, {13, 0.87f}, {14, 0.87f}, {15, 1.0f},
        {16, 0.72f}, {17, 0.94f}, {18, 0.6f},
        {19, 0.41f /*default*/}, {20, 0.41f /*default*/},
    };
    for (const P& p : presets) {
        SCOPED_TRACE(p.num);
        SetUp();  // fresh bus per preset
        // Sentinel: an SIZE value distinct from every preset target.
        bus_->masterfxConfig[GLOBALFX_SIZE] = 0.5f;
        bus_->paramChanged();
        ASSERT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_SIZE], 0.5f);

        bus_->nextPresetNum = p.num;
        bus_->slowParamChange();
        ASSERT_TRUE(bus_->somethingChanged);
        ASSERT_EQ(bus_->waitCountBeforeChange, 100);

        for (int i = 0; i < 100; i++) Cycle();
        EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_SIZE], 0.5f)
            << "fired early: the wait counter was not honored";
        EXPECT_EQ(bus_->waitCountBeforeChange, 0);

        Cycle();  // 101st: fires
        EXPECT_FALSE(bus_->somethingChanged) << "machine must disarm";
        EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_SIZE], p.size);
    }
}

// The deferred machine's OTHER arm: when nextPresetNum == currentPresetNum,
// the fire applies paramChanged() (recompute coefficients from the current
// masterfxConfig) instead of presetChanged(). Proven via lfoProcess: a fresh
// LFOSPEED of 0.2 only takes effect in lfoProcess once paramChanged ran.
// Afterwards, preset 0 is driven through the presetChanged arm (only
// reachable once currentPresetNum != 0).
TEST_F(FxBusTest, DeferredParamChangeBranchRecomputesCoefficients) {
    bus_->nextPresetNum = 3;                 // != currentPresetNum (0)
    bus_->slowParamChange();
    for (int i = 0; i < 101; i++) Cycle();  // fire presetChanged(3)
    ASSERT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_SIZE], 0.26f);
    ASSERT_FALSE(bus_->somethingChanged);
    // presetChanged(3) synced the public handshake fields: next == 3.
    ASSERT_EQ(bus_->nextPresetNum, 3);

    bus_->masterfxConfig[GLOBALFX_LFOSPEED] = 0.2f;
    bus_->slowParamChange();
    for (int i = 0; i < 100; i++) Cycle();
    // Not fired yet: lfoSpeed still preset 3's 0.55^3 (presetChanged's own
    // trailing paramChanged applied it).
    {
        float lfo = 0.0f, tri = 0.0f, inc = 1.0f;
        bus_->lfoProcess(&lfo, &tri, &inc);
        float ls = 0.55f; ls *= ls * ls;
        EXPECT_FLOAT_EQ(tri, 1.0f * ls) << "paramChanged fired early";
    }
    Cycle();  // 101st: currentPresetNum == nextPresetNum -> paramChanged()
    {
        float lfo = 0.0f, tri = 0.0f, inc = 1.0f;
        bus_->lfoProcess(&lfo, &tri, &inc);
        float ls = 0.2f; ls *= ls * ls;  // 0.008
        EXPECT_FLOAT_EQ(tri, 1.0f * ls)
            << "paramChanged must recompute lfoSpeed from masterfxConfig";
    }
    EXPECT_FALSE(bus_->somethingChanged);
    EXPECT_EQ(bus_->waitCountBeforeChange, -1);
    // The public preset slot was not disturbed by the param-only fire.
    EXPECT_EQ(bus_->nextPresetNum, 3);

    // Preset 0 through the presetChanged arm (currentPresetNum is now 3):
    bus_->nextPresetNum = 0;
    bus_->slowParamChange();
    for (int i = 0; i < 101; i++) Cycle();
    EXPECT_FLOAT_EQ(bus_->masterfxConfig[GLOBALFX_SIZE], 0.1f);
    EXPECT_EQ(bus_->nextPresetNum, 0);
}

}  // namespace
