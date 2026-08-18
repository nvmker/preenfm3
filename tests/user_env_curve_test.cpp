// Host-side coverage for firmware/Src/filesystem/UserEnvCurve.cpp —
// user envelope-curve txt/bin load + the txt->bin pipeline.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). Quirks pinned
// (deferred-work.md):
//   * normalize()'s condition is INVERTED: m = (max-min)==0 ? 1/(max-min)
//     : 1. A FLAT curve (range 0) computes 1/0 = +inf and every sample
//     becomes 0*inf = NaN (the clamps don't catch NaN). A non-flat curve
//     gets m=1: shifted to 0..range but NEVER scaled into 0..1.
//   * interpolate() reads buffer[iPos+1] one past the populated source
//     window (same shape as UserWaveform).
//   * the txt parser requires EXACTLY 64 samples; anything else -> '#'
//     error and no load.
// userEnvCurves is the REAL global from Env.cpp (already linked); envCurveNames
// comes from the Phase-4 stub table.
#include "gtest/gtest.h"

#include "Common.h"
#include "FileSystemUtils.h"
#include "fatfs.h"
#define private public
#include "UserEnvCurve.h"
#undef private

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern float userEnvCurves[4][64];
extern const char* envCurveNames[];

class UserEnvCurveTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3/envcurve");
        fsu_ = new FileSystemUtils;
        uec_.setFileSystemUtils(fsu_);
        memset(userEnvCurves, 0, sizeof(userEnvCurves));
    }
    void TearDown() override { delete fsu_; }

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
    UserEnvCurve uec_;
    FileSystemUtils* fsu_;
};

TEST_F(UserEnvCurveTest, NoFilesGivesLinearRamp) {
    uec_.loadUserEnvCurves();
    EXPECT_FLOAT_EQ(userEnvCurves[0][0], 0.0f);
    EXPECT_FLOAT_EQ(userEnvCurves[0][63], 63 / 64.0f);
}

TEST_F(UserEnvCurveTest, BinFileLoadsRoundTrip) {
    std::vector<uint8_t> bin(6 + 64 * 4, 0);
    memcpy(bin.data(), "CRV1", 4);
    uint16_t n = 64;
    memcpy(bin.data() + 4, &n, 2);
    float f[64];
    for (int i = 0; i < 64; i++) f[i] = (i * i) / (63.0f * 63.0f);
    memcpy(bin.data() + 6, f, sizeof(f));
    fatfsShimInjectBytes("0:/pfm3/envcurve/usr1.bin", bin.data(), bin.size());

    uec_.loadUserEnvCurves();
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[0][i], f[i]);
    }
    EXPECT_EQ(envCurveNames[3 + 0], uec_.userEnvCurveNames[0]);
}

TEST_F(UserEnvCurveTest, Txt64SamplesInterpolatesNothingAndCachesBin) {
    std::string txt = MakeTxt("EXP1", 64, 0.0f, 1.0f / 63.0f);
    fatfsShimInjectString("0:/pfm3/envcurve/usr2.txt", txt.c_str());
    uec_.loadUserEnvCurves();
    // normalize quirk (see header): ramp 0..1, range != 0 -> m=1: shift by
    // -min(=0) only. Values stay 0..1 unscaled.
    EXPECT_FLOAT_EQ(userEnvCurves[1][0], 0.0f);
    EXPECT_FLOAT_EQ(userEnvCurves[1][63], 1.0f);
    std::vector<uint8_t> bin;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr2.bin", bin));
    ASSERT_EQ(bin.size(), 6u + 64 * 4);
    uint16_t cnt = 0;
    memcpy(&cnt, bin.data() + 4, 2);
    EXPECT_EQ(cnt, 64);
}

TEST_F(UserEnvCurveTest, TxtCountOtherThan64IsRejected) {
    // QUIRK ADJACENT: the parser requires EXACTLY 64 samples, so the
    // 3 < n < 64 interpolate branch inside loadUserEnvCurves is DEAD CODE
    // (numberOfSample can only be 64 or error when reached). 16 -> '#'.
    std::string txt = MakeTxt("I16", 16, 0.0f, 1.0f / 15.0f);
    fatfsShimInjectString("0:/pfm3/envcurve/usr3.txt", txt.c_str());
    uec_.loadUserEnvCurves();
    EXPECT_EQ(uec_.userEnvCurveNames[2][0], '#');
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/envcurve/usr3.bin"));
}

TEST_F(UserEnvCurveTest, TxtWrongSampleCountMarksErrorAndSkips) {
    fatfsShimInjectString("0:/pfm3/envcurve/usr4.txt", "BAD1 32\n0.5 0.5\n");
    uec_.loadUserEnvCurves();
    EXPECT_EQ(uec_.userEnvCurveNames[3][0], '#');
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/envcurve/usr4.bin"));
}

TEST_F(UserEnvCurveTest, AllZeroCurveNormalizesToNaNQuirk) {
    // QUIRK GOLDEN: inverted condition m = (max-min)==0 ? 1/(max-min) : 1.
    // min/max are seeded to 0 (not buffer[0]), so range==0 only for an
    // all-zero curve -> m = +inf -> every sample is 0*inf = NaN (the
    // >1 / <0 clamps do not catch NaN).
    float buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0.0f;
    uec_.normalize(buf, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_TRUE(std::isnan(buf[i])) << "sample " << i;
    }
}

TEST_F(UserEnvCurveTest, FlatNonZeroCurveIsLeftUntouchedByMinSeeding) {
    // ADJACENT QUIRK: min is seeded to 0, so a constant 0.5 curve has
    // min=0/max=0.5 -> range != 0 -> m = 1 -> values unchanged.
    float buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0.5f;
    uec_.normalize(buf, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(buf[i], 0.5f);
    }
}

TEST_F(UserEnvCurveTest, NonFlatNormalizeNeverScalesQuirk) {
    // m == 1 whenever range != 0 AND min is never negative-side seeded:
    // a 2..4 ramp is never shifted (min stays 0) and clamps to 1.
    float buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 2.0f + 2.0f * i / 63.0f;
    uec_.normalize(buf, 64);
    EXPECT_FLOAT_EQ(buf[0], 1.0f);   // raw 2.0 -> clamped
    EXPECT_FLOAT_EQ(buf[63], 1.0f);  // raw 4.0 -> clamped
    // a -1..1 ramp: min=-1, max=1, m=1 -> shifted by +1 -> 0..2, clamped 1
    for (int i = 0; i < 64; i++) buf[i] = -1.0f + 2.0f * i / 63.0f;
    uec_.normalize(buf, 64);
    EXPECT_FLOAT_EQ(buf[0], 0.0f);
    EXPECT_NEAR(buf[31], 0.984f, 0.01f);  // -1+62/63 shifted +1, unscaled
}

TEST_F(UserEnvCurveTest, InterpolateReadsOnePastPopulatedSourceQuirk) {
    float buf[64];
    for (int i = 0; i < 16; i++) buf[i] = 1.0f;
    for (int i = 16; i < 64; i++) buf[i] = 0.0f;
    uec_.interpolate(buf, 16, 64);
    // last target sample reads buf[15]..buf[16] where 16 == srcN (unpopulated)
    float pos = 63.0f * 16.0f / 64.0f;   // 15.75
    int iPos = (int)pos;
    float decimal = pos - iPos;
    EXPECT_FLOAT_EQ(buf[63], 1.0f * (1 - decimal) + 0.0f * decimal);
    EXPECT_EQ(uec_.numberOfSample, 64);
}
