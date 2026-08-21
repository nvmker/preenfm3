// Host-side coverage for firmware/Src/filesystem/ScalaFile.cpp — .scl
// microtuning parsing + scale-table application.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4): asserts what the code
// DOES, including the flagged quirks (deferred-work.md):
//   * TRUNCATED-FILE DIVISION BY ZERO: with numberOfDegrees=N declared but
//     fewer interval lines present, interval[N-1] stays 0.0f -> octaveRatio=0
//     -> the fill-Cs loop computes freq[48] = 261.626/0 = +inf, and most of
//     the table collapses to 0.
//   * scalaScaleFileName=="" (v0.99) path silently copies the current file's
//     name into the mixer state instead of matching.
// FatFs goes through the in-memory shim (tests/host_shims/fatfs.h).
#include "gtest/gtest.h"

// Private member access (interval[][] / numberOfDegrees[]): the established
// scoped `#define private public` pattern (tests/midi_decoder_test.cpp
// precedent). Every header ScalaFile.h reaches is pre-included FIRST so the
// macro affects only the ScalaFile class body.
#include "Common.h"
#include "FileSystemUtils.h"
#include "fatfs.h"
#define private public
#include "ScalaFile.h"
#undef private
#include "MixerState.h"

#include "fatfs.h"

#include <cmath>
#include <cstring>

// Expose the protected surface for direct characterization.
class TestScalaFile : public ScalaFile {
public:
    using ScalaFile::isCorrectFile;
    const char* folder() { return getFolderName(); }
    float* apply(MixerState* m, int i) { return applyScalaScale(m, i); }
    float intervalValue(int inst, int i) { return interval[inst][i]; }
    uint8_t degrees(int inst) { return numberOfDegrees[inst]; }
};

// .scl layout as the parser's state machine sees it: line 1 = description
// (state 0, ignored), line 2 = degree count (state 1), then one interval
// line per degree (state 2+). '!' lines are skipped WITHOUT consuming state.
static const char* k12Tet =
    "12-tone equal temperament\n"
    "!\n"
    "12\n"
    "100.0\n200.0\n300.0\n400.0\n500.0\n600.0\n"
    "700.0\n800.0\n900.0\n1000.0\n1100.0\n"
    "2/1\n";

class ScalaFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3/scala");
        fsu_ = new FileSystemUtils;
        scala_.setFileSystemUtils(fsu_);
        ms_.instrumentState_[0].scalaEnable = 1;
        ms_.instrumentState_[0].scaleScaleNumber = 0;
        ms_.instrumentState_[0].scalaScaleFileName[0] = 0;
        ms_.instrumentState_[0].scalaMapping = SCALA_MAPPING_CONT;
    }
    void TearDown() override { delete fsu_; }
    TestScalaFile scala_;
    FileSystemUtils* fsu_;
    MixerState ms_;
};

TEST_F(ScalaFileTest, FolderIsScalaDir) {
    EXPECT_STREQ(scala_.folder(), "0:/pfm3/scala");
}

TEST_F(ScalaFileTest, DisabledScalaReturnsDiatonicTable) {
    ms_.instrumentState_[0].scalaEnable = 0;
    float* r = scala_.loadScalaScale(&ms_, 0);
    // diatonicScaleFrequency is the firmware-wide default table (waves.c)
    extern float diatonicScaleFrequency[];
    EXPECT_EQ(r, diatonicScaleFrequency);
}

TEST_F(ScalaFileTest, IsCorrectFileAcceptsSclUnder2048Bytes) {
    char name1[] = "12tet.scl";
    EXPECT_TRUE(scala_.isCorrectFile(name1, 100));
    char name2[] = "12TET.SCL";
    EXPECT_TRUE(scala_.isCorrectFile(name2, 100));
}

TEST_F(ScalaFileTest, IsCorrectFileRejectsWrongExtOrTooBig) {
    // isCorrectFile scans name[1..8] for '.' unconditionally: fixtures live
    // in 16-byte buffers (the firmware always passes FILINFO.fname[256]).
    char n1[16]; strcpy(n1, "a.mid");
    EXPECT_FALSE(scala_.isCorrectFile(n1, 100));
    char n2[16]; strcpy(n2, "a.scl");
    EXPECT_FALSE(scala_.isCorrectFile(n2, 2048));   // >= 2048 (sysex guard)
    char n3[16]; strcpy(n3, "noscldot");
    EXPECT_FALSE(scala_.isCorrectFile(n3, 100));    // no '.' in 1..8
    char n4[16]; strcpy(n4, ".sclx");
    EXPECT_FALSE(scala_.isCorrectFile(n4, 100));    // 'x' after .scl
}

TEST_F(ScalaFileTest, Load12TetFillsScaleTable) {
    fatfsShimInjectString("0:/pfm3/scala/12tet.scl", k12Tet);
    float* freq = scala_.loadScalaScale(&ms_, 0);
    ASSERT_NE(freq, nullptr);
    EXPECT_EQ(scala_.degrees(0), 12);
    EXPECT_FLOAT_EQ(freq[60], 261.626f);
    EXPECT_NEAR(freq[61], 261.626f * powf(2.0f, 1.0f / 12.0f), 0.02f);
    EXPECT_NEAR(freq[62], 261.626f * powf(2.0f, 2.0f / 12.0f), 0.02f);
    EXPECT_NEAR(freq[71], 261.626f * powf(2.0f, 11.0f / 12.0f), 0.05f);
    EXPECT_FLOAT_EQ(freq[72], 261.626f * 2.0f);  // octave ratio 2/1
    EXPECT_FLOAT_EQ(freq[72 + 1], freq[72] * scala_.intervalValue(0, 0));
}

TEST_F(ScalaFileTest, LoadUsesNumberedFileAndCopiesNameV099) {
    fatfsShimInjectString("0:/pfm3/scala/12tet.scl", k12Tet);
    float* freq = scala_.loadScalaScale(&ms_, 0);
    ASSERT_NE(freq, nullptr);
    // v0.99 path: empty scalaScaleFileName gets the enumerated name copied in
    // (initFiles only pads with '~' for DOTLESS names; "12tet.scl" keeps its
    // extension verbatim)
    EXPECT_STREQ(ms_.instrumentState_[0].scalaScaleFileName, "12tet.scl");
}

TEST_F(ScalaFileTest, LoadRemapsByNameWhenFileNameDiffers) {
    fatfsShimInjectString("0:/pfm3/scala/aaa.scl", k12Tet);
    fatfsShimInjectString("0:/pfm3/scala/zzz.scl", k12Tet);
    strcpy(ms_.instrumentState_[0].scalaScaleFileName, "zzz.scl");
    float* freq = scala_.loadScalaScale(&ms_, 0);
    ASSERT_NE(freq, nullptr);
    EXPECT_EQ(ms_.instrumentState_[0].scaleScaleNumber, 1);  // remapped
}

TEST_F(ScalaFileTest, LoadReturnsNullWhenNamedFileDisappeared) {
    fatfsShimInjectString("0:/pfm3/scala/aaa.scl", k12Tet);
    strcpy(ms_.instrumentState_[0].scalaScaleFileName, "gone.scl");
    EXPECT_EQ(scala_.loadScalaScale(&ms_, 0), nullptr);
}

TEST_F(ScalaFileTest, MissingFileReturnsNull) {
    fatfsShimInjectString("0:/pfm3/scala/other.scl", "");
    ms_.instrumentState_[0].scaleScaleNumber = 5;  // errorFile_
    EXPECT_EQ(scala_.loadScalaScale(&ms_, 0), nullptr);
}

TEST_F(ScalaFileTest, ZeroDegreesFallsBackToDiatonic) {
    // A .scl with no degree count line: numberOfDegrees stays 0 -> diatonic.
    fatfsShimInjectString("0:/pfm3/scala/empty.scl", "! only a comment\n");
    float* r = scala_.loadScalaScale(&ms_, 0);
    extern float diatonicScaleFrequency[];
    EXPECT_EQ(r, diatonicScaleFrequency);
}

TEST_F(ScalaFileTest, TruncatedFileFallsBackToDiatonic) {
    // Fixed (was TruncatedFileDividesByZeroIntervalQuirk): declares 12 degrees
    // but provides one. The parse now detects parsed < declared and falls back
    // to the diatonic table instead of dividing by a zero interval — no +inf
    // below middle C, no collapsed in-octave frequencies.
    fatfsShimInjectString("0:/pfm3/scala/trunc.scl",
                          "trunc\n12\n100.0\n");
    float* freq = scala_.loadScalaScale(&ms_, 0);
    extern float diatonicScaleFrequency[];
    EXPECT_EQ(freq, diatonicScaleFrequency);
    for (int n = 0; n < 127; n++) {  // table is float[127] (0..126)
        EXPECT_TRUE(std::isfinite(freq[n])) << "note " << n;
    }
}

TEST_F(ScalaFileTest, DegenerateFinalIntervalFallsBackToDiatonic) {
    // Review hardening (bugfix-phase1): a FULL-COUNT file whose final interval
    // is degenerate must fall back to diatonic too — the same +inf/0 collapse
    // as the truncated-file division, via a different door. Garbage ratio
    // "x/y" parses as 0/0 = NaN; "3/0" parses as +inf. (A garbage CENTS line
    // is NOT degenerate: stof quirk -> 0 cents -> ratio 1.0, a valid unison;
    // a literal "0" likewise.)
    fatfsShimInjectString(
        "0:/pfm3/scala/nan.scl",
        "nan\n3\n9/8\n5/4\nx/y\n");
    float* freq = scala_.loadScalaScale(&ms_, 0);
    extern float diatonicScaleFrequency[];
    EXPECT_EQ(freq, diatonicScaleFrequency);
    fatfsShimInjectString(
        "0:/pfm3/scala/inf.scl",
        "inf\n3\n9/8\n5/4\n3/0\n");
    freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);
    for (int n = 0; n < 127; n++) {
        EXPECT_TRUE(std::isfinite(freq[n])) << "note " << n;
    }
}

TEST_F(ScalaFileTest, DegenerateNonFinalIntervalFallsBackToDiatonic) {
    // A NON-final interval that is 0 / negative / NaN / Inf used to pass
    // validation (only the octave interval was checked) and poisoned the
    // frequency table (division collapse / NaN frequencies). Every interval
    // is now validated: each falls back to diatonic like the octave case.
    extern float diatonicScaleFrequency[];

    // interval[0] = 0.0f ("0/3")
    fatfsShimInjectString("0:/pfm3/scala/zero.scl",
                          "zero\n3\n0/3\n5/4\n2/1\n");
    float* freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);

    // interval[0] negative (negative cents -> ratio < 1 but positive is
    // actually fine, so use a negative RATIO: "-3/2" = -1.5)
    fatfsShimInjectString("0:/pfm3/scala/neg.scl",
                          "neg\n3\n-3/2\n5/4\n2/1\n");
    freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);

    // interval[0] NaN ("x/y" = 0/0)
    fatfsShimInjectString("0:/pfm3/scala/nanmid.scl",
                          "nanmid\n3\nx/y\n5/4\n2/1\n");
    freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);

    // interval[0] +inf ("3/0")
    fatfsShimInjectString("0:/pfm3/scala/infmid.scl",
                          "infmid\n3\n3/0\n5/4\n2/1\n");
    freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);
    for (int n = 0; n < 127; n++) {
        EXPECT_TRUE(std::isfinite(freq[n])) << "note " << n;
    }
}

TEST_F(ScalaFileTest, ExtremeRatioFallsBackToDiatonic) {
    // Bugfix-phase4 folded-A: an interval that is finite and > 0 but EXTREME
    // (e.g. 1e30 / 1e-30) passes interval validation, yet overflows or
    // underflows the ratio-written frequency entries at computation time.
    // Every ratio-written entry is validated when computed: any inf / 0
    // result falls back to diatonic. Deliberate 0.0f silences and
    // defaultFreq backfills are NOT ratio-written and stay valid.
    extern float diatonicScaleFrequency[];

    // octave ratio 1e30: C-fill overflows to inf
    fatfsShimInjectString("0:/pfm3/scala/huge.scl",
                          "huge\n3\n9/8\n5/4\n"
                          "1000000000000000000000000000000/1\n");
    float* freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);

    // interval 1e-30: degree-fill collapses entries to 0
    fatfsShimInjectString("0:/pfm3/scala/tiny.scl",
                          "tiny\n3\n"
                          "1/1000000000000000000000000000000\n5/4\n2/1\n");
    freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);

    // octave ratio 1e-30: C-fill underflows toward 0
    fatfsShimInjectString("0:/pfm3/scala/tinyoct.scl",
                          "tinyoct\n3\n9/8\n5/4\n"
                          "1/1000000000000000000000000000000\n");
    freq = scala_.loadScalaScale(&ms_, 0);
    EXPECT_EQ(freq, diatonicScaleFrequency);
    for (int n = 0; n < 127; n++) {
        EXPECT_TRUE(std::isfinite(freq[n])) << "note " << n;
    }
}

TEST_F(ScalaFileTest, RatioLinesParseAsFraction) {
    float f = 0; // getScalaIntervale is private; covered via full-table test
    (void)f;
    fatfsShimInjectString("0:/pfm3/scala/ratio.scl",
                          "ratio\n1\n3/2\n");
    float* freq = scala_.loadScalaScale(&ms_, 0);
    ASSERT_NE(freq, nullptr);
    EXPECT_EQ(scala_.degrees(0), 1);
    EXPECT_FLOAT_EQ(freq[61], 261.626f * 1.5f);
    EXPECT_FLOAT_EQ(freq[60 + 1], 261.626f * 1.5f);
}

TEST_F(ScalaFileTest, KeyboardMappingUsesTwelveOctaveDegrees) {
    // 7-degree scale, SCALA_MAPPING_KEYB: octaveDegree = ((7+11)/12)*12 = 12
    // (silent slots zeroed between degree 7 and the octave).
    fatfsShimInjectString(
        "0:/pfm3/scala/dia.scl",
        "dia\n7\n9/8\n5/4\n4/3\n3/2\n5/3\n15/8\n2/1\n");
    ms_.instrumentState_[0].scalaMapping = SCALA_MAPPING_KEYB;
    float* freq = scala_.loadScalaScale(&ms_, 0);
    ASSERT_NE(freq, nullptr);
    EXPECT_FLOAT_EQ(freq[72], 261.626f * 2.0f);  // octave still 2/1
    EXPECT_FLOAT_EQ(freq[67], 0.0f);             // degree 7..11 silent
    // CONT mapping instead walks by 7: octave C at 67
    ms_.instrumentState_[0].scalaMapping = SCALA_MAPPING_CONT;
    freq = scala_.apply(&ms_, 0);
    EXPECT_FLOAT_EQ(freq[67], 261.626f * 2.0f);
}

TEST_F(ScalaFileTest, ApplyDisabledReturnsDiatonic) {
    ms_.instrumentState_[0].scalaEnable = 0;
    extern float diatonicScaleFrequency[];
    EXPECT_EQ(scala_.apply(&ms_, 0), diatonicScaleFrequency);
}

TEST_F(ScalaFileTest, UpdateScaleIsANoOp) {
    scala_.updateScale(&ms_, 0);  // empty body — call for coverage
    SUCCEED();
}
