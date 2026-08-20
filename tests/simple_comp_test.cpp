// Direct characterization coverage for the block compressor used by Synth.
// Firmware instances live in BSS, so the fixture begins object lifetime in
// zeroed aligned storage before calling the documented initRuntime().
#include "gtest/gtest.h"

#include "SimpleComp.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>

using chunkware_simple::SimpleComp;

class SimpleCompTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&storage_, 0, sizeof(storage_));
        comp_ = new (&storage_) SimpleComp;
        comp_->initRuntime();
    }
    void TearDown() override { comp_->~SimpleComp(); }

    static void fill(float (&block)[64], float left, float right) {
        for (std::size_t frame = 0; frame < 32; ++frame) {
            const std::size_t sample = frame * 2U;
            block[sample] = left;
            block[sample + 1U] = right;
        }
    }

    static void expectBlockExactlyEqual(const float (&actual)[64],
                                        const float (&expected)[64]) {
        for (std::size_t sample = 0; sample < 64; ++sample) {
            EXPECT_EQ(actual[sample], expected[sample]) << "sample " << sample;
        }
    }

    typename std::aligned_storage<sizeof(SimpleComp), alignof(SimpleComp)>::type storage_;
    SimpleComp* comp_ = nullptr;
};

TEST_F(SimpleCompTest, BypassPreservesDifferentiatedBlockAndReportsUnity) {
    comp_->setThresh(101.0f);
    comp_->setRatio(0.25f);
    float block[64];
    for (int i = 0; i < 64; ++i) block[i] = (i - 31.0f) / 17.0f;
    float original[64];
    std::copy(block, block + 64, original);

    EXPECT_FLOAT_EQ(comp_->processPfm3(block), 1.0f);
    expectBlockExactlyEqual(block, original);
    EXPECT_FLOAT_EQ(comp_->getCurrentGainReduction(), 0.0f);
}

TEST_F(SimpleCompTest, ExactlyAtBypassThresholdStillTakesCompressorPath) {
    // The bypass gate is strict (threshdB_ > 100.0f), so exactly 100.0f is
    // NOT bypass: it takes the compressor path. For sub-threshold input the
    // gain is unity and the block is untouched, but gr_ comes from the
    // envelope/transfer branch (a denormal decay residue, measured
    // -7.5e-26 on the host) rather than the bypass early-out's exact 0.0f.
    // Tolerance keeps this robust on FTZ targets where the residue flushes.
    comp_->setThresh(100.0f);
    comp_->setRatio(0.25f);
    float block[64];
    for (int i = 0; i < 64; ++i) block[i] = (i - 31.0f) / 17.0f;
    float original[64];
    std::copy(block, block + 64, original);

    EXPECT_FLOAT_EQ(comp_->processPfm3(block), 1.0f);
    expectBlockExactlyEqual(block, original);
    EXPECT_NEAR(comp_->getCurrentGainReduction(), 0.0f, 1e-20f);
}

TEST_F(SimpleCompTest, FastCompressionHasDeterministicSmoothedGoldenBlock) {
    comp_->setSampleRate(1000.0f);
    comp_->setAttack(1.0f);
    comp_->setRelease(20.0f);
    comp_->setThresh(-12.0f);
    comp_->setRatio(0.25f);
    float block[64];
    fill(block, 1.0f, 0.5f);

    const float gain = comp_->processPfm3(block);
    ASSERT_TRUE(std::isfinite(gain));
    EXPECT_NEAR(gain, 0.5194524f, 1e-5f);
    EXPECT_NEAR(comp_->getCurrentGainReduction(), -5.689085f, 1e-4f);
    // Shared stereo gain ramps linearly from the BSS-zero previousGain_.
    for (std::size_t frame = 0; frame < 32; ++frame) {
        const std::size_t sample = frame * 2U;
        const float frameGain = gain * static_cast<float>(frame + 1U) / 32.0f;
        ASSERT_TRUE(std::isfinite(block[sample]));
        ASSERT_TRUE(std::isfinite(block[sample + 1U]));
        EXPECT_NEAR(block[sample], frameGain, 1e-5f) << "frame " << frame;
        EXPECT_NEAR(block[sample + 1U], frameGain * 0.5f, 1e-5f)
            << "frame " << frame;
    }
}


TEST_F(SimpleCompTest, FreshBelowThresholdNonzeroBlockIsUnityAndUnchanged) {
    comp_->setThresh(-6.0f);
    comp_->setRatio(0.25f);
    float block[64];
    for (std::size_t sample = 0; sample < 64; ++sample) {
        block[sample] = (sample & 1U) ? -0.125f : 0.25f;
    }
    float original[64];
    std::copy(block, block + 64, original);

    EXPECT_FLOAT_EQ(comp_->processPfm3(block), 1.0f);
    expectBlockExactlyEqual(block, original);
}

TEST_F(SimpleCompTest, LoudThenQuietUsesReleaseAndMovesGainTowardUnity) {
    comp_->setSampleRate(1000.0f);
    comp_->setAttack(0.1f);
    comp_->setRelease(100.0f);
    comp_->setThresh(-20.0f);
    comp_->setRatio(0.5f);
    float loud[64];
    fill(loud, 1.0f, 1.0f);
    const float compressed = comp_->processPfm3(loud);
    ASSERT_LT(compressed, 1.0f);

    float quiet[64];
    fill(quiet, 0.001f, 0.001f);
    const float released = comp_->processPfm3(quiet);
    EXPECT_GT(released, compressed);
    EXPECT_LE(released, 1.0f);
    EXPECT_GT(quiet[62], quiet[0]);
}

TEST_F(SimpleCompTest, AbsolutePeakAndSharedStereoGainIgnorePolarity) {
    comp_->setSampleRate(1000.0f);
    comp_->setAttack(0.1f);
    comp_->setThresh(-6.0f);
    comp_->setRatio(0.5f);
    float block[64] = {};
    block[20] = -1.0f;
    block[21] = 0.25f;
    const float gain = comp_->processPfm3(block);
    EXPECT_LT(gain, 1.0f);
    EXPECT_LT(block[20], 0.0f);
    EXPECT_NEAR(block[20], -block[21] * 4.0f, 1e-6f);
    EXPECT_NEAR(comp_->getCurrentVolume(), 0.0f, 1e-5f);
}

TEST_F(SimpleCompTest, SilenceIsUnityAndMeterRollsOverOnUint8Counter) {
    comp_->setThresh(-20.0f);
    comp_->setRatio(0.5f);
    float block[64] = {};
    EXPECT_FLOAT_EQ(comp_->processPfm3(block), 1.0f);

    fill(block, 1.0f, 1.0f);
    comp_->processPfm3(block);
    EXPECT_NEAR(comp_->getCurrentVolume(), 0.0f, 1e-5f);
    for (int call = 0; call < 510; ++call) {
        fill(block, 0.1f, 0.1f);
        comp_->processPfm3(block);
    }
    EXPECT_NEAR(comp_->getCurrentVolume(), -20.0f, 1e-3f);
}

TEST_F(SimpleCompTest, ExpansionReturnsBoostButLeavesSamplesUnchanged) {
    comp_->setSampleRate(1000.0f);
    comp_->setAttack(0.1f);
    comp_->setThresh(-20.0f);
    comp_->setRatio(2.0f);
    float block[64];
    fill(block, 1.0f, -0.5f);
    float original[64];
    std::copy(block, block + 64, original);
    const float gain = comp_->processPfm3(block);
    EXPECT_GT(gain, 1.0f);
    expectBlockExactlyEqual(block, original);
}

TEST(SimpleCompStackInstanceTest, FreshStackInstanceMatchesBssInitializedGolden) {
    // The constructor now explicitly zeroes previousGain_/keydBMax_/gr_/
    // keydBMaxCpt_, so a plain stack instance (uninitialized storage) must
    // behave identically to the BSS-backed fixture instance.
    SimpleComp stackComp;
    stackComp.initRuntime();
    stackComp.setThresh(-6.0f);
    stackComp.setRatio(0.25f);
    float block[64];
    for (std::size_t sample = 0; sample < 64; ++sample) {
        block[sample] = (sample & 1U) ? -0.125f : 0.25f;
    }
    float original[64];
    std::copy(block, block + 64, original);

    const float gain = stackComp.processPfm3(block);
    EXPECT_FLOAT_EQ(gain, 1.0f);
    ASSERT_TRUE(std::isfinite(gain));
    for (std::size_t sample = 0; sample < 64; ++sample) {
        ASSERT_TRUE(std::isfinite(block[sample]));
        EXPECT_EQ(block[sample], original[sample]) << "sample " << sample;
    }

    // Two fresh stack instances must produce identical output blocks.
    float a[64], b[64];
    for (std::size_t sample = 0; sample < 64; ++sample) {
        a[sample] = b[sample] = (sample & 1U) ? -0.5f : 0.75f;
    }
    SimpleComp compA, compB;
    compA.initRuntime(); compB.initRuntime();
    for (SimpleComp* c : {&compA, &compB}) {
        c->setSampleRate(1000.0f);
        c->setAttack(1.0f);
        c->setRelease(20.0f);
        c->setThresh(-12.0f);
        c->setRatio(0.25f);
    }
    const float gainA = compA.processPfm3(a);
    const float gainB = compB.processPfm3(b);
    EXPECT_EQ(gainA, gainB);
    for (std::size_t sample = 0; sample < 64; ++sample) {
        EXPECT_EQ(a[sample], b[sample]) << "sample " << sample;
    }
}

TEST(SimpleCompRmsTest, RuntimeConfigurationDelegatesToEnvelopeDetectors) {
    using SimpleCompRms = chunkware_simple::SimpleCompRms;
    typename std::aligned_storage<sizeof(SimpleCompRms), alignof(SimpleCompRms)>::type storage;
    std::memset(&storage, 0, sizeof(storage));
    SimpleCompRms* comp = new (&storage) SimpleCompRms;
    comp->setSampleRate(48000.0f);
    comp->setWindow(7.5f);
    comp->setAttack(2.0f);
    comp->setRelease(80.0f);
    comp->initRuntime();
    EXPECT_FLOAT_EQ(comp->getSampleRate(), 48000.0f);
    EXPECT_FLOAT_EQ(comp->getWindow(), 7.5f);
    EXPECT_FLOAT_EQ(comp->getAttack(), 2.0f);
    EXPECT_FLOAT_EQ(comp->getRelease(), 80.0f);
    comp->~SimpleCompRms();
}
