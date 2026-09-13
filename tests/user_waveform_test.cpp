// Host-side coverage for firmware/Src/filesystem/UserWaveform.cpp —
// user wavetable txt/bin load + the txt->bin conversion pipeline.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim.
// Fixed behavior and remaining constraints (deferred-work.md):
//   * FIXED (6.4): interpolate() clamps iPos+1 to the populated source
//     window, so the final target sample cannot consume stale tail data.
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
        for (int i = 0; i < 6; i++) priorShapeNames_[i] = oscShapeNames[8 + i];
    }
    void TearDown() override {
        for (int i = 0; i < 6; i++) oscShapeNames[8 + i] = priorShapeNames_[i];
        delete fsu_;
    }

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
    // 8.1 (B5): v2 bin layout — magic "P3W2" @0, name[4] @4, int32 count
    // @8, float32 body @12. count and body.size() may be inconsistent on
    // purpose (the validation tests); body.size() sizes the file.
    std::vector<uint8_t> MakeV2Bin(const char* name, int32_t count, const std::vector<float>& body) {
        std::vector<uint8_t> bin(12 + body.size() * 4, 0);
        memcpy(bin.data(), USERWAVEFORM_BIN_MAGIC, 4);
        memcpy(bin.data() + 4, name, 4);
        memcpy(bin.data() + 8, &count, 4);
        if (!body.empty()) memcpy(bin.data() + 12, body.data(), body.size() * 4);
        return bin;
    }
    // pre-8.1 cache layout (name @0, int32 count @4, body @8) — no magic.
    std::vector<uint8_t> MakeLegacyBin(const char* name, int32_t count, const std::vector<float>& body) {
        std::vector<uint8_t> bin(8 + body.size() * 4, 0);
        memcpy(bin.data(), name, 4);
        memcpy(bin.data() + 4, &count, 4);
        if (!body.empty()) memcpy(bin.data() + 8, body.data(), body.size() * 4);
        return bin;
    }
    UserWaveform uw_;
    FileSystemUtils* fsu_;
    const char* priorShapeNames_[6];
};

TEST_F(UserWaveformTest, NoFilesLeavesWaveformSilent) {
    uw_.loadUserWaveforms();
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[0][s], 0.0f);
    }
}

TEST_F(UserWaveformTest, BinFileLoadsNameCountAndSamples) {
    // 8.1 v2 layout: magic "P3W2" @0, 4 name chars @4, int32 count @8,
    // float32 samples @12 (64 samples -> 268 bytes).
    std::vector<float> f(64);
    for (int i = 0; i < 64; i++) f[i] = i / 64.0f;
    std::vector<uint8_t> bin = MakeV2Bin("SIN1", 64, f);
    ASSERT_EQ(bin.size(), 12u + 64 * 4);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr1.bin", bin.data(), bin.size());

    uw_.loadUserWaveforms();
    EXPECT_EQ(uw_.userWaveFormNames[0][0], 'S');
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
    // bin cache was written in the v2 layout with the SAME values, and a
    // second load (bin path) must be byte-identical to the first (round-trip
    // golden): magic @0, name @4, count @8, body @12.
    std::vector<uint8_t> bin1;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr2.bin", bin1));
    ASSERT_EQ(bin1.size(), 12u + 64 * 4);
    EXPECT_EQ(memcmp(bin1.data(), USERWAVEFORM_BIN_MAGIC, 4), 0);
    EXPECT_EQ(memcmp(bin1.data() + 4, "RAMP", 4), 0);
    int32_t cnt = 0;
    memcpy(&cnt, bin1.data() + 8, 4);
    EXPECT_EQ(cnt, 64);
    std::vector<float> reloaded(64);
    uw_.loadUserWaveforms();  // now takes the bin path
    memcpy(reloaded.data(), userWaveform[1], 64 * 4);
    std::vector<float> saved(64);
    memcpy(saved.data(), bin1.data() + 12, 64 * 4);
    EXPECT_EQ(reloaded, saved);  // bin save->load byte-identical
}

TEST_F(UserWaveformTest, TxtIntermediateLengthIsInterpolatedUp) {
    // 200 samples (128 < 200 < 256) -> interpolate to 256, then bin-cached
    std::string txt = MakeTxt("I200", 200, 0.0f, 1.0f / 199.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr3.txt", txt.c_str());
    uw_.loadUserWaveforms();
    std::vector<uint8_t> bin;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr3.bin", bin));
    EXPECT_EQ(memcmp(bin.data(), USERWAVEFORM_BIN_MAGIC, 4), 0);
    int32_t cnt = 0;
    memcpy(&cnt, bin.data() + 8, 4);
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

TEST_F(UserWaveformTest, RejectedTxtSlotIsZeroedNotLeftGarbage) {
    // FIXED (7.7): userWaveform lives in .instruction_ram (ITCMRAM), which
    // the startup FillZerobss loop does not cover — a rejected slot used to
    // keep its power-on content and render noise (device: −45 dB). Simulate
    // that garbage: prefill, reject via declared 31 (< 32), expect zeros.
    //
    // Neighbor guard: a VALID usr2 loads first (f order) — the reject reset
    // must not clobber its ramp (a wrong-slot or off-by-one loop would).
    std::string keep = MakeTxt("KEEP", 64, 0.0f, 1.0f / 63.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr2.txt", keep.c_str());
    int priorMax = waveTables[8 + 2].max;  // reject must leave this untouched
    for (int s = 0; s < 1024; s++) userWaveform[2][s] = 42.0f;
    fatfsShimInjectString("0:/pfm3/waveform/usr3.txt", "BAD3 31\n0.5 0.5\n");
    uw_.loadUserWaveforms();
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[2][s], 0.0f) << "sample " << s;
    }
    EXPECT_EQ(oscShapeNames[8 + 2][0], '#');
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/waveform/usr3.bin"));
    EXPECT_EQ(waveTables[8 + 2].max, priorMax);
    // the valid neighbor kept its normalized ±1 ramp (bin path on reload)
    EXPECT_NEAR(userWaveform[1][0], -1.0f, 0.01f);
    EXPECT_NEAR(userWaveform[1][63], 1.0f, 0.01f);
    EXPECT_EQ(oscShapeNames[8 + 1][0], 'K');

    // Same guard for a too-large declared count (1025 > 1024).
    for (int s = 0; s < 1024; s++) userWaveform[4][s] = 42.0f;
    fatfsShimInjectString("0:/pfm3/waveform/usr5.txt", "BAD5 1025\n0.5\n");
    uw_.loadUserWaveforms();
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[4][s], 0.0f) << "sample " << s;
    }
    EXPECT_EQ(oscShapeNames[8 + 4][0], '#');
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/waveform/usr5.bin"));
    // neighbor still intact after the second (rejecting) load
    EXPECT_NEAR(userWaveform[1][63], 1.0f, 0.01f);
}

TEST_F(UserWaveformTest, Valid32CountTxtStillLoadsValues) {
    // Positive control for the 7.7 reject-path zeroing: the minimum valid
    // count (32) is NOT rejected and must still load+normalize its values
    // so the guard is not vacuous.
    std::string txt = MakeTxt("MIN3", 32, 0.0f, 1.0f / 31.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr3.txt", txt.c_str());
    uw_.loadUserWaveforms();
    // A 0..1 ramp normalizes to ±1 (avg 0.5 centered, then scaled by 2).
    EXPECT_NEAR(userWaveform[2][0], -1.0f, 0.01f);
    EXPECT_NEAR(userWaveform[2][31], 1.0f, 0.01f);
    EXPECT_EQ(oscShapeNames[8 + 2][0], 'M');
    EXPECT_EQ(waveTables[8 + 2].max, 31);
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/waveform/usr3.bin"));
}

TEST_F(UserWaveformTest, Valid1024CountUpperBoundaryLoads) {
    // Upper boundary of the 32..1024 contract (7.7 review): the inclusive
    // maximum must load, not reject — and exercises the chunked (>512 B)
    // bin save/load path.
    std::string txt = MakeTxt("MAXW", 1024, -1.0f, 2.0f / 1023.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr4.txt", txt.c_str());
    uw_.loadUserWaveforms();
    EXPECT_EQ(oscShapeNames[8 + 3][0], 'M');
    EXPECT_EQ(waveTables[8 + 3].max, 1023);
    // -1..1 ramp: avg ~0, min/max ~±1 -> normalize preserves the extremes
    EXPECT_NEAR(userWaveform[3][0], -1.0f, 0.01f);
    EXPECT_NEAR(userWaveform[3][1023], 1.0f, 0.01f);
    std::vector<uint8_t> bin;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr4.bin", bin));
    // v2 exact-chunk save: 12-byte header + 4096-byte body, NOT 512-rounded.
    EXPECT_EQ(bin.size(), 12u + 1024 * 4);
    EXPECT_EQ(memcmp(bin.data(), USERWAVEFORM_BIN_MAGIC, 4), 0);
}

TEST_F(UserWaveformTest, InterpolateLastSampleStaysInsidePopulatedWindow) {
    // FIXED (6.4): target[1023] used to interpolate buffer[599]..buffer[600]
    // where 600 == srcN (never populated by the txt parse — a stale/zero
    // tail leaked into the last sample). The upper read is now clamped to
    // the last populated sample: the final target interpolates
    // buf[599]..buf[599] == buf[599] exactly.
    float buf[1024];
    for (int i = 0; i < 600; i++) buf[i] = 1.0f;
    for (int i = 600; i < 1024; i++) buf[i] = -7.0f;  // hostile tail must NOT leak
    uw_.interpolate(buf, 600, 1024);
    EXPECT_FLOAT_EQ(buf[1023], 1.0f)
        << "last sample interpolates only populated source samples";
    // Second-to-last still two-sample interpolation (iPos 598..599).
    float pos = 1022.0f * 600.0f / 1024.0f;
    int iPos = (int)pos;
    float decimal = pos - iPos;
    EXPECT_FLOAT_EQ(buf[1022], 1.0f * (1 - decimal) + 1.0f * decimal);
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

// --- Bin-header numberOfSample validation (B1 / T1c) -----------------------
// A corrupt or stale usr#.bin header bypassed every txt-side check: the
// loader trusted numberOfSample, fed it to waveTables[].max, and took the
// chunked (>512 B) body-load path — trampling neighboring userWaveform
// slots (and, on device, past .instruction_ram). The fix rejects the bin
// exactly like a bad txt: slot zeroed, '#' name, no bin rewrite.

TEST_F(UserWaveformTest, BinHeaderSampleCount2000IsRejectedLikeBadTxt) {
    // A valid earlier slot proves the reject path is byte-exact on neighbors:
    // usr1 (f=0) loads first; the bad usr2 (f=1) must not disturb it.
    std::string keep = MakeTxt("KEEP", 64, 0.0f, 1.0f / 63.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr1.txt", keep.c_str());

    // numberOfSample=2000 + a real 2000-float body (12 + 8000-byte file) so
    // the pre-fix chunked path would engage (512-byte reads into a 4096-byte
    // slot) and spill recognizable garbage into the following slots.
    std::vector<float> big(2000);
    for (int i = 0; i < 2000; i++) big[i] = 7.0f + i;
    std::vector<uint8_t> bin = MakeV2Bin("BIG2", 2000, big);
    ASSERT_EQ(bin.size(), 12u + 2000 * 4);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr2.bin", bin.data(), bin.size());

    // Simulated power-on garbage in the rejected slot (7.7 ITCM semantics:
    // the reject path must zero it, not keep it).
    for (int s = 0; s < 1024; s++) userWaveform[1][s] = 42.0f;
    int priorMax = waveTables[8 + 1].max;

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[1][0], '#');
    EXPECT_EQ(oscShapeNames[8 + 1], uw_.userWaveFormNames[1]);
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[1][s], 0.0f) << "sample " << s;
    }
    EXPECT_EQ(waveTables[8 + 1].max, priorMax);  // table entry untouched
    // The bin is NOT rewritten by the reject path.
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr2.bin", after));
    EXPECT_EQ(after, bin);
    // Neighbor slot 0 kept its normalized ±1 ramp (loads via its own bin).
    EXPECT_NEAR(userWaveform[0][0], -1.0f, 0.01f);
    EXPECT_NEAR(userWaveform[0][63], 1.0f, 0.01f);
    EXPECT_EQ(oscShapeNames[8 + 0][0], 'K');
}

TEST_F(UserWaveformTest, BinHeaderSampleCount1024LoadsByteIdentical) {
    // Positive control at the inclusive upper boundary: a valid 1024-sample
    // bin must load byte-identically (the bin path does no normalize) — via
    // the chunked >512-byte load path — and must NOT be rejected by the new
    // validation.
    std::vector<float> wf(1024);
    for (int i = 0; i < 1024; i++) wf[i] = -1.0f + 2.0f * i / 1023.0f;
    std::vector<uint8_t> bin = MakeV2Bin("MAX3", 1024, wf);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr3.bin", bin.data(), bin.size());

    uw_.loadUserWaveforms();

    EXPECT_EQ(memcmp(userWaveform[2], wf.data(), 1024 * 4), 0);
    EXPECT_EQ(waveTables[8 + 2].max, 1023);
    EXPECT_EQ(oscShapeNames[8 + 2][0], 'M');
}

TEST_F(UserWaveformTest, BinHeaderSampleCount16IsRejected) {
    // Lower boundary violation (16 < 32): rejected like the 2000 case.
    std::vector<float> tiny(16);
    for (int i = 0; i < 16; i++) tiny[i] = 0.25f * i;
    std::vector<uint8_t> bin = MakeV2Bin("TINY", 16, tiny);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr4.bin", bin.data(), bin.size());
    for (int s = 0; s < 1024; s++) userWaveform[3][s] = 42.0f;

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[3][0], '#');
    EXPECT_EQ(oscShapeNames[8 + 3], uw_.userWaveFormNames[3]);
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[3][s], 0.0f) << "sample " << s;
    }
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr4.bin", after));
    EXPECT_EQ(after, bin);  // not rewritten
}

// --- B1 review: boundary matrix + truncated-header bypass (red→green for
// the numberOfSample = -1 pre-set) -------------------------------------------

TEST_F(UserWaveformTest, BinHeaderSampleCount31IsRejected) {
    // Inclusive lower boundary minus one: 31 < 32 must reject.
    std::vector<uint8_t> bin = MakeV2Bin("B31 ", 31, std::vector<float>());
    fatfsShimInjectBytes("0:/pfm3/waveform/usr5.bin", bin.data(), bin.size());
    for (int s = 0; s < 1024; s++) userWaveform[4][s] = 42.0f;

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[4][0], '#');
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[4][s], 0.0f) << "sample " << s;
    }
}

TEST_F(UserWaveformTest, BinHeaderSampleCount32LoadsByteIdentical) {
    // Inclusive lower boundary: 32 must load byte-identically.
    std::vector<float> wf(32);
    for (int i = 0; i < 32; i++) wf[i] = -1.0f + 2.0f * i / 31.0f;
    std::vector<uint8_t> bin = MakeV2Bin("B32 ", 32, wf);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr6.bin", bin.data(), bin.size());

    uw_.loadUserWaveforms();

    EXPECT_EQ(memcmp(userWaveform[5], wf.data(), 32 * 4), 0);
    EXPECT_EQ(waveTables[8 + 5].max, 31);
    EXPECT_EQ(oscShapeNames[8 + 5][0], 'B');
}

TEST_F(UserWaveformTest, BinHeaderSampleCount1025IsRejected) {
    // Inclusive upper boundary plus one: 1025 > 1024 must reject (body sized
    // so the chunked path would engage).
    std::vector<uint8_t> bin = MakeV2Bin("B1K+", 1025, std::vector<float>());
    fatfsShimInjectBytes("0:/pfm3/waveform/usr2.bin", bin.data(), bin.size());
    for (int s = 0; s < 1024; s++) userWaveform[1][s] = 42.0f;

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[1][0], '#');
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[1][s], 0.0f) << "sample " << s;
    }
}

TEST_F(UserWaveformTest, TruncatedBinBodyWithValidHeaderIsRejected) {
    // B1/Copilot: header count 64 (valid range) but only 20 bytes of body —
    // the old code published the valid name + max and left the slot tail as
    // power-on garbage (7.7 noise class). The declared body must be present.
    std::vector<float> shortBody(20 / 4);
    for (int i = 0; i < 20 / 4; i++) shortBody[i] = 0.5f * i;
    std::vector<uint8_t> bin = MakeV2Bin("SHRT", 64, shortBody);
    ASSERT_EQ(bin.size(), 12u + 20);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr4.bin", bin.data(), bin.size());
    for (int s = 0; s < 1024; s++) userWaveform[3][s] = 42.0f;

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[3][0], '#');
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[3][s], 0.0f) << "sample " << s;
    }
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr4.bin", after));
    EXPECT_EQ(after, bin);  // not rewritten
}

TEST_F(UserWaveformTest, TruncatedBinHeaderAfterValidSlotIsRejected) {
    // RED→GREEN for the numberOfSample = -1 pre-set (B1 review finding):
    // numberOfSample is a MEMBER carrying the previous slot's count. A v2
    // bin cut mid-count (magic + name + only the LOW 2 bytes of the count)
    // partially overwrites the stale value — without the pre-set, stale-64 +
    // 2 file bytes (0x40,0x00) reconstructs 64, passes [32,1024], and the
    // slot keeps its garbage body. With -1 pre-set, the partial write yields
    // 0xFFFF0040 (negative) → rejected like any bad header.
    std::vector<float> wf(64);
    for (int i = 0; i < 64; i++) wf[i] = -1.0f + 2.0f * i / 63.0f;
    std::vector<uint8_t> good = MakeV2Bin("KEEP", 64, wf);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr1.bin", good.data(), good.size());

    // 10-byte truncated usr2.bin: magic + full 4-byte name, then ONLY 2 of
    // the 4 count bytes — chosen so the partial write over a stale
    // 0x00000040 would re-read as exactly 64 (in-range) pre-fix.
    std::vector<uint8_t> trunc(10, 0);
    memcpy(trunc.data(), USERWAVEFORM_BIN_MAGIC, 4);
    memcpy(trunc.data() + 4, "TRUN", 4);
    trunc[8] = 0x40;
    trunc[9] = 0x00;
    fatfsShimInjectBytes("0:/pfm3/waveform/usr2.bin", trunc.data(), trunc.size());
    for (int s = 0; s < 1024; s++) userWaveform[1][s] = 42.0f;

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[1][0], '#');
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[1][s], 0.0f) << "sample " << s;
    }
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr2.bin", after));
    EXPECT_EQ(after, trunc);  // not rewritten
    // The earlier valid slot is untouched by the truncated one.
    EXPECT_EQ(memcmp(userWaveform[0], wf.data(), 64 * 4), 0);
}

// --- 8.1 (B5): truncated-txt rejection + legacy-cache invalidation ----------

TEST_F(UserWaveformTest, TruncatedTxtIsRejectedNotCachedAsPoisonedBin) {
    // A txt that DECLARES 1024 but provides only ~300 floats: the old fill
    // loop kept parsing stale bytes past the populated part of lineBuffer
    // until the declared count was reached, normalized the result, and
    // persisted it — every future boot then preferred that poisoned bin.
    // Fix: EOF/short-read/non-consumable token all route through
    // numberOfSampleError -> slot zeroed, '#' name, NO bin written.
    std::string txt = "TRUN 1024\n";
    char buf[32];
    for (int i = 0; i < 300; i++) {
        snprintf(buf, sizeof(buf), "%.3f ", -1.0f + 2.0f * i / 1023.0f);
        txt += buf;
    }
    fatfsShimInjectString("0:/pfm3/waveform/usr1.txt", txt.c_str());

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[0][0], '#');
    EXPECT_EQ(oscShapeNames[8 + 0][0], '#');
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[0][s], 0.0f) << "sample " << s;
    }
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/waveform/usr1.bin"));
    // 8.1 review: waveTables must stay untouched on reject — max used to be
    // published at count-init, before the body parsed.
    int priorMax = waveTables[8 + 0].max;
    fatfsShimInjectString("0:/pfm3/waveform/usr1.txt", "TRUN 1024\n0.5");  // still truncated
    uw_.loadUserWaveforms();
    EXPECT_EQ(waveTables[8 + 0].max, priorMax);
}

TEST_F(UserWaveformTest, OneSampleShortWithTrailingWhitespaceIsRejected) {
    // 8.1 review (B5): stof's skip phase swallows a trailing separator run
    // and returns 0.0f with a POSITIVE floatSize — a file declaring 64 but
    // providing exactly 63 floats + trailing whitespace used to fabricate
    // the final 0.0 sample, pass validation, normalize, and persist the
    // poisoned table as a v2 cache. The separator-skip-then-NUL check now
    // rejects it like any other truncation.
    std::string txt = "SHRT 64\n";
    char buf[32];
    for (int i = 0; i < 63; i++) {
        snprintf(buf, sizeof(buf), "%.3f ", i / 63.0f);
        txt += buf;
    }
    txt += "   ";  // trailing separator run where sample 64 should be
    fatfsShimInjectString("0:/pfm3/waveform/usr1.txt", txt.c_str());

    uw_.loadUserWaveforms();

    EXPECT_EQ(uw_.userWaveFormNames[0][0], '#');
    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[0][s], 0.0f) << "sample " << s;
    }
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/waveform/usr1.bin"));
}

TEST_F(UserWaveformTest, LegacyBinWithTxtIsRegeneratedAsV2) {
    // A legacy (magic-less) bin is untrustworthy — a poisoned one is
    // count/size-indistinguishable — so it is regenerated from the source
    // txt exactly once and the v2 cache replaces the same file in place.
    std::vector<float> stale(64);
    for (int i = 0; i < 64; i++) stale[i] = 0.5f;
    std::vector<uint8_t> legacy = MakeLegacyBin("OLDW", 64, stale);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr2.bin", legacy.data(), legacy.size());
    std::string txt = MakeTxt("NEW2", 64, -1.0f, 2.0f / 63.0f);
    fatfsShimInjectString("0:/pfm3/waveform/usr2.txt", txt.c_str());

    uw_.loadUserWaveforms();

    // Table equals the TXT-derived values (normalized ±1 ramp), not the
    // stale flat 0.5 body of the legacy bin.
    EXPECT_NEAR(userWaveform[1][0], -1.0f, 0.01f);
    EXPECT_NEAR(userWaveform[1][63], 1.0f, 0.01f);
    EXPECT_EQ(oscShapeNames[8 + 1][0], 'N');
    // The on-disk bin was overwritten in place and now carries the magic.
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr2.bin", after));
    EXPECT_EQ(memcmp(after.data(), USERWAVEFORM_BIN_MAGIC, 4), 0);
    int32_t cnt = 0;
    memcpy(&cnt, after.data() + 8, 4);
    EXPECT_EQ(cnt, 64);
}

TEST_F(UserWaveformTest, LegacyBinAloneIsIgnoredNotRewritten) {
    // Legacy bin with NO source txt: fall through to the no-file default
    // (zeros) and leave the old file on disk untouched (Ask-First pinned
    // policy — this intentionally stops loading valid legacy bins whose
    // txt was deleted).
    std::vector<float> stale(64);
    for (int i = 0; i < 64; i++) stale[i] = 0.5f;
    std::vector<uint8_t> legacy = MakeLegacyBin("OLDW", 64, stale);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr3.bin", legacy.data(), legacy.size());
    for (int s = 0; s < 1024; s++) userWaveform[2][s] = 42.0f;
    int priorMax = waveTables[8 + 2].max;

    uw_.loadUserWaveforms();

    for (int s = 0; s < 1024; s++) {
        EXPECT_FLOAT_EQ(userWaveform[2][s], 0.0f) << "sample " << s;
    }
    EXPECT_EQ(waveTables[8 + 2].max, priorMax);  // table entry untouched
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr3.bin", after));
    EXPECT_EQ(after, legacy);  // NOT rewritten (no v2 regeneration without txt)
}

TEST_F(UserWaveformTest, MultiChunkTxtWithBoundaryTokenStillLoads) {
    // 8.1 review: the previous version of this test checked that byte 512
    // of the FILE fell inside a token — but the loader resumes chunk 2 at
    // the 40-byte margin stop (~472), so 512 was never an actual read
    // boundary. Tokens longer than the 40-byte margin defeat the margin and
    // force the real boundary path: a token whose digits reach the NUL
    // without a closing separator is DEFERRED (fill returns the token start)
    // and re-parsed whole from the next chunk — instead of being parsed as
    // a truncated prefix plus a phantom remainder sample.
    //
    // Token shape: "0." + 34 '0' + %07d of i = 43 bytes (> margin), with a
    // tiny finite mantissa — stof accumulates the mantissa in FLOAT, so a
    // %.40f-style 40-significant-digit mantissa would overflow to +inf and
    // the normalized table to NaN. Leading zeros keep every value finite
    // and strictly increasing.
    std::string txt = "BNDY 1024\n";
    char buf[64];
    for (int i = 0; i < 1024; i++) {
        snprintf(buf, sizeof(buf), "0.0000000000000000000000000000000000%07d ", i);
        txt += buf;
    }
    ASSERT_EQ(strlen(buf) - 1, 43u);  // 43-byte tokens (excluding separator)
    fatfsShimInjectString("0:/pfm3/waveform/usr4.txt", txt.c_str());
    uw_.loadUserWaveforms();

    EXPECT_EQ(oscShapeNames[8 + 3][0], 'B');
    EXPECT_EQ(waveTables[8 + 3].max, 1023);
    // 0..1 ramp -> normalized to ±1 with the endpoints preserved; a
    // phantom/dropped sample would break the declared count (max) or the
    // endpoint exactness.
    EXPECT_NEAR(userWaveform[3][0], -1.0f, 0.02f);
    EXPECT_NEAR(userWaveform[3][1023], 1.0f, 0.02f);
    std::vector<uint8_t> bin;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/waveform/usr4.bin", bin));
    int32_t cnt = 0;
    memcpy(&cnt, bin.data() + 8, 4);
    EXPECT_EQ(cnt, 1024);
    // A strictly increasing ramp must stay monotonic after normalization:
    // a fabricated 0.0 (whitespace run) or a split token would violate it.
    for (int s = 1; s < 1024; s++) {
        EXPECT_LT(userWaveform[3][s - 1], userWaveform[3][s]) << "sample " << s;
    }
    EXPECT_TRUE(std::isfinite(userWaveform[3][512]));
}

TEST_F(UserWaveformTest, V2BinNonAlignedChunkedBodyLoadsByteIdentical) {
    // 8.1 review: the exact-final-chunk body path was only exercised with
    // 1024 samples (4096 bytes = 8 x 512 exactly). A 600-sample body is
    // 2400 bytes = 4 x 512 + 352: the shortened final read must load the
    // table byte-identically.
    std::vector<float> wf(600);
    for (int i = 0; i < 600; i++) wf[i] = -1.0f + 2.0f * i / 599.0f;
    std::vector<uint8_t> bin(12 + 600 * 4, 0);
    memcpy(bin.data(), USERWAVEFORM_BIN_MAGIC, 4);
    memcpy(bin.data() + 4, "MID6", 4);
    int32_t n = 600;
    memcpy(bin.data() + 8, &n, 4);
    memcpy(bin.data() + 12, wf.data(), wf.size() * 4);
    fatfsShimInjectBytes("0:/pfm3/waveform/usr1.bin", bin.data(), bin.size());

    uw_.loadUserWaveforms();

    EXPECT_EQ(memcmp(userWaveform[0], wf.data(), 600 * 4), 0);
    EXPECT_EQ(waveTables[8 + 0].max, 599);
    EXPECT_EQ(oscShapeNames[8 + 0][0], 'M');
}
