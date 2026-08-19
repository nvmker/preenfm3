// Host-side coverage for firmware/Src/filesystem/MixerBank.cpp —
// mixer bank save/load round-trips, bank creation, and the scala
// integration in loadMixerData.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim.
// Quirks pinned (deferred-work.md):
//   * saveDefaultMixer opens FA_OPEN_ALWAYS|FA_WRITE then lseek(0) — the
//     default file is REWRITTEN from byte 0, never truncated: a previously
//     longer file keeps its tail beyond the new mixer's bytes.
//   * loadMixerData zeroes storageBuffer then reads MIXER_SIZE bytes; a
//     SHORT bank leaves the tail zeroed — restoreFullState consumes zeros
//     (defaults), and the timbre patch reads that fall short mark "##".
#include "gtest/gtest.h"

#include "MixerBank.h"
#include "ScalaFile.h"
#include "MixerState.h"
#include "FileSystemUtils.h"
#include "Common.h"
#include "fatfs.h"

#include "fatfs.h"

#include <cstring>
#include <string>
#include <vector>

extern const struct OneSynthParams preenMainPreset;

class MixerBankTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3");
        fsu_ = new FileSystemUtils;
        bank_.setFileSystemUtils(fsu_);
        scala_.setFileSystemUtils(fsu_);
        for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
            timbres_[t] = preenMainPreset;
            memset(timbres_[t].presetName, ' ', 13);
            snprintf(timbres_[t].presetName, 13, "TIMBRE%d", t);
        }
        bank_.init(&timbres_[0], &timbres_[1], &timbres_[2],
                   &timbres_[3], &timbres_[4], &timbres_[5]);
        bank_.setMixerState(&ms_);
        bank_.setScalaFile(&scala_);
        char defaultState[PROPERTY_FILE_SIZE]{};
        uint32_t defaultStateSize = 0;
        ms_.getFullDefaultState(defaultState, &defaultStateSize, 0);
        ms_.restoreFullState(defaultState);
        strcpy(ms_.mixName_, "MYMIX       ");
        ms_.instrumentState_[0].out = 3;
        ms_.instrumentState_[0].midiChannel = 2;
        ms_.instrumentState_[3].volume = 0.75f;
    }
    void TearDown() override { delete fsu_; }
    MixerBank bank_;
    FileSystemUtils* fsu_;
    ScalaFile scala_;
    MixerState ms_;
    OneSynthParams timbres_[NUMBER_OF_TIMBRES];
};

TEST_F(MixerBankTest, DefaultMixerSaveLoadRoundTrip) {
    ASSERT_TRUE(bank_.saveDefaultMixer());
    std::vector<uint8_t> before;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mix.dfl", before));

    MixerState ms2;  // default-constructed
    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        timbres_[t].presetName[0] = 'X';  // wipe preset names
    }
    MixerBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.init(&timbres_[0], &timbres_[1], &timbres_[2],
               &timbres_[3], &timbres_[4], &timbres_[5]);
    bank2.setMixerState(&ms2);
    ASSERT_TRUE(bank2.loadDefaultMixer());

    // compare the 12-char prefix: mixName_[12] is scratch in the save/load
    // path (firmware writes 12 chars + relies on display NUL elsewhere)
    EXPECT_EQ(strncmp(ms2.mixName_, "MYMIX       ", 12), 0);
    EXPECT_EQ(ms2.instrumentState_[0].out, 3);
    EXPECT_EQ(ms2.instrumentState_[0].midiChannel, 2);
    EXPECT_FLOAT_EQ(ms2.instrumentState_[3].volume, 0.75f);
    EXPECT_STREQ(timbres_[0].presetName, "TIMBRE0");

    // A second save serializes every loaded mixer field and all six timbre
    // patches; byte equality catches corruption outside the spot checks.
    ASSERT_TRUE(bank2.saveDefaultMixer());
    std::vector<uint8_t> after;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mix.dfl", after));
    EXPECT_EQ(before, after);
}

TEST_F(MixerBankTest, SaveDefaultMixerOpenFailReturnsFalse) {
    fatfsShimReset();
    EXPECT_FALSE(bank_.saveDefaultMixer());
}

TEST_F(MixerBankTest, LoadDefaultMixerMissingFileReturnsFalse) {
    EXPECT_FALSE(bank_.loadDefaultMixer());
}

TEST_F(MixerBankTest, SaveDefaultMixerTruncatesStaleTail) {
    // Fixed (was SaveDefaultMixerRewritesInPlaceWithoutTruncation):
    // FA_CREATE_ALWAYS truncates on open, so a previously longer mix.dfl
    // can no longer keep stale bytes past the new mixer's content.
    std::vector<uint8_t> big(FULL_MIXER_SIZE + 128, 0xEE);
    fatfsShimInjectBytes("0:/pfm3/mix.dfl", big.data(), big.size());
    ASSERT_TRUE(bank_.saveDefaultMixer());
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/mix.dfl"), FULL_MIXER_SIZE);  // truncated
    std::vector<uint8_t> now;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mix.dfl", now));
    EXPECT_NE(now[FULL_MIXER_SIZE - 1], 0xEE);  // content is the saved mixer, not the seed
}

TEST_F(MixerBankTest, SaveDefaultMixerCreatesWhenAbsent) {
    // The create-or-open path must still work when mix.dfl does not exist
    // (first boot): FA_CREATE_ALWAYS creates it at exactly FULL_MIXER_SIZE.
    fatfsShimMkdir("0:/pfm3");
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/mix.dfl"));
    ASSERT_TRUE(bank_.saveDefaultMixer());
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/mix.dfl"), FULL_MIXER_SIZE);
}

TEST_F(MixerBankTest, CreateMixerBankWrites32SlotsAndLoads) {
    bank_.createMixerBank("mybank123456"); // 12 chars (addEmptyFile copies 12)
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/mybank123456"));
    EXPECT_EQ(fatfsShimFileSize("0:/pfm3/mybank123456"),
              FULL_MIXER_SIZE * NUMBER_OF_MIXERS_PER_BANK);

    PFM3File bank;
    strcpy(bank.name, "mybank123456");
    bank.fileType = FILE_OK;
    bank.version = 0;

    MixerState ms2;
    MixerBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.init(&timbres_[0], &timbres_[1], &timbres_[2],
               &timbres_[3], &timbres_[4], &timbres_[5]);
    bank2.setMixerState(&ms2);
    ASSERT_TRUE(bank2.loadMixer(&bank, 7));
    // created slots are defaults: mixName carries the mix number
    EXPECT_STRNE(ms2.mixName_, "");
}

TEST_F(MixerBankTest, SaveAndLoadMixerSlotRoundTrip) {
    bank_.createMixerBank("mybank123456"); // 12 chars (addEmptyFile copies 12)
    PFM3File bank;
    strcpy(bank.name, "mybank123456");
    bank.fileType = FILE_OK;

    strcpy(ms_.mixName_, "SLOTTHREE   "); // 12 chars: copy(12) below
    char slotName[] = "SLOTTHREE   ";
    ASSERT_TRUE(bank_.saveMixer(&bank, 3, slotName));
    EXPECT_STREQ(ms_.mixName_, "SLOTTHREE   ");
    EXPECT_STREQ(bank_.loadMixerName(&bank, 3), "SLOTTHREE   ");

    MixerState ms2;
    MixerBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.init(&timbres_[0], &timbres_[1], &timbres_[2],
               &timbres_[3], &timbres_[4], &timbres_[5]);
    bank2.setMixerState(&ms2);
    ASSERT_TRUE(bank2.loadMixer(&bank, 3));
    EXPECT_EQ(strncmp(ms2.mixName_, "SLOTTHREE   ", 12), 0);
}

TEST_F(MixerBankTest, SaveMixerMissingFileReturnsFalse) {
    PFM3File bank;
    strcpy(bank.name, "nowhere");
    bank.fileType = FILE_OK;
    char mixerName[] = "xxxxxxxxxxxx"; // 12-char: copy(12)
    EXPECT_FALSE(bank_.saveMixer(&bank, 0, mixerName));
}

TEST_F(MixerBankTest, LoadMixerMissingFileReturnsFalse) {
    PFM3File bank;
    strcpy(bank.name, "nowhere");
    bank.fileType = FILE_OK;
    EXPECT_FALSE(bank_.loadMixer(&bank, 0));
}

TEST_F(MixerBankTest, ShortBankMarksTimbrePresetNameWithHashes) {
    // Only the mixer-state block exists: every timbre patch read falls short
    // -> presetName becomes "##".
    std::vector<uint8_t> shortBank(MIXER_SIZE, 0);
    fatfsShimInjectBytes("0:/pfm3/short", shortBank.data(), shortBank.size());
    PFM3File bank;
    strcpy(bank.name, "short");
    bank.fileType = FILE_OK;
    ASSERT_TRUE(bank_.loadMixer(&bank, 0));
    EXPECT_EQ(timbres_[0].presetName[0], '#');
    EXPECT_EQ(timbres_[0].presetName[1], '#');
    EXPECT_EQ(timbres_[0].presetName[2], 0);
}

TEST_F(MixerBankTest, ScalaEnabledButUnresolvableIsDisabledOnLoad) {
    ms_.instrumentState_[0].scalaEnable = 1;
    strcpy(ms_.instrumentState_[0].scalaScaleFileName, "gone.scl");
    ASSERT_TRUE(bank_.saveDefaultMixer());
    MixerState ms2;
    ms2.instrumentState_[0].scalaEnable = 1;
    strcpy(ms2.instrumentState_[0].scalaScaleFileName, "gone.scl");
    MixerBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.init(&timbres_[0], &timbres_[1], &timbres_[2],
               &timbres_[3], &timbres_[4], &timbres_[5]);
    bank2.setMixerState(&ms2);
    bank2.setScalaFile(&scala_);
    ASSERT_TRUE(bank2.loadDefaultMixer());
    EXPECT_EQ(ms2.instrumentState_[0].scalaEnable, 0);        // disabled
    EXPECT_EQ(ms2.instrumentState_[0].scalaScaleFileName[0], 0);
    EXPECT_EQ(ms2.instrumentState_[0].scaleScaleNumber, 0);
}

TEST_F(MixerBankTest, IsCorrectFileRequiresMixAndBigSize) {
    class TestMixerBank : public MixerBank {
    public:
        bool correct(char* n, int s) { return isCorrectFile(n, s); }
    } t;
    char n1[16]; strcpy(n1, "bank.mix");
    EXPECT_TRUE(t.correct(n1, 100001));
    EXPECT_TRUE(t.correct(n1, 100000));  // exact boundary: size < 100000 excludes
    EXPECT_FALSE(t.correct(n1, 99999));   // size < 100000 excluded
    char n2[16]; strcpy(n2, "bank.bnk");
    EXPECT_FALSE(t.correct(n2, 100001));
    char n3[16]; strcpy(n3, "nodot");
    EXPECT_FALSE(t.correct(n3, 100001));
}

TEST_F(MixerBankTest, RemoveDefaultMixerDeletesFile) {
    ASSERT_TRUE(bank_.saveDefaultMixer());
    bank_.removeDefaultMixer();
    EXPECT_FALSE(fatfsShimFileExists("0:/pfm3/mix.dfl"));
}
