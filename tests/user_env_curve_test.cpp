// Host-side coverage for firmware/Src/filesystem/UserEnvCurve.cpp —
// user envelope-curve txt/bin load + the txt->bin pipeline.
//
// Characterization suite (spec-test-coverage-phase4). The normalize()
// inverted-ternary quirk (NaN on flat curves, never-scaled non-flat) was
// FIXED in bugfix-phase1 (item 1.3): non-finite samples are sanitized,
// finite flat curves early out untouched, and non-flat curves scale into 0..1.
// Still-pinned quirks (deferred-work.md):
//   * the txt parser requires EXACTLY 64 samples; anything else -> '#'
//     error and no load. (The dead interpolate() member — including its
//     one-past-source read — was removed in bugfix-phase5 item 5.4.)
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
    // 8.1 (B5): v2 bin layout — magic "P3C2" @0, name[4] @4, uint16 count
    // @8, float32 body @10 (count==64 -> 266 bytes). count and body.size()
    // may be inconsistent on purpose (the validation tests).
    std::vector<uint8_t> MakeV2Bin(const char* name, uint16_t count, const std::vector<float>& body) {
        std::vector<uint8_t> bin(10 + body.size() * 4, 0);
        memcpy(bin.data(), USERCURVE_BIN_MAGIC, 4);
        memcpy(bin.data() + 4, name, 4);
        memcpy(bin.data() + 8, &count, 2);
        if (!body.empty()) memcpy(bin.data() + 10, body.data(), body.size() * 4);
        return bin;
    }
    // pre-8.1 cache layout (name @0, uint16 count @4, body @6) — no magic.
    std::vector<uint8_t> MakeLegacyBin(const char* name, uint16_t count, const std::vector<float>& body) {
        std::vector<uint8_t> bin(6 + body.size() * 4, 0);
        memcpy(bin.data(), name, 4);
        memcpy(bin.data() + 4, &count, 2);
        if (!body.empty()) memcpy(bin.data() + 6, body.data(), body.size() * 4);
        return bin;
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
    // 8.1 v2 layout: magic "P3C2" @0, name[4] @4, uint16 count @8,
    // float32 body @10 (64 samples -> 266 bytes).
    std::vector<float> f(64);
    for (int i = 0; i < 64; i++) f[i] = (i * i) / (63.0f * 63.0f);
    std::vector<uint8_t> bin = MakeV2Bin("CRV1", 64, f);
    ASSERT_EQ(bin.size(), 10u + 64 * 4);
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
    ASSERT_EQ(bin.size(), 10u + 64 * 4);
    EXPECT_EQ(memcmp(bin.data(), USERCURVE_BIN_MAGIC, 4), 0);
    EXPECT_EQ(memcmp(bin.data() + 4, "EXP1", 4), 0);
    uint16_t cnt = 0;
    memcpy(&cnt, bin.data() + 8, 2);
    EXPECT_EQ(cnt, 64);

    // Clear runtime state and load again through the generated BIN path.
    // Compare every serialized float byte, not only the endpoints.
    memset(userEnvCurves[1], 0, sizeof(userEnvCurves[1]));
    uec_.loadUserEnvCurves();
    std::vector<uint8_t> loadedBytes(64 * sizeof(float));
    memcpy(loadedBytes.data(), userEnvCurves[1], loadedBytes.size());
    std::vector<uint8_t> savedBytes(bin.begin() + 10, bin.end());
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

TEST_F(UserEnvCurveTest, RejectedTxtSlotFallsBackToLinearRamp) {
    // FIXED (7.7): like userWaveform, the curve table is not zeroed at boot
    // (outside FillZerobss coverage) — a rejected slot used to keep its
    // power-on garbage. Simulate it, reject via declared 32 (!= 64), and
    // expect the no-file linear-ramp default i/64 (stock behavior),
    // mirroring the no-file branch of loadUserEnvCurves.
    //
    // Neighbor guard: a VALID flat 0.25 usr2 curve (normalize flat
    // early-out keeps its DC level) must survive the usr3 reject — a
    // wrong-slot ramp write would turn it into i/64.
    std::string keep = MakeTxt("KEEP", 64, 0.25f, 0.0f);
    fatfsShimInjectString("0:/pfm3/envcurve/usr2.txt", keep.c_str());
    for (int i = 0; i < 64; i++) userEnvCurves[2][i] = 42.0f;
    std::string txt = MakeTxt("BADC", 32, 0.0f, 1.0f / 31.0f);
    fatfsShimInjectString("0:/pfm3/envcurve/usr3.txt", txt.c_str());
    uec_.loadUserEnvCurves();
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[2][i], i / 64.0f) << "sample " << i;
    }
    EXPECT_EQ(uec_.userEnvCurveNames[2][0], '#');
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/envcurve/usr3.bin"));
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[1][i], 0.25f) << "neighbor " << i;
    }
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

// 5.4 removed the never-reachable 3 < n < 64 interpolate branch (and the
// interpolate member itself): the txt parser rejects anything but exactly
// 64 samples, so the branch could never run. TxtCountOtherThan64IsRejected
// above pins that contract; no interpolate regression test is possible.

// --- 8.1 (B5 + A4c): truncated-txt rejection, v2 bin validation, legacy ---

TEST_F(UserEnvCurveTest, TruncatedTxtIsRejectedRampHashNoBin) {
    // Declares 64 but provides ~30 floats: the old fill loop parsed stale
    // bytes past the populated part of lineBuffer until the declared count
    // was reached, normalized, and cached the poisoned curve as a bin.
    // Fix: EOF/non-consumable-token rejection via numberOfSampleError ->
    // linear ramp, '#' name, NO bin written.
    std::string txt = "TRUN 64\n";
    char buf[32];
    for (int i = 0; i < 30; i++) {
        snprintf(buf, sizeof(buf), "%.3f ", i / 63.0f);
        txt += buf;
    }
    fatfsShimInjectString("0:/pfm3/envcurve/usr1.txt", txt.c_str());

    uec_.loadUserEnvCurves();

    EXPECT_EQ(uec_.userEnvCurveNames[0][0], '#');
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[0][i], i / 64.0f) << "sample " << i;
    }
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/envcurve/usr1.bin"));
    // A4c (8.1): the '#' must reach the row list from a COLD boot too —
    // numberOfSampleError now repoints envCurveNames[3+f] like the waveform
    // twin repoints oscShapeNames[8+f]. Pre-fix the row kept showing
    // "Usr1".."Usr4" (the stub default) after a reject.
    EXPECT_EQ(envCurveNames[3 + 0], uec_.userEnvCurveNames[0]);
    EXPECT_EQ(envCurveNames[3 + 0][0], '#');
}

TEST_F(UserEnvCurveTest, V2BinCountNot64IsRejectedNoOverrun) {
    // B5 memory-overrun regression: loadUserEnvCurveFromBin used to feed the
    // raw uint16 count straight into numberOfSample*4 — a garbage count
    // multiplied into an unbounded body read past the 256-byte curve slot
    // into neighboring globals. count must be exactly 64 and the 266-byte
    // extent present before any body byte is read.
    std::vector<float> hostile(1000);
    for (int i = 0; i < 1000; i++) hostile[i] = i / 999.0f;
    std::vector<uint8_t> bin = MakeV2Bin("BAD1", 1000, hostile);
    fatfsShimInjectBytes("0:/pfm3/envcurve/usr2.bin", bin.data(), bin.size());
    for (int i = 0; i < 64; i++) userEnvCurves[1][i] = 42.0f;

    uec_.loadUserEnvCurves();

    EXPECT_EQ(uec_.userEnvCurveNames[1][0], '#');
    EXPECT_EQ(envCurveNames[3 + 1][0], '#');
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[1][i], i / 64.0f) << "sample " << i;
    }
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr2.bin", after));
    EXPECT_EQ(after, bin);  // not rewritten
}

TEST_F(UserEnvCurveTest, V2BinShortBodyIsRejected) {
    // Valid count (64) but a truncated body: reject like a bad txt — ramp,
    // '#', no rewrite. (sizeBin >= 266 must hold before the body read.)
    std::vector<float> shortBody(10);  // 40 bytes of body, header says 64
    std::vector<uint8_t> bin = MakeV2Bin("SHRT", 64, shortBody);
    ASSERT_EQ(bin.size(), 10u + 40);
    fatfsShimInjectBytes("0:/pfm3/envcurve/usr3.bin", bin.data(), bin.size());

    uec_.loadUserEnvCurves();

    EXPECT_EQ(uec_.userEnvCurveNames[2][0], '#');
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[2][i], i / 64.0f) << "sample " << i;
    }
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr3.bin", after));
    EXPECT_EQ(after, bin);
}

TEST_F(UserEnvCurveTest, LegacyBinWithTxtIsRegeneratedAsV2) {
    // Legacy (magic-less) curve cache + valid txt: regenerate from the txt,
    // overwrite the cache in place with the v2 layout.
    std::vector<float> stale(64);
    for (int i = 0; i < 64; i++) stale[i] = 0.9f;
    std::vector<uint8_t> legacy = MakeLegacyBin("OLD1", 64, stale);
    fatfsShimInjectBytes("0:/pfm3/envcurve/usr4.bin", legacy.data(), legacy.size());
    std::string txt = MakeTxt("NEW1", 64, 0.0f, 1.0f / 63.0f);
    fatfsShimInjectString("0:/pfm3/envcurve/usr4.txt", txt.c_str());

    uec_.loadUserEnvCurves();

    // Table equals the txt-derived values (0..1 ramp normalizes to itself),
    // not the stale flat 0.9 body.
    EXPECT_FLOAT_EQ(userEnvCurves[3][0], 0.0f);
    EXPECT_FLOAT_EQ(userEnvCurves[3][63], 1.0f);
    EXPECT_EQ(envCurveNames[3 + 3][0], 'N');
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr4.bin", after));
    EXPECT_EQ(memcmp(after.data(), USERCURVE_BIN_MAGIC, 4), 0);
}

TEST_F(UserEnvCurveTest, LegacyBinAloneIsRampNotRewritten) {
    // Legacy curve cache with NO source txt: no-file default (linear ramp),
    // old file left on disk untouched.
    std::vector<float> stale(64);
    for (int i = 0; i < 64; i++) stale[i] = 0.9f;
    std::vector<uint8_t> legacy = MakeLegacyBin("OLD2", 64, stale);
    fatfsShimInjectBytes("0:/pfm3/envcurve/usr1.bin", legacy.data(), legacy.size());
    for (int i = 0; i < 64; i++) userEnvCurves[0][i] = 42.0f;

    uec_.loadUserEnvCurves();

    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[0][i], i / 64.0f) << "sample " << i;
    }
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr1.bin", after));
    EXPECT_EQ(after, legacy);  // NOT rewritten
}

TEST_F(UserEnvCurveTest, GarbageFinalTokenIsRejected) {
    // Copilot review twin: a digitless final token ("abc") used to be
    // accepted as a 0.0f sample (stof reports skipped chars as consumed)
    // and the poisoned curve cached. Now rejected like any truncation.
    std::string txt = "GARB 64\n";
    char buf[32];
    for (int i = 0; i < 63; i++) {
        snprintf(buf, sizeof(buf), "%.3f ", i / 63.0f);
        txt += buf;
    }
    txt += " abc ";
    fatfsShimInjectString("0:/pfm3/envcurve/usr2.txt", txt.c_str());

    uec_.loadUserEnvCurves();

    EXPECT_EQ(uec_.userEnvCurveNames[1][0], '#');
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[1][i], i / 64.0f) << "sample " << i;
    }
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/envcurve/usr2.bin"));
}

TEST_F(UserEnvCurveTest, InterruptedInitialSaveFallsBackToTxt) {
    // Copilot review twin: a 2-byte bin (interrupted first save, no
    // committed magic) is UNCOMMITTED — the valid txt regenerates it
    // instead of a permanent '#' with the txt fallback blocked.
    fatfsShimInjectBytes("0:/pfm3/envcurve/usr1.bin", "AB", 2);
    std::string txt = MakeTxt("NEW9", 64, 0.0f, 1.0f / 63.0f);
    fatfsShimInjectString("0:/pfm3/envcurve/usr1.txt", txt.c_str());

    uec_.loadUserEnvCurves();

    EXPECT_FLOAT_EQ(userEnvCurves[0][0], 0.0f);
    EXPECT_FLOAT_EQ(userEnvCurves[0][63], 1.0f);
    EXPECT_EQ(envCurveNames[3 + 0][0], 'N');
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr1.bin", after));
    EXPECT_EQ(memcmp(after.data(), USERCURVE_BIN_MAGIC, 4), 0);
}

TEST_F(UserEnvCurveTest, TwoByteBinAloneIsUncommittedNotError) {
    // Uncommitted 2-byte bin with NO txt: no-file default (ramp), old file
    // untouched — NOT a '#' error (nothing was committed, nothing rejected).
    fatfsShimInjectBytes("0:/pfm3/envcurve/usr2.bin", "AB", 2);
    for (int i = 0; i < 64; i++) userEnvCurves[1][i] = 42.0f;

    uec_.loadUserEnvCurves();

    EXPECT_NE(uec_.userEnvCurveNames[1][0], '#');
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[1][i], i / 64.0f) << "sample " << i;
    }
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr2.bin", after));
    EXPECT_EQ(after.size(), 2u);  // not rewritten without a txt
}

TEST_F(UserEnvCurveTest, OneSampleShortWithTrailingWhitespaceIsRejected) {
    // 8.1 review (B5): stof's skip phase swallows a trailing separator run
    // and returns 0.0f with a POSITIVE floatSize — a curve txt declaring 64
    // but providing exactly 63 floats + trailing whitespace used to
    // fabricate the final 0.0 sample and cache it as a v2 bin. Rejected now.
    std::string txt = "SHR1 64\n";
    char buf[32];
    for (int i = 0; i < 63; i++) {
        snprintf(buf, sizeof(buf), "%.3f ", i / 63.0f);
        txt += buf;
    }
    txt += "  ";
    fatfsShimInjectString("0:/pfm3/envcurve/usr2.txt", txt.c_str());

    uec_.loadUserEnvCurves();

    EXPECT_EQ(uec_.userEnvCurveNames[1][0], '#');
    for (int i = 0; i < 64; i++) {
        EXPECT_FLOAT_EQ(userEnvCurves[1][i], i / 64.0f) << "sample " << i;
    }
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/envcurve/usr2.bin"));
}

TEST_F(UserEnvCurveTest, LongTokensStraddlingChunkEdgesLoadExactly) {
    // 8.1 review (B5): curve chunks are 64 bytes, so 43-byte tokens straddle
    // constantly (the header is 8 bytes; token 2 spans 54..97 across the
    // first chunk edge). A token whose digits reach the chunk NUL without a
    // closing separator is DEFERRED (re-parsed whole from the next chunk)
    // instead of split into a truncated prefix plus a phantom remainder
    // sample.
    //
    // Expected values are computed through the SAME stof parser over the
    // full token strings and the SAME normalize() — a 0..max ramp maps to
    // (v-min)/range — so the loaded table must match byte-exact. Token
    // shape keeps the mantissa tiny and finite (stof accumulates in FLOAT:
    // %.40f mantissas overflow to +inf).
    std::string txt = "LNG7 64\n";
    char buf[64];
    std::vector<std::string> tokens;
    for (int i = 0; i < 64; i++) {
        snprintf(buf, sizeof(buf), "0.0000000000000000000000000000000000%07d", i * 100);
        tokens.push_back(buf);
        txt += buf;
        txt += ' ';
    }
    ASSERT_EQ(tokens[1].size(), 43u);
    fatfsShimInjectString("0:/pfm3/envcurve/usr3.txt", txt.c_str());

    uec_.loadUserEnvCurves();

    ASSERT_NE(uec_.userEnvCurveNames[2][0], '#');
    std::vector<float> expected(64);
    for (int i = 0; i < 64; i++) {
        int consumed = 0;
        expected[i] = fsu_->stof(tokens[i].c_str(), consumed);
        ASSERT_GT(consumed, 0);
    }
    uec_.normalize(expected.data(), 64);  // same transform the loader applies
    EXPECT_EQ(memcmp(userEnvCurves[2], expected.data(), 64 * sizeof(float)), 0);
    EXPECT_EQ(envCurveNames[3 + 2][0], 'L');
    std::vector<uint8_t> bin;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/envcurve/usr3.bin", bin));
    EXPECT_EQ(bin.size(), 10u + 64 * 4);
}
