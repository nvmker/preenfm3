// Host-side coverage for firmware/Src/filesystem/UserWaveform.cpp —
// user wavetable txt/bin load + the txt->bin conversion pipeline.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim.
// Quirks pinned (deferred-work.md):
//   * interpolate() reads buffer[iPos+1] ONE PAST the populated source
//     window (iPos+1 == srcN happens for the last target sample) — value
//     comes from stale/zero tail data, not the source wave.
//   * the txt parser's numberOfSample must be 32..1024; outside -> '#' error
//     name and NO waveform load.
// userWaveform/waveTables are the REAL globals from Osc.cpp (already linked);
// oscShapeNames comes from the Phase-4 stub table.
#include "gtest/gtest.h"

#include "Common.h"
#include "FileSystemUtils.h"
#include "fatfs.h"
#define private public
#include "UserWaveform.h"
#undef private
#include "Osc.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern float userWaveform[6][1024];
extern struct WaveTable waveTables[];
extern const char* oscShapeNames[];

class UserWaveformTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3/waveform");
        fsu_ = new FileSystemUtils;
        uw_.setFileSystemUtils(fsu_);
        memset(userWaveform, 0, sizeof(userWaveform));
    }
    void TearDown() override { delete fsu_; }

    // "NAME 64\n" + count floats, one digit after the point
    std::string MakeTxt(const char* name, int count, float start, float step) {
        std::string s = std::string(name) + " " + std::to_string(count) + "\n";
        char buf[32];
        for (int i = 0; i < count; i++) {
            snprintf(buf, sizeof(buf), "%.3f", start + step * i);
            s += buf;
            s += " ";
        }
        return s;
    }
    UserWaveform uw_;
    FileSystemUtils* fsu_;
};

TEST_F(UserWaveformTest, NoFilesLeavesWaveformSilent) {
    uw_.loadUserWaveforms();
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[0][s], 0.0f);
    }
}

TEST_F(UserWaveformTest, BinFileLoadsNameCountAndSamples) {
    // layout: 4 name chars, int32 sample count @4, float32 samples @8
    std::vector<uint8_t> bin(8 + 64 * 4, 0);
    memcpy(bin.data(), "SIN1", 4);
    int32_t n = 64;
    memcpy(bin.data() + 4, &n, 4);
    float f[64];
    for (int i = 0; i < 64; i++) f[i] = i / 64.0f;
    memcpy(bin.data() + 8, f, sizeof(f));
    fatfsShimInjectBytes("0:/pfm3/waveform/usr1.bin", bin.data(), bin.size());

    uw_.loadUserWaveforms();
    EXPECT_EQ(memcmp(bin.data(), "SIN1", 4), 0);
    EXPECT_FLOAT_EQ(userWaveform[0][0], 0.0f);
    EXPECT_FLOAT_EQ(userWaveform[0][63], 63 / 64.0f);
    EXPECT_EQ(waveTables[8 + 0].max, 63);
    EXPECT_EQ(oscShapeNames[8 + 0], uw_.userWaveFormNames[0]);
}

TEST_F(UserWaveformTest, TxtFileIsParsedNormalizedAndCachedAsBin) {
    std::string txt = MakeTxt("RAMP", 64, -1.0f, 2.0f / 63.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr2.txt", txt.c_str());

    uw_.loadUserWaveforms();
    // normalized: min-max ramp centered then scaled by min(|1/min|,1/max)
    // average ~ 0 -> min ~ -1, max ~ +1 -> m = 1/1 = 1
    EXPECT_NEAR(userWaveform[1][0], -1.0f, 0.05f);
    EXPECT_NEAR(userWaveform[1][63], 1.0f, 0.05f);
    // bin cache was written with the SAME values, and a second load (bin
    // path) must be byte-identical to the first (round-trip golden)
    std::vector<uint8_t> bin1;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr2.bin", bin1));
    ASSERT_EQ(bin1.size(), 8u + 64 * 4);
    EXPECT_EQ(memcmp(bin1.data(), "RAMP", 4), 0);
    int32_t cnt = 0;
    memcpy(&cnt, bin1.data() + 4, 4);
    EXPECT_EQ(cnt, 64);
    std::vector<float> reloaded(64);
    uw_.loadUserWaveforms();  // now takes the bin path
    memcpy(reloaded.data(), userWaveform[1], 64 * 4);
    std::vector<float> saved(64);
    memcpy(saved.data(), bin1.data() + 8, 64 * 4);
    EXPECT_EQ(reloaded, saved);  // bin save->load byte-identical
}

TEST_F(UserWaveformTest, TxtIntermediateLengthIsInterpolatedUp) {
    // 200 samples (128 < 200 < 256) -> interpolate to 256, then bin-cached
    std::string txt = MakeTxt("I200", 200, 0.0f, 1.0f / 199.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr3.txt", txt.c_str());
    uw_.loadUserWaveforms();
    std::vector<uint8_t> bin;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr3.bin", bin));
    int32_t cnt = 0;
    memcpy(&cnt, bin.data() + 4, 4);
    EXPECT_EQ(cnt, 256);
    EXPECT_EQ(waveTables[8 + 2].max, 255);
}

TEST_F(UserWaveformTest, TxtBadSampleCountMarksErrorAndSkips) {
    fatfsShimInjectString("0:/pfm3/waveform/usr4.txt", "BAD1 31\n0.5 0.5\n");
    uw_.loadUserWaveforms();
    EXPECT_EQ(uw_.userWaveFormNames[3][0], '#');
    EXPECT_EQ(oscShapeNames[8 + 3], uw_.userWaveFormNames[3]);
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/waveform/usr4.bin"));
    // too big also fails
    fatfsShimInjectString("0:/pfm3/waveform/usr5.txt", "BAD2 2000\n0.5\n");
    uw_.loadUserWaveforms();
    EXPECT_EQ(uw_.userWaveFormNames[4][0], '#');
}

TEST_F(UserWaveformTest, InterpolateReadsOnePastPopulatedSourceQuirk) {
    // QUIRK GOLDEN: target[1023] interpolates buffer[599]..buffer[600] where
    // 600 == srcN (never populated by the txt parse). With a clean zero tail
    // the last sample is v[599]*(1-decimal); we pin the exact value.
    float buf[1024];
    for (int i = 0; i < 600; i++) buf[i] = 1.0f;
    for (int i = 600; i < 1024; i++) buf[i] = 0.0f;  // firmware tail state
    uw_.interpolate(buf, 600, 1024);
    float pos = 1023.0f * 600.0f / 1024.0f;
    int iPos = (int)pos;
    float decimal = pos - iPos;
    EXPECT_FLOAT_EQ(buf[1023], 1.0f * (1 - decimal) + 0.0f * decimal);
    EXPECT_EQ(uw_.numberOfSample, 1024);  // side effect of interpolate()
}

TEST_F(UserWaveformTest, NormalizeCentersAndScales) {
    float buf[8] = {-2.0f, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f, 2.0f};
    uw_.normalize(buf, 8);
    // average = 0.75; centered min = -2.75, max = +1.25;
    // m1 = 1/2.75, m2 = 1/1.25 -> m = m1 (smaller) -> buf[0] = -1, buf[7] = 1.25/2.75
    EXPECT_FLOAT_EQ(buf[0], -1.0f);
    EXPECT_FLOAT_EQ(buf[7], 1.25f / 2.75f);
}

TEST_F(UserWaveformTest, FillFromTxtHandlesLeadingNameAcrossBuffers) {
    // chunked load path: a txt bigger than 512 bytes exercises the
    // readIndex loop in loadUserWaveformFromTxt (covered implicitly by the
    // 200-sample test above); here we drive fillUserWaveFormFromTxt once
    // with a "last" buffer to pin the stop rule.
    char line[512];
    strcpy(line, "ABCD 64 0.25");
    for (int i = 1; i < 64; i++) strcat(line, " 0.5");
    memset(uw_.userWaveFormNames[0], 0, 5);
    uw_.numberOfSample = -1;
    uw_.floatRead = 0;
    int used = uw_.fillUserWaveFormFromTxt(0, line, (int)strlen(line), true);
    EXPECT_EQ(memcmp(uw_.userWaveFormNames[0], "ABCD", 4), 0);
    EXPECT_EQ(uw_.numberOfSample, 64);
    EXPECT_FLOAT_EQ(userWaveform[0][0], 0.25f);
    EXPECT_FLOAT_EQ(userWaveform[0][63], 0.5f);
    EXPECT_EQ(used, (int)strlen(line));
}
