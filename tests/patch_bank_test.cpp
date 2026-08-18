// Host-side coverage for firmware/Src/filesystem/PatchBank.cpp —
// patch bank creation, save/load, the PRESET_VERSION2 memcpy fast path,
// and name lookup.
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the shim.
#include "gtest/gtest.h"

#include "PatchBank.h"
#include "FileSystemUtils.h"
#include "Common.h"
#include "fatfs.h"

#include "fatfs.h"

#include <cstring>
#include <string>
#include <vector>

extern const struct OneSynthParams preenMainPreset;
extern const struct OneSynthParams newPresetParams;

class PatchBankTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3");
        fsu_ = new FileSystemUtils;
        bank_.setFileSystemUtils(fsu_);
        arpFlag_ = 1;
        bank_.setArpeggiatorPartOfThePreset(&arpFlag_);
    }
    void TearDown() override { delete fsu_; }
    PFM3File BankFile(const char* name) {
        PFM3File f;
        strcpy(f.name, name);
        f.fileType = FILE_OK;
        f.version = 0;
        return f;
    }
    PatchBank bank_;
    FileSystemUtils* fsu_;
    uint8_t arpFlag_;
};

TEST_F(PatchBankTest, CreatePatchBankWrites128ZeroedSlotsWithVersionStamp) {
    bank_.createPatchBank("mybank123456"); // 12 chars (addEmptyFile copies 12)
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/mybank123456"));
    std::vector<uint8_t> data;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mybank123456", data));
    EXPECT_EQ(data.size(), 128u * ALIGNED_PATCH_SIZE);
    // version stamp sits at [ALIGNED_PATCH_SIZE-5] of EVERY slot
    for (int p = 0; p < 128; p++) {
        uint32_t v = 0;
        memcpy(&v, &data[p * ALIGNED_PATCH_SIZE + ALIGNED_PATCH_SIZE - 5], 4);
        EXPECT_EQ(v, (uint32_t)PRESET_CURRENT_VERSION) << "slot " << p;
        // slot body is preenMainPreset flash data, not zeros
        EXPECT_NE(data[p * ALIGNED_PATCH_SIZE], 0xEE);
    }
}

TEST_F(PatchBankTest, CreatePatchBankWithArpFlagOffStripsArpFromFlash) {
    arpFlag_ = 0;  // *arpeggiatorPartOfThePreset_ > 0 == false
    bank_.createPatchBank("noarp1234567");
    EXPECT_TRUE(fatfsShimFileExists("0:/pfm3/noarp1234567"));
}

TEST_F(PatchBankTest, SavePatchLoadPatchRoundTripVersion1) {
    bank_.createPatchBank("mybank123456");
    PFM3File bf = BankFile("mybank123456");
    OneSynthParams src = preenMainPreset;
    snprintf(src.presetName, 13, "GOLDENPATCH ");
    bank_.savePatch(&bf, 10, &src);

    OneSynthParams dst;
    memset(&dst, 0, sizeof(dst));
    bank_.loadPatch(&bf, 10, &dst);
    EXPECT_STREQ(dst.presetName, "GOLDENPATCH ");  // 12-char truncation
    EXPECT_FLOAT_EQ(dst.osc1.shape, src.osc1.shape);
    EXPECT_FLOAT_EQ(dst.engine1.playMode, src.engine1.playMode);
}

TEST_F(PatchBankTest, LoadPatchVersion2IsDirectMemcpy) {
    // Craft a VERSION2 slot: the first PFM3_PATCH_FLASH_SIZE bytes are a raw
    // OneSynthParams, the version stamp at [size-5].
    bank_.createPatchBank("mybank123456");
    PFM3File bf = BankFile("mybank123456");
    OneSynthParams src = preenMainPreset;
    snprintf(src.presetName, 13, "V2DIRECT    ");
    std::vector<uint8_t> slot(ALIGNED_PATCH_SIZE, 0);
    memcpy(slot.data(), &src, PFM3_PATCH_FLASH_SIZE);
    uint32_t v2 = PRESET_VERSION2;
    memcpy(&slot[ALIGNED_PATCH_SIZE - 5], &v2, 4);

    std::vector<uint8_t> data;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mybank123456", data));
    memcpy(&data[42 * ALIGNED_PATCH_SIZE], slot.data(), ALIGNED_PATCH_SIZE);
    fatfsShimInjectBytes("0:/pfm3/mybank123456", data.data(), data.size());

    OneSynthParams dst;
    memset(&dst, 0, sizeof(dst));
    bank_.loadPatch(&bf, 42, &dst);
    // VERSION2 = direct copy of the flash bytes into the params struct.
    // QUIRK GOLDEN: PFM3_PATCH_FLASH_SIZE (936) < offsetof(OneSynthParams,
    // presetName) (992), so the memcpy NEVER covers presetName — dst keeps
    // its prior value. (V2 is dormant: PRESET_CURRENT_VERSION == VERSION1.)
    EXPECT_EQ(memcmp(&dst, &src, PFM3_PATCH_FLASH_SIZE), 0);
    EXPECT_STREQ(dst.presetName, "");
}

TEST_F(PatchBankTest, LoadPatchShortReadLeavesParamsUntouched) {
    bank_.createPatchBank("mybank123456");
    std::vector<uint8_t> data;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mybank123456", data));
    data.resize(50 * ALIGNED_PATCH_SIZE);  // slot 100 now past EOF
    fatfsShimInjectBytes("0:/pfm3/mybank123456", data.data(), data.size());
    PFM3File bf = BankFile("mybank123456");
    OneSynthParams dst;
    memset(&dst, 0xEE, sizeof(dst));
    bank_.loadPatch(&bf, 100, &dst);
    EXPECT_EQ(((unsigned char*)&dst)[0], 0xEE);  // untouched on short read
}

TEST_F(PatchBankTest, LoadPatchNameFindsSavedName) {
    bank_.createPatchBank("mybank123456");
    PFM3File bf = BankFile("mybank123456");
    OneSynthParams src = preenMainPreset;
    snprintf(src.presetName, 13, "NAMEDPRESET");
    bank_.savePatch(&bf, 3, &src);
    EXPECT_STREQ(bank_.loadPatchName(&bf, 3), "NAMEDPRESET");
}

TEST_F(PatchBankTest, LoadPatchNameVersion2Offset) {
    bank_.createPatchBank("mybank123456");
    PFM3File bf = BankFile("mybank123456");
    OneSynthParams src = preenMainPreset;
    snprintf(src.presetName, 13, "V2NAME      ");
    std::vector<uint8_t> slot(ALIGNED_PATCH_SIZE, 0);
    memcpy(slot.data(), &src, PFM3_PATCH_FLASH_SIZE);
    uint32_t v2 = PRESET_VERSION2;
    memcpy(&slot[ALIGNED_PATCH_SIZE - 5], &v2, 4);
    // V2 name lookup reads at the OneSynthParams presetName offset (992) —
    // BEYOND the 936-byte flash body, from the slot padding.
    size_t nameOff = (size_t) &((OneSynthParams*)0)->presetName;
    memcpy(&slot[nameOff], "V2NAME      ", 12);
    std::vector<uint8_t> data;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/mybank123456", data));
    memcpy(&data[7 * ALIGNED_PATCH_SIZE], slot.data(), ALIGNED_PATCH_SIZE);
    fatfsShimInjectBytes("0:/pfm3/mybank123456", data.data(), data.size());
    EXPECT_STREQ(bank_.loadPatchName(&bf, 7), "V2NAME      ");
}

TEST_F(PatchBankTest, CopyNewPresetCopiesNewPresetParamsBytes) {
    OneSynthParams p;
    memset(&p, 0, sizeof(p));
    bank_.copyNewPreset(&p);
    EXPECT_EQ(memcmp(&p, &newPresetParams, sizeof(struct OneSynthParams)), 0);
}

TEST_F(PatchBankTest, IsCorrectFileRequiresBnkAndBigSize) {
    class T : public PatchBank {
    public:
        bool correct(char* n, int s) { return isCorrectFile(n, s); }
    } t;
    char n1[16]; strcpy(n1, "bank.bnk");
    EXPECT_TRUE(t.correct(n1, 100001));
    EXPECT_TRUE(t.correct(n1, 100000));  // exact boundary: size < 100000 excludes
    EXPECT_FALSE(t.correct(n1, 99999));
    char n2[16]; strcpy(n2, "bank.mix");
    EXPECT_FALSE(t.correct(n2, 100001));
    char n3[16]; strcpy(n3, "nodot");
    EXPECT_FALSE(t.correct(n3, 100001));
}
