// Host-side coverage for firmware/Src/filesystem/UserEnvCurve.cpp —
// user envelope-curve txt/bin load + the txt->bin pipeline.
//
// Characterization suite (spec-test-coverage-phase4). The normalize()
// inverted-ternary quirk (NaN on flat curves, never-scaled non-flat) was
// FIXED in bugfix-phase1 (item 1.3): non-finite samples are sanitized,
// finite flat curves early out untouched, and non-flat curves scale into 0..1.
// Still-pinned quirks (deferred-work.md):
//   * interpolate() reads buffer[iPos+1] one past the populated source
//     window (same shape as UserWaveform).
//   * the txt parser requires EXACTLY 64 samples; anything else -> '#'
//     error and no load.
// userEnvCurves is the REAL global from Env.cpp (already linked); envCurveNames
// comes from the Phase-4 stub table.
// pi-lens-ignore: fatal error
#include "gtest/gtest.h"

#include "FileSystemUtils.h"
#include "fatfs.h"
#define private public
#include "UserEnvCurve.h"
#undef private

#include <cmath>
#include <cstring>
#include <limits>
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
        for (int i = 0; i < 4; i++) priorCurveNames_[i] = envCurveNames[3 + i];
    }
    void TearDown() override {
        for (int i = 0; i < 4; i++) envCurveNames[3 + i] = priorCurveNames_[i];
        delete fsu_;
    }

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
    const char* priorCurveNames_[4];
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
    // The source already spans 0..1, so normalization preserves its values.
    EXPECT_FLOAT_EQ(userEnvCurves[1][0], 0.0f);
    EXPECT_FLOAT_EQ(userEnvCurves[1][63], 1.0f);
    std::vector<uint8_t> bin;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr2.bin", bin));
    ASSERT_EQ(bin.size(), 6u + 64 * 4);
    uint16_t cnt = 0;
    memcpy(&cnt, bin.data() + 4, 2);
    EXPECT_EQ(cnt, 64);

    // Clear runtime state and load again through the generated BIN path.
    // Compare every serialized float byte, not only the endpoints.
    memset(userEnvCurves[1], 0, sizeof(userEnvCurves[1]));
    uec_.loadUserEnvCurves();
    std::vector<uint8_t> loadedBytes(64 * sizeof(float));
    memcpy(loadedBytes.data(), userEnvCurves[1], loadedBytes.size());
    std::vector<uint8_t> savedBytes(bin.begin() + 6, bin.end());
    EXPECT_EQ(loadedBytes, savedBytes);
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr2.bin", after));
    EXPECT_EQ(after, bin);
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

TEST_F(UserEnvCurveTest, FlatCurveIsLeftUntouchedNoNaN) {
    // Fixed (was AllZeroCurveNormalizesToNaNQuirk): a flat curve has no
    // shape to normalize — early-out leaves every sample finite and equal.
    // (Old behavior: inverted ternary -> m = 1/0 = +inf -> 0*inf = NaN.)
    float buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0.0f;
    uec_.normalize(buf, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(buf[i], 0.0f) << "sample " << i;
        EXPECT_FALSE(std::isnan(buf[i])) << "sample " << i;
    }
}

TEST_F(UserEnvCurveTest, FlatNonZeroCurveIsLeftUntouched) {
    // A constant 0.5 curve is flat: DC level preserved (now for the right
    // reason — flat early-out — instead of the old min-seeded-to-0 quirk
    // that coincidentally left it untouched).
    float buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0.5f;
    uec_.normalize(buf, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(buf[i], 0.5f);
        EXPECT_FALSE(std::isnan(buf[i]));
    }
}

TEST_F(UserEnvCurveTest, NonFlatNormalizeScalesIntoUnitRange) {
    // Fixed (was NonFlatNormalizeNeverScalesQuirk): min/max seed from
    // buffer[0], m = 1/(max-min) — the curve's SHAPE maps into 0..1.
    float buf[64];
    // 2..4 ramp -> 0..1 exactly
    for (int i = 0; i < 64; i++) buf[i] = 2.0f + 2.0f * i / 63.0f;
    uec_.normalize(buf, 64);
    EXPECT_FLOAT_EQ(buf[0], 0.0f);
    EXPECT_FLOAT_EQ(buf[63], 1.0f);
    EXPECT_NEAR(buf[31], 31.0f / 63.0f, 0.001f);
    // -1..1 ramp -> 0..1 exactly (negative side now reachable: min from
    // buffer[0], not seeded 0)
    for (int i = 0; i < 64; i++) buf[i] = -1.0f + 2.0f * i / 63.0f;
    uec_.normalize(buf, 64);
    EXPECT_FLOAT_EQ(buf[0], 0.0f);
    EXPECT_FLOAT_EQ(buf[63], 1.0f);
    EXPECT_NEAR(buf[31], 31.0f / 63.0f, 0.001f);
    // >1 clamps are now post-scale no-ops at the extremes only
    for (int i = 0; i < 64; i++) buf[i] = 10.0f * i / 63.0f;  // 0..10
    uec_.normalize(buf, 64);
    EXPECT_FLOAT_EQ(buf[0], 0.0f);
    EXPECT_FLOAT_EQ(buf[63], 1.0f);
    EXPECT_NEAR(buf[6], 6.0f / 63.0f, 0.002f);
}

TEST_F(UserEnvCurveTest, FlatOutOfRangeCurveIsLeftUntouched) {
    // A finite flat curve has no shape to normalize. Preserve its DC level
    // exactly, including values outside 0..1, as required by the file contract.
    float buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 2.0f;
    uec_.normalize(buf, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(buf[i], 2.0f) << "sample " << i;
    }
    for (int i = 0; i < 64; i++) buf[i] = -0.5f;
    uec_.normalize(buf, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(buf[i], -0.5f) << "sample " << i;
    }
}

TEST_F(UserEnvCurveTest, NonFiniteSamplesIncludingFirstAreSanitized) {
    // Sanitize before min/max seeding so a non-finite first sample cannot
    // poison the extrema or survive into the normalized output.
    float buf[64];
    for (int i = 0; i < 64; i++) buf[i] = i / 63.0f;
    buf[0] = std::nanf("");
    buf[1] = std::numeric_limits<float>::infinity();
    buf[31] = -std::numeric_limits<float>::infinity();
    uec_.normalize(buf, 64);
    for (int i = 0; i < 64; i++) {
        EXPECT_TRUE(std::isfinite(buf[i])) << "sample " << i;
        EXPECT_GE(buf[i], 0.0f) << "sample " << i;
        EXPECT_LE(buf[i], 1.0f) << "sample " << i;
    }
}

TEST_F(UserEnvCurveTest, OppositeFiniteExtremaNormalizeWithoutRangeOverflow) {
    float buf[] = {
        std::numeric_limits<float>::lowest(),
        0.0f,
        std::numeric_limits<float>::max(),
    };
    uec_.normalize(buf, 3);
    EXPECT_FLOAT_EQ(buf[0], 0.0f);
    EXPECT_FLOAT_EQ(buf[1], 0.5f);
    EXPECT_FLOAT_EQ(buf[2], 1.0f);
    for (float sample : buf) {
        EXPECT_TRUE(std::isfinite(sample));
    }
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
