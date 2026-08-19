// Host-side coverage for firmware/Src/filesystem/ConfigurationFile.cpp —
// Settings.txt load/save (midiConfig table + DX7 folder-picker keys).
//
// CHARACTERIZATION suite (spec-test-coverage-phase4). FatFs via the in-memory
// shim. The midiConfig[] table is the Phase-4 STUB TABLE
// (tests/stubs/tft_display_stub.cpp): field/count parity with Menu.h, fixture
// strings — ConfigurationFile's logic is data-driven, so these tests are
// faithful to the CODE, not the shipped strings.
#include "gtest/gtest.h"

#include "Menu.h"  // MIDICONFIG_* indices + MidiConfig (stub table)
#include "ConfigurationFile.h"
#include "DX7SysexFile.h"
#include "FileSystemUtils.h"

#include "fatfs.h"

#include <cstring>
#include <string>
#include <vector>

extern const struct MidiConfig midiConfig[];

class ConfigurationFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        fatfsShimReset();
        fatfsShimMkdir("0:/pfm3");
        fsu_ = new FileSystemUtils;
        cf_.setFileSystemUtils(fsu_);
        memset(bytes_, 0, sizeof(bytes_));
    }
    void TearDown() override { delete fsu_; }
    ConfigurationFile cf_;
    FileSystemUtils* fsu_;
    uint8_t bytes_[MIDICONFIG_SIZE];
};

TEST_F(ConfigurationFileTest, LoadAppliesKnownKeysAndSkipsComments) {
    fatfsShimInjectString(
        "0:/pfm3/Settings.txt",
        "# preenfm3 settings\n"
        "usbmode=2\n"
        "encoder=1\n"
        "unknownkey=9\n"
        "reverbparams=4\n");
    cf_.loadConfig(bytes_);
    EXPECT_EQ(bytes_[MIDICONFIG_USB], 2);
    EXPECT_EQ(bytes_[MIDICONFIG_ENCODER], 1);
    EXPECT_EQ(bytes_[MIDICONFIG_REVERB_PARAMS], 4);
    EXPECT_EQ(bytes_[MIDICONFIG_TEST_VELOCITY], 0);  // untouched key stays 0
}

TEST_F(ConfigurationFileTest, LoadWithoutFileLeavesBytesUntouched) {
    bytes_[MIDICONFIG_USB] = 7;
    cf_.loadConfig(bytes_);
    EXPECT_EQ(bytes_[MIDICONFIG_USB], 7);  // checkSize -1 -> early return
}

TEST_F(ConfigurationFileTest, LoadOversizedFileIsRejected) {
    std::string big(PROPERTY_FILE_SIZE, 'x');
    fatfsShimInjectString("0:/pfm3/Settings.txt", big.c_str());
    bytes_[MIDICONFIG_USB] = 7;
    cf_.loadConfig(bytes_);
    EXPECT_EQ(bytes_[MIDICONFIG_USB], 7);  // size >= 8192 -> early return
}

TEST_F(ConfigurationFileTest, SaveWritesHeaderCommentsAndKeys) {
    for (int k = 0; k < MIDICONFIG_SIZE; k++) bytes_[k] = (uint8_t)k;
    cf_.saveConfig(bytes_);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/Settings.txt", out));
    std::string text(out.begin(), out.end());
    EXPECT_EQ(text.compare(0, 2, "# "), 0);
    EXPECT_NE(text.find("# Usb Mode\n"), std::string::npos);
    // maxValue<10 entries emit a "#   0=..." legend with maxValue names
    EXPECT_NE(text.find("#   0=Off, In\n"), std::string::npos);
    EXPECT_NE(text.find("usbmode=0\n"), std::string::npos);
    EXPECT_NE(text.find("encoder=4\n"), std::string::npos);
    // maxValue >= 10 entries have no legend. Every emitted "#   0=" legend
    // must be one of the stub table's known maxValue<10 name sequences —
    // an unknown legend (future table edit) fails here instead of hiding
    // behind a first-occurrence comparison.
    const char* kKnownLegends[] = {
        "#   0=Off, In\n",   // usbmode (maxValue 2: first 2 of Off/In/In+Out)
        "#   0=No\n",         // midireceives / programchange / arpinpreset / tftautoreinit (maxValue 1)
        "#   0=Rel\n",        // encoder (maxValue 1)
        "#   0=C2, C3\n",     // testnote (maxValue 2)
        "#   0=None\n",       // encoderpush (maxValue 1)
        "#   0=r0, r1, r2, r3, r4, r5, r6, r7, r8\n",  // reverbparams (9)
    };
    size_t pos = 0;
    int legends = 0;
    while ((pos = text.find("#   0=", pos)) != std::string::npos) {
        bool known = false;
        for (const char* lg : kKnownLegends) {
            if (text.compare(pos, strlen(lg), lg) == 0) { known = true; break; }
        }
        EXPECT_TRUE(known) << "unknown legend at offset " << pos;
        pos += 6;
        legends++;
    }
    EXPECT_GT(legends, 0);
}

TEST_F(ConfigurationFileTest, SaveLoadRoundTripIsLosslessForAllKeys) {
    uint8_t original[MIDICONFIG_SIZE];
    for (int k = 0; k < MIDICONFIG_SIZE; k++) {
        original[k] = (uint8_t)(k * 3 + 1);
        bytes_[k] = original[k];
    }
    cf_.saveConfig(bytes_);
    memset(bytes_, 0, sizeof(bytes_));
    cf_.loadConfig(bytes_);
    for (int k = 0; k < MIDICONFIG_SIZE; k++) {
        EXPECT_EQ(bytes_[k], original[k]) << "key " << k;
    }
}

TEST_F(ConfigurationFileTest, LoadFeedsDx7FolderPickerState) {
    DX7SysexFile dx7;
    cf_.setDX7SysexFile(&dx7);
    fatfsShimInjectString(
        "0:/pfm3/Settings.txt",
        "dx7bankdir=0:/pfm3/dx7lib\n"
        "dx7current=subA\n"
        "dx7bank=17\n"
        "dx7preset=99\n");
    cf_.loadConfig(bytes_);
    EXPECT_STREQ(dx7.getRoot(), "0:/pfm3/dx7lib");
    EXPECT_STREQ(dx7.getSelectedSubDir(), "subA");
    EXPECT_EQ(dx7.getLastBank(), 17);
    EXPECT_EQ(dx7.getLastPreset(), 99);
}

TEST_F(ConfigurationFileTest, SavePersistsDx7Cursor) {
    DX7SysexFile dx7;
    dx7.setLastBank(5);
    dx7.setLastPreset(31);
    cf_.setDX7SysexFile(&dx7);
    cf_.saveConfig(bytes_);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/Settings.txt", out));
    std::string text(out.begin(), out.end());
    EXPECT_NE(text.find("dx7bank=5\n"), std::string::npos);
    EXPECT_NE(text.find("dx7preset=31\n"), std::string::npos);
}

TEST_F(ConfigurationFileTest, SaveWithDx7StagesCursorBeforeRewrite) {
    DX7SysexFile dx8;
    dx8.setRoot("0:/pfm3/dx7lib");
    dx8.applySelectedSubDir("subB");
    cf_.setDX7SysexFile(&dx8);
    cf_.saveConfigWithDx7(bytes_, 21, 64);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/Settings.txt", out));
    std::string text(out.begin(), out.end());
    EXPECT_NE(text.find("dx7bankdir=0:/pfm3/dx7lib\n"), std::string::npos);
    EXPECT_NE(text.find("dx7current=subB\n"), std::string::npos);
    EXPECT_NE(text.find("dx7bank=21\n"), std::string::npos);
    EXPECT_NE(text.find("dx7preset=64\n"), std::string::npos);
    EXPECT_EQ(dx8.getLastBank(), 21);   // staged before the rewrite
    EXPECT_EQ(dx8.getLastPreset(), 64);
}

TEST_F(ConfigurationFileTest, SaveWithoutDx7OmitsDx7Keys) {
    cf_.saveConfig(bytes_);
    std::vector<uint8_t> out;
    ASSERT_TRUE(fatfsShimExtract("0:/pfm3/Settings.txt", out));
    std::string text(out.begin(), out.end());
    EXPECT_EQ(text.find("dx7bankdir="), std::string::npos);
}
