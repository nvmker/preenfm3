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

// ---- 6.1: truncated/unreadable mixer-state reads reject before mutation ----

TEST_F(MixerBankTest, TruncatedMixerStateIsRejectedBeforeRestore) {
    // Regression (6.1): a bank shorter than the mixer-STATE extent fed
    // restoreFullState a short read past the populated bytes (unbounded
    // per-version readers). The f_size pre-check now rejects BEFORE any
    // mutation: load fails and the previous mixer state is untouched.
    MixerState ms2;
    strcpy(ms2.mixName_, "SENTINEL    ");
    ms2.currentChannel_ = 7;
    MixerBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.init(&timbres_[0], &timbres_[1], &timbres_[2],
               &timbres_[3], &timbres_[4], &timbres_[5]);
    bank2.setMixerState(&ms2);

    std::vector<uint8_t> truncated(MIXER_SIZE - 1, 0xAB);  // one byte short
    fatfsShimInjectBytes("0:/pfm3/truncmix", truncated.data(), truncated.size());
    PFM3File bank;
    strcpy(bank.name, "truncmix");
    bank.fileType = FILE_OK;

    EXPECT_FALSE(bank2.loadMixer(&bank, 0))
        << "truncated state extent must be rejected";
    EXPECT_EQ(strncmp(ms2.mixName_, "SENTINEL", 8), 0)
        << "mixer state unchanged on rejection";
    EXPECT_EQ(ms2.currentChannel_, 7);
    // The timbre "##" marking must NOT have run either (load aborted).
    EXPECT_EQ(timbres_[0].presetName[0], 'T');
}

TEST_F(MixerBankTest, MixerStateReadFailureRejectsBeforeRestore) {
    // Regression (6.1): the f_read result and byteRead were ignored — an
    // I/O failure consumed whatever stale bytes sat in storageBuffer.
    // (Create a full-size bank first, then fail the read.)
    bank_.createMixerBank("fullbank1234");
    PFM3File bank;
    strcpy(bank.name, "fullbank1234");  // 12 chars: name[13] fits 12+NUL
    bank.fileType = FILE_OK;

    MixerState ms2;
    strcpy(ms2.mixName_, "SENTINEL    ");
    MixerBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.init(&timbres_[0], &timbres_[1], &timbres_[2],
               &timbres_[3], &timbres_[4], &timbres_[5]);
    bank2.setMixerState(&ms2);

    fatfsShimFailNext("f_read", FR_DISK_ERR);
    EXPECT_FALSE(bank2.loadMixer(&bank, 0));
    EXPECT_EQ(strncmp(ms2.mixName_, "SENTINEL", 8), 0)
        << "state unchanged on read failure";
}

TEST_F(MixerBankTest, StateExtentButTruncatedPatchTailStillLoads) {
    // Boundary of the 6.1 pre-check: it requires offset + MIXER_SIZE ONLY
    // (not the full FULL_MIXER_SIZE slot) so a bank with a truncated patch
    // tail still restores its state and marks timbres "##" (pinned
    // ShortBankMarksTimbrePresetNameWithHashes behavior, here at slot > 0).
    std::vector<uint8_t> bankBytes(FULL_MIXER_SIZE + MIXER_SIZE, 0);
    // Slot 1 header: current version + a name at the v6 offsets.
    char* slot1 = (char*)(bankBytes.data() + FULL_MIXER_SIZE);
    slot1[0] = MIXER_BANK_CURRENT_VERSION;
    memcpy(slot1 + 1, "TAILMIX     ", 12);
    fatfsShimInjectBytes("0:/pfm3/tailmix", bankBytes.data(), bankBytes.size());
    PFM3File bank;
    strcpy(bank.name, "tailmix");
    bank.fileType = FILE_OK;

    MixerState ms2;
    MixerBank bank2;
    bank2.setFileSystemUtils(fsu_);
    bank2.init(&timbres_[0], &timbres_[1], &timbres_[2],
               &timbres_[3], &timbres_[4], &timbres_[5]);
    bank2.setMixerState(&ms2);
    ASSERT_TRUE(bank2.loadMixer(&bank, 1))
        << "state extent present: must load despite missing patch tail";
    EXPECT_EQ(strncmp(ms2.mixName_, "TAILMIX", 7), 0);
    EXPECT_EQ(timbres_[0].presetName[0], '#');
    EXPECT_EQ(timbres_[0].presetName[1], '#');
}

// ---- 6.6: bounded mixer-name copy ------------------------------------------

TEST_F(MixerBankTest, ShortMixerNameIsZeroPaddedNotOverread) {
    // Regression (6.6): saveMixer copied 12 bytes regardless of the source
    // length — bytes past the caller's NUL leaked in. The bounded copy pads
    // with zeros and terminates at [12].
    bank_.createMixerBank("namebank1234");
    PFM3File bank;
    strcpy(bank.name, "namebank1234");
    bank.fileType = FILE_OK;

    char shortName[] = "SHORT";  // 5 chars + NUL: old copy read past it
    ASSERT_TRUE(bank_.saveMixer(&bank, 0, shortName));
    EXPECT_STREQ(ms_.mixName_, "SHORT")
        << "name must stop at the source NUL";
    EXPECT_EQ(ms_.mixName_[5], 0);
    EXPECT_EQ(ms_.mixName_[11], 0);
    EXPECT_EQ(ms_.mixName_[12], 0) << "mixName_[12] always NUL";
    // And the padded name round-trips through the bank.
    EXPECT_STREQ(bank_.loadMixerName(&bank, 0), "SHORT");
    // Full-width names are still copied byte-for-byte (12 chars, no NUL).
    char fullName[] = "SLOTTHREE   ";
    ASSERT_TRUE(bank_.saveMixer(&bank, 3, fullName));
    EXPECT_EQ(strncmp(ms_.mixName_, "SLOTTHREE   ", 12), 0);
}
